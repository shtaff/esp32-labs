// =============================================================================
// app.cpp - the handset state machine, and the task that runs it.
//
// One task (voiceTask) owns the codec and the I2S peripheral for the whole of
// its life, and moves between five states. That is not a simplification to be
// undone later: a Codec2 instance carries mutable state and is not thread
// safe, and there is one I2S peripheral that can only point one way at a time.
// Splitting capture and playback into two tasks would mean adding a lock
// around both of those and gaining nothing, because they can never run
// together anyway.
//
//                        PTT down (radio preset)
//           IDLE ───────────────────────────────▶ TX
//            ▲ ▲                                   │
//            │ └───────────────────────────────────┘
//            │            PTT up, tail sent
//            │                                     ▲
//            │  packet arrives                     │ PTT down. Transmitting
//            ├──────────────▶ RX ──────────────────┘ always wins, exactly like
//            │                                       every other walkie-talkie
//            │  PTT down (self-test preset)
//            └──────────────▶ TEST_REC ──▶ TEST_PLAY ──┐
//            ▲                  (PTT up)   (buffer done)│
//            └───────────────────────────────────────────┘
//
// The button handler runs in buttonsTask and does almost nothing - it flips a
// setting or nudges voiceTask. The state machine reads buttonsPttHeld() rather
// than tracking the up and down events itself, so a dropped event cannot leave
// the transmitter keyed.
// =============================================================================
#include "app.h"

#include <Arduino.h>
#include <string.h>

#include "audio.h"
#include "buttons.h"
#include "codec.h"
#include "config.h"
#include "crypto.h"
#include "link.h"
#include "ui.h"

static VoiceState state = VOICE_IDLE;
static uint32_t   stateSince = 0;
static uint32_t   underruns = 0;
static TaskHandle_t voiceTaskHandle = nullptr;

// Set by the console, cleared by voiceTask when it plays the demo. A plain
// bool is enough: one writer, one reader, and a lost request would only mean
// pressing enter again.
static volatile bool cueDemoRequested = false;

// Working buffers. Static because voiceTask's stack has to hold Codec2's own
// frame of locals as well, and a few kilobytes of scratch on top of that is
// not a good use of it.
static int16_t pcm[VOICE_MAX_SAMPLES_PER_FRAME];
static uint8_t packing[VOICE_FRAMES_PER_PACKET * VOICE_MAX_BYTES_PER_FRAME];
static uint8_t frameIn[VOICE_MAX_BYTES_PER_FRAME];

// The self-test recording. Encoded frames, not samples - which is why ten
// seconds of audio fits in two kilobytes instead of 160.
static uint8_t  selfTest[VOICE_SELFTEST_MAX_FRAMES * VOICE_MAX_BYTES_PER_FRAME];
static uint16_t selfTestFrames = 0;   // how many are held
static uint16_t selfTestPlayed = 0;   // how many have been played back

VoiceState appState()        { return state; }
uint32_t   appStateSinceMs() { return stateSince; }
uint32_t   appUnderruns()    { return underruns; }

void appRequestCueDemo() {
  cueDemoRequested = true;
  // Nudge voiceTask so it acts on this now rather than on its next 30 ms tick.
  if (voiceTaskHandle) xTaskNotifyGive(voiceTaskHandle);
}

uint32_t appSelfTestRecordedMs() { return selfTestFrames * codecFrameMs(); }
uint32_t appSelfTestPlayedMs()   { return selfTestPlayed * codecFrameMs(); }

const char* appStateName() {
  switch (state) {
    case VOICE_TX:        return "TX";
    case VOICE_RX:        return "RX";
    case VOICE_TEST_REC:  return "REC";
    case VOICE_TEST_PLAY: return "PLAY";
    default:              return "IDLE";
  }
}

static void setState(VoiceState s) {
  if (s == state) return;
  state = s;
  stateSince = millis();
}

// -----------------------------------------------------------------------------
// Buttons. Runs in buttonsTask; everything here is either a scalar write or a
// call that queues work for another task, so it never blocks that task for
// long enough to delay a PTT release.
// -----------------------------------------------------------------------------
static void onButton(ButtonEvent e) {
  switch (e) {
    case BTN_PTT_DOWN:
      // Wake voiceTask now rather than letting it find out on its next tick.
      // The 30 ms it would otherwise wait is 30 ms of the first syllable.
      if (voiceTaskHandle) xTaskNotifyGive(voiceTaskHandle);
      break;

    case BTN_PTT_UP:
      // Nothing to do: every loop that cares reads buttonsPttHeld() directly.
      break;

    case BTN_MODE_SINGLE: {
      // Step to the next preset - a whole operating point, not just a
      // frequency. Both handsets stay together because both operators press
      // the button the same number of times.
      const uint8_t next = (uint8_t)((linkPresetIndex() + 1) % VOICE_PRESET_COUNT);
      linkSetPreset(next);

      // linkSetPreset() hands the change to linkTask, so linkPresetIndex()
      // has not caught up yet - report what was asked for, not what is
      // current.
      const VoicePreset& p = linkPreset(next);
      char l1[20], l2[24];
      snprintf(l1, sizeof(l1), "%s", p.label);
      if (p.kind == PRESET_RADIO) {
        snprintf(l2, sizeof(l2), "%d dBm  max %.0f%%",
                 (int)p.powerDbm, (double)p.dutyLimit);
      } else {
        snprintf(l2, sizeof(l2), "%s", p.note);
      }
      uiFlash(l1, l2, 1100);
      break;
    }

    case BTN_MODE_DOUBLE:
      if (linkSetEncryption(!linkEncryption())) {
        uiFlash(linkEncryption() ? "ENCRYPTED" : "CLEAR",
                linkEncryption() ? cryptoFingerprint() : "anyone can listen",
                1200);
      } else {
        // Refused, because there is no key. Say why - otherwise the button
        // simply appears not to work, which is a worse failure than the
        // missing key it is trying to report.
        uiFlash("NO KEY", cryptoKeyProblem(), 1800);
      }
      break;

    case BTN_MODE_LONG:
      uiNextScreen();
      break;
  }
}

// -----------------------------------------------------------------------------
// IDLE: listening. The radio is in receive, the I2S peripheral is shut down
// and the amplifier is being held silent.
// -----------------------------------------------------------------------------
static void runIdle() {
  audioSetDirection(AUDIO_DIR_OFF);

  for (;;) {
    // The console asked to hear the cue tones. Done here, in the task that
    // owns the I2S peripheral, rather than from the console's own task - see
    // appRequestCueDemo() in app.h.
    if (cueDemoRequested) {
      cueDemoRequested = false;
      audioPlayCue(AUDIO_CUE_KEYUP);    vTaskDelay(pdMS_TO_TICKS(250));
      audioPlayCue(AUDIO_CUE_INCOMING); vTaskDelay(pdMS_TO_TICKS(250));
      audioPlayCue(AUDIO_CUE_ROGER);    vTaskDelay(pdMS_TO_TICKS(250));
      audioPlayCue(AUDIO_CUE_LOST);
      // Back to silence: the cues left the peripheral pointed at the speaker.
      audioSetDirection(AUDIO_DIR_OFF);
    }

    if (buttonsPttHeld()) {
      // What PTT means depends on the preset: on a radio preset it transmits,
      // on the self-test preset it records locally.
      setState(linkSelfTestMode() ? VOICE_TEST_REC : VOICE_TX);
      return;
    }

    // Frames only appear here on a radio preset - the self-test preset leaves
    // the radio in standby, so this stays empty.
    if (linkRxFrameCount()) { setState(VOICE_RX); return; }

    // Woken immediately by a PTT press; otherwise a 30 ms tick, which is what
    // checks whether the radio task has put anything in the jitter buffer.
    // There is no cheaper way round that: the arrival of audio is the one
    // event that has to be noticed without anybody pressing anything.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30));
  }
}

// -----------------------------------------------------------------------------
// TX: PTT is held. Capture, encode, bundle, hand to the radio.
//
// The loop is paced entirely by audioCaptureFrame(), which blocks on the
// microphone DMA. That is the only clock in the transmit path - there is no
// timer and no delay anywhere in it, so the encoder runs at exactly the rate
// the microphone produces samples and cannot drift away from it.
// -----------------------------------------------------------------------------
static void runTx() {
  const int bytesPerFrame = codecBytesPerFrame();

  // Anything still buffered from an incoming transmission is now stale by
  // definition: the moment you key up you have stopped listening.
  linkFlushRx();
  audioResetLevel();

  // "You are keyed, go ahead." Local only - nothing is transmitted, and the
  // far end never hears it. It plays before the microphone opens, so it also
  // covers the settling delay: without it you talk into the dead time and lose
  // the first syllable, which is precisely the mistake a real handset's
  // key-up beep exists to prevent.
  audioPlayCue(AUDIO_CUE_KEYUP);

  audioSetDirection(AUDIO_DIR_MIC);   // blocks for the microphone settle
  linkStreamBegin();

  uint8_t packed = 0;   // frames accumulated toward the next packet

  while (buttonsPttHeld()) {
    audioCaptureFrame(pcm);
    codecEncode(packing + (size_t)packed * bytesPerFrame, pcm);
    packed++;

    if (packed >= VOICE_FRAMES_PER_PACKET) {
      linkSendFrames(packing, packed, false);
      packed = 0;
    }
  }

  // The tail. Whatever is half-packed goes out flagged as the last packet, so
  // the far end can stop immediately instead of waiting out the silence
  // timeout with the squelch open.
  //
  // If the release landed exactly on a packet boundary there is nothing to
  // send and still an end-of-stream to signal, so one frame of encoded silence
  // carries the flag. Cheaper than a separate packet type, and it gives the
  // receiver a clean tail instead of a hard cut.
  if (packed == 0) {
    memset(pcm, 0, sizeof(int16_t) * codecSamplesPerFrame());
    codecEncode(packing, pcm);
    packed = 1;
  }
  linkSendFrames(packing, packed, true);

  // Hold here until the tail is actually on the air. Dropping straight back to
  // idle would be harmless for the radio - linkTask would finish the job - but
  // it would show IDLE on the display while the PA was still running.
  linkWaitTxIdle(2000);

  setState(VOICE_IDLE);
}

// -----------------------------------------------------------------------------
// RX: a transmission is arriving. Wait for the pre-roll, then decode and play
// until it stops.
//
// Playback is paced by i2s_write() blocking on a full DMA ring - again, no
// timer. The pre-roll is what stops that pacing from being interrupted: it
// puts VOICE_PREROLL_MS of audio in the jitter buffer before the first sample
// is played, so a packet that arrives late is absorbed by the buffer instead
// of being heard as a hole.
// -----------------------------------------------------------------------------
static void runRx() {
  const uint32_t frameMs = codecFrameMs();
  // Round up: a pre-roll that is one frame short of the intended figure is
  // worse than one frame over.
  const uint16_t preroll = (uint16_t)((VOICE_PREROLL_MS + frameMs - 1) / frameMs);

  audioResetLevel();
  audioSetDirection(AUDIO_DIR_SPK);

  // "Someone is transmitting." This lands inside the pre-roll wait below,
  // which is dead time that already existed, so the cue is genuinely free - it
  // delays the audio by nothing at all.
  audioPlayCue(AUDIO_CUE_INCOMING);

  // Fill the jitter buffer. Bail out early if the far end has already stopped
  // - a very short transmission may be over before the pre-roll is met, and
  // waiting for audio that will never arrive would clip it entirely.
  const uint32_t prerollDeadline = millis() + VOICE_PREROLL_MS * 3;
  while (linkRxFrameCount() < preroll) {
    if (buttonsPttHeld())            { setState(VOICE_TX); return; }
    if (linkRxStreamEnded())         break;
    if ((int32_t)(millis() - prerollDeadline) >= 0) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // How the transmission ended decides which cue closes it, and the two mean
  // genuinely different things - see the cue-tone section of config.h.
  bool signedOff = false;

  for (;;) {
    // PTT wins. No closing cue: you interrupted them, they did not finish, and
    // a roger beep would be claiming otherwise.
    if (buttonsPttHeld()) { setState(VOICE_TX); return; }

    if (linkTakeRxFrame(frameIn, frameMs * 2)) {
      codecDecode(pcm, frameIn);
      audioPlayFrame(pcm);
      continue;
    }

    // Nothing in the buffer. Either they signed off, or they fell off the air.
    if (linkRxStreamEnded() && linkRxFrameCount() == 0) { signedOff = true; break; }
    if (millis() - linkLastRxMs() > VOICE_RX_STREAM_TIMEOUT_MS) break;

    // The stream is still live but a packet is late. Conceal with silence:
    // Codec2 has no packet loss concealment, and repeating the last frame -
    // which is the other cheap option - turns a dropout into a stutter that
    // sounds like a fault in the audio rather than a gap in the radio link.
    underruns++;
    memset(pcm, 0, sizeof(int16_t) * codecSamplesPerFrame());
    audioPlayFrame(pcm);
  }

  // The whole reason the roger beep is generated here rather than transmitted:
  // a beep that travelled over the air would be missing in exactly the case
  // that most needs marking. Deciding locally means "they finished" and "they
  // vanished" get different sounds instead of one sound and silence.
  audioPlayCue(signedOff ? AUDIO_CUE_ROGER : AUDIO_CUE_LOST);

  setState(VOICE_IDLE);
}

// =============================================================================
// SELF TEST (preset 7): record while PTT is held, play it back on release.
//
// The radio is in standby throughout. What this exercises is everything else:
// the microphone, I2S capture, the Codec2 encoder, the Codec2 decoder, I2S
// playback, and the direction switch between capture and playback - which on
// this rig is a full driver reinstall and therefore the most fragile part.
//
// It is the test you want on a board you have just soldered, because it splits
// the problem in half. Speech that comes back recognisable here proves the
// whole audio chain, so if the far end still hears nothing the fault is in the
// radio. Speech that comes back as noise proves the opposite.
//
// It works with no microphone (it records the synthetic test signal, which is
// then a pure amplifier test) and with no amplifier (nothing is audible, but
// the VU bar still moves during playback).
//
// Recording only exists because the rig is half duplex: with capture and
// playback separated in time there is no conflict over the one I2S peripheral.
//
// Live monitoring of your own voice - sidetone - would need both at once, which
// means two I2S peripherals and six pins. That does fit, but only by spending
// GPIO25's status LED on an I2S data line, or by adding an external pull-up to
// put a button on an input-only pin. It is still the wrong thing to build: an
// open microphone and a live speaker five centimetres apart is an acoustic
// feedback loop, and half duplex is precisely what stops it howling. Real
// handsets only get away with sidetone through an earpiece.
// =============================================================================
static void runSelfTestRecord() {
  const int bytesPerFrame = codecBytesPerFrame();

  // How many frames fit in VOICE_SELFTEST_MS at the active codec's frame rate,
  // clamped to the buffer, which is sized for the shortest frame period of any
  // supported mode.
  uint16_t cap = (uint16_t)(VOICE_SELFTEST_MS / codecFrameMs());
  if (cap > VOICE_SELFTEST_MAX_FRAMES) cap = VOICE_SELFTEST_MAX_FRAMES;

  selfTestFrames = 0;
  selfTestPlayed = 0;
  audioResetLevel();

  // The key-up cue earns its keep twice over here: it marks the start of the
  // recording, and because it is synthesised rather than recorded it proves
  // the amplifier is alive before you have committed to saying anything. If
  // you hear this and then hear nothing played back, the fault is the
  // microphone, not the speaker - which halves the search straight away.
  audioPlayCue(AUDIO_CUE_KEYUP);

  audioSetDirection(AUDIO_DIR_MIC);

  while (buttonsPttHeld() && selfTestFrames < cap) {
    audioCaptureFrame(pcm);
    codecEncode(selfTest + (size_t)selfTestFrames * bytesPerFrame, pcm);
    selfTestFrames++;
  }

  setState(VOICE_TEST_PLAY);
}

static void runSelfTestPlay() {
  const int bytesPerFrame = codecBytesPerFrame();

  audioResetLevel();
  audioSetDirection(AUDIO_DIR_SPK);

  for (selfTestPlayed = 0; selfTestPlayed < selfTestFrames; selfTestPlayed++) {
    // Pressing PTT again abandons the playback and starts a new recording.
    // Without this you would have to sit through ten seconds of your own voice
    // before you could try again.
    if (buttonsPttHeld()) { setState(VOICE_TEST_REC); return; }

    codecDecode(pcm, selfTest + (size_t)selfTestPlayed * bytesPerFrame);
    audioPlayFrame(pcm);   // blocks on the DMA ring, which is the pacing
  }

  // Marks the end of the recording. Without it there is no way to tell "the
  // playback finished" from "the playback stopped working half way through".
  audioPlayCue(AUDIO_CUE_ROGER);

  setState(VOICE_IDLE);
}

static void voiceTask(void* arg) {
  (void)arg;
  for (;;) {
    switch (state) {
      case VOICE_TX:        runTx();             break;
      case VOICE_RX:        runRx();             break;
      case VOICE_TEST_REC:  runSelfTestRecord(); break;
      case VOICE_TEST_PLAY: runSelfTestPlay();   break;
      default:              runIdle();           break;
    }
  }
}

void appBegin() {
  stateSince = millis();

  // Core 1, away from the radio, the buttons and the display on core 0.
  //
  // The stack is large because Codec2 is: the encoder's FFT working set alone
  // is a few kilobytes of stack per call, and the failure mode of getting this
  // wrong is a stack overflow inside a DSP routine, which lands as a corrupted
  // backtrace pointing at nothing useful. There is a quarter of a megabyte of
  // heap spare on this board, so being generous costs nothing that matters.
  if (xTaskCreatePinnedToCore(voiceTask, "voice", 28672, nullptr, 5,
                              &voiceTaskHandle, 1) != pdPASS) {
    Serial.println("[app] failed to start voiceTask");
    return;
  }

  buttonsBegin(onButton);

#ifdef VOICE_ENCRYPT_ON_BOOT
  linkSetEncryption(true);
#endif

  Serial.println("[app] ready");
  Serial.printf("[app] station %u; hold PTT (GPIO%d) to talk; MODE (GPIO%d): "
                "press = preset, double = encryption, hold = screen\n",
                (unsigned)linkStationId(), PIN_BTN_PTT, PIN_BTN_MODE);
}
