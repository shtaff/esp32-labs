// =============================================================================
// console.cpp - the serial console, run from the Arduino loop task.
//
// Every button action is reachable from here. That is not a convenience: it is
// what lets the firmware be brought up, and the radio link proved, on a board
// that has nothing soldered to it yet - which is exactly the state a new lab
// board is in, and exactly when you most want to know whether the radio works.
//
// It runs at the lowest priority in the system and never touches the radio,
// the codec or the I2S peripheral directly. `ptt` injects a button event and
// lets the same state machine handle it; `ch` and `enc` go through the same
// entry points the buttons use.
// =============================================================================
#include "console.h"

#include <Arduino.h>
#include <string.h>

#include "app.h"
#include "audio.h"
#include "buttons.h"
#include "codec.h"
#include "config.h"
#include "crypto.h"
#include "link.h"
#include "ui.h"

static char line[64];
static uint8_t fill = 0;

// A scripted PTT press has to release itself. Holding the loop task in a
// delay() would work and would also stop the console answering for the
// duration, which is the one time you might want to type something.
static uint32_t pttReleaseAt = 0;

static void printHelp() {
  Serial.println();
  Serial.println("  help              this list");
  Serial.println("  stat              everything the display screens show");
  Serial.println("  presets           list the preset table");
  Serial.println("  preset [0-7]      show or set the preset (alias: ch)");
  Serial.println("  enc [on|off]      show or set encryption");
  Serial.println("  screen [0-3]      show or set the display screen");
  Serial.println("  ptt [ms]          key up for ms (default 3000), then release");
  Serial.println("  tone [on|off]     force the synthetic test signal");
  Serial.println("  beep [on|off]     cue tones at the edges of a transmission");
  Serial.println("  reboot            restart");
  Serial.println();
  Serial.println("  On the self-test preset, `ptt` records and plays back locally");
  Serial.println("  instead of transmitting - the radio stays in standby.");
  Serial.println();
}

// The preset table, with the duty arithmetic worked out per row so you can see
// which ones are defensible without doing it in your head.
static void printPresets() {
  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;

  Serial.println();
  Serial.println("  #  preset        freq      pwr   limit   keyed   verdict");
  Serial.println("  -  ------------  --------  ----  ------  ------  ------------------");
  for (uint8_t i = 0; i < VOICE_PRESET_COUNT; i++) {
    const VoicePreset& p = linkPreset(i);
    const char* marker = (i == linkPresetIndex()) ? "*" : " ";

    if (p.kind != PRESET_RADIO) {
      Serial.printf(" %s%u  %-12s  %-8s  %-4s  %-6s  %-6s  %s\n",
                    marker, (unsigned)i, p.label, "-", "-", "-", "-", p.note);
      continue;
    }

    // "keyed" is what the duty cycle would be if you never let go of PTT. It
    // depends on the codec mode as well as the modem, so it is computed rather
    // than baked into the table.
    //
    // Only the ACTIVE row's airtime is measured - that is the one RadioLib was
    // asked about. The others are scaled from it by the roughly ten-to-one
    // ratio between LoRa SF7 and FSK 50k for this payload size. Getting exact
    // figures for every row would mean retuning the radio eight times just to
    // print a table.
    //
    // If the self-test preset is active there is no measured airtime at all,
    // so there is nothing to scale from and the row says so rather than
    // reporting a confident zero.
    const uint32_t active = linkAirtimeMs();
    if (active == 0) {
      Serial.printf(" %s%u  %-12s  %7.3f   %2d    %5.1f%%      ?   %s\n",
                    marker, (unsigned)i, p.label, (double)p.freqMHz,
                    (int)p.powerDbm, (double)p.dutyLimit, p.note);
      continue;
    }

    const uint32_t est = (p.modem == linkCurrentPreset().modem)
                       ? active
                       : (p.modem == MODEM_FSK ? active / 10 : active * 10);
    const uint32_t keyed = audioMs ? est * 100UL / audioMs : 0;

    const char* verdict = (keyed <= p.dutyLimit) ? "compliant"
                        : (keyed <= p.dutyLimit * 5) ? "over the limit"
                        : "far over the limit";

    Serial.printf(" %s%u  %-12s  %7.3f   %2d    %5.1f%%  %4lu%%   %s (%s)\n",
                  marker, (unsigned)i, p.label, (double)p.freqMHz,
                  (int)p.powerDbm, (double)p.dutyLimit,
                  (unsigned long)keyed, verdict, p.note);
  }
  Serial.println();
  Serial.println("  keyed = duty cycle while PTT is held; estimated for inactive rows.");
  Serial.println();
}

static void printStat() {
  const LinkStats& s = linkStats();
  const uint32_t up = millis() / 1000UL;
  const uint32_t budget = codecFrameMs() * 1000UL;
  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;

  const VoicePreset& p = linkCurrentPreset();

  Serial.println();
  Serial.printf("station   %u  (this board)\n", (unsigned)linkStationId());
  Serial.printf("state     %s for %lus\n", appStateName(),
                (unsigned long)((millis() - appStateSinceMs()) / 1000UL));
  if (p.kind != PRESET_RADIO) {
    Serial.printf("preset    %u = %s - %s; the radio is in standby\n",
                  (unsigned)linkPresetIndex(), p.label, p.note);
  } else if (p.modem == MODEM_LORA) {
    Serial.printf("preset    %u = %s, %.3f MHz, SF%d BW%.0f CR4/%d, %d dBm\n",
                  (unsigned)linkPresetIndex(), p.label, (double)p.freqMHz,
                  (int)VOICE_SF, (double)VOICE_BW_KHZ, (int)VOICE_CR,
                  (int)p.powerDbm);
  } else {
    Serial.printf("preset    %u = %s, %.3f MHz, FSK %.0f kbps dev %.0f kHz "
                  "rxbw %.1f kHz, %d dBm\n",
                  (unsigned)linkPresetIndex(), p.label, (double)p.freqMHz,
                  (double)VOICE_FSK_BR_KBPS, (double)VOICE_FSK_FDEV_KHZ,
                  (double)VOICE_FSK_RXBW_KHZ, (int)p.powerDbm);
  }
  Serial.printf("crypto    %s, key %s%s\n",
                linkEncryption() ? "ARMED" : "off",
                cryptoFingerprint(),
                cryptoKeyAvailable() ? "" : " (no key configured)");
  Serial.printf("codec     Codec2 %s, %d samples / %lums / %d bytes, %u per packet\n",
                codecModeName(), codecSamplesPerFrame(),
                (unsigned long)codecFrameMs(), codecBytesPerFrame(),
                (unsigned)VOICE_FRAMES_PER_PACKET);
  Serial.printf("cpu       encode %lu us (peak %lu) = %lu%% of the %lums frame\n",
                (unsigned long)codecEncodeUs(), (unsigned long)codecEncodePeakUs(),
                (unsigned long)(budget ? codecEncodeUs() * 100UL / budget : 0),
                (unsigned long)codecFrameMs());
  Serial.printf("          decode %lu us (peak %lu)\n",
                (unsigned long)codecDecodeUs(), (unsigned long)codecDecodePeakUs());
  Serial.printf("airtime   %lu ms per packet for %lu ms of audio = %u%% while keyed\n",
                (unsigned long)linkAirtimeMs(), (unsigned long)audioMs,
                (unsigned)linkKeyedDutyPercent());
  if (p.kind == PRESET_RADIO) {
    // The measured rolling hour, the limit for THIS sub-band, and what is left
    // expressed as talking time - which is the form you can actually use.
    Serial.printf("duty      %.3f%% of the last hour, limit %.0f%% for this "
                  "sub-band, %lus of talking left\n",
                  (double)linkDutyPercent(), (double)linkDutyLimit(),
                  (unsigned long)linkTalkSecondsLeft());
  }
  Serial.printf("packets   tx %lu  rx %lu  lost %lu  dropped %lu\n",
                (unsigned long)s.packetsTx, (unsigned long)s.packetsRx,
                (unsigned long)s.rxLost, (unsigned long)s.txDropped);
  Serial.printf("rejects   crc/read %lu  foreign %lu  codec-mismatch %lu  malformed %lu\n",
                (unsigned long)s.rxErrors, (unsigned long)s.rxForeign,
                (unsigned long)s.rxCodecMismatch, (unsigned long)s.rxMalformed);
  if (s.rxNoKey > 0) {
    Serial.printf("          %lu encrypted packets dropped - the other handset "
                  "is armed and this one has no key\n", (unsigned long)s.rxNoKey);
  }
  if (s.packetsRx == 0) {
    Serial.println("signal    nothing received yet");
  } else if (p.modem == MODEM_LORA) {
    Serial.printf("signal    %.1f dBm, SNR %.1f dB, from station %u, "
                  "last packet %lus ago%s\n",
                  (double)s.lastRssi, (double)s.lastSnr,
                  (unsigned)s.lastStation,
                  (unsigned long)((millis() - linkLastRxMs()) / 1000UL),
                  linkRxWasEncrypted() ? ", encrypted" : "");
  } else {
    // No SNR estimator in FSK mode on this chip.
    Serial.printf("signal    %.1f dBm, SNR n/a (FSK), from station %u, "
                  "last packet %lus ago%s\n",
                  (double)s.lastRssi, (unsigned)s.lastStation,
                  (unsigned long)((millis() - linkLastRxMs()) / 1000UL),
                  linkRxWasEncrypted() ? ", encrypted" : "");
  }
  if (audioMicPresent()) {
    Serial.printf("mic       INMP441 in I2S slot %u, %u%% confidence%s\n",
                  (unsigned)audioMicSlot(), (unsigned)audioMicConfidence(),
                  audioTestSignalActive() ? ", OVERRIDDEN by test signal" : "");
    // The capture rate is not the codec rate, and that difference is the whole
    // reason speech comes back clean rather than muffled - see config.h.
    Serial.printf("          captured at %lu Hz (%ux oversampled), averaged "
                  "down to %d Hz for the codec\n",
                  (unsigned long)audioCaptureRate(),
                  (unsigned)VOICE_MIC_OVERSAMPLE, VOICE_SAMPLE_RATE);
  } else {
    Serial.println("mic       none - PTT sends the synthetic test signal");
  }
  Serial.printf("cues      %s - roger beep is generated here, never transmitted\n",
                audioCuesEnabled() ? "on" : "off");
  Serial.printf("amp       %s\n", audioAmpEnabled()
                ? "MAX98357A assumed present (it has no readback)"
                : "disabled at build time (-DNO_AMP)");
  Serial.printf("audio     underruns %lu, capture overruns %lu, level %u%%\n",
                (unsigned long)appUnderruns(),
                (unsigned long)audioCaptureTimeouts(), (unsigned)audioLevel());
  // Clipping is the difference between a broken microphone and a loud one.
  Serial.printf("          %lu clipped samples%s\n",
                (unsigned long)audioClipCount(),
                audioClipCount()
                  ? "  <- lower VOICE_MIC_GAIN_SHIFT, or back off the mic"
                  : "");
  // Stack headroom for every task, worst case since boot.
  //
  // This is here because the first thing this firmware ever did on real
  // hardware was overflow a stack: codec2_create() puts 8 kB of FFT working
  // set in a single frame, and Arduino's loop task has 8 kB in total. The
  // failure mode is a reset with a corrupted backtrace, which tells you
  // nothing at all - whereas a margin that is shrinking tells you everything.
  //
  // uxTaskGetStackHighWaterMark() reports the MINIMUM free the task has ever
  // had, not a snapshot, so a healthy number here is a real guarantee. On
  // ESP-IDF it is in bytes, unlike vanilla FreeRTOS where it is words.
  Serial.print("stacks    ");
  static const char* const TASKS[] = { "loopTask", "voice", "link", "btn", "ui" };
  for (unsigned i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
    TaskHandle_t h = xTaskGetHandle(TASKS[i]);
    if (h) Serial.printf("%s %u  ", TASKS[i],
                         (unsigned)uxTaskGetStackHighWaterMark(h));
  }
  Serial.println("bytes never used");

  Serial.printf("system    up %luh%02lum%02lus, heap %lu bytes, display %s\n",
                (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
                (unsigned long)(up % 60), (unsigned long)ESP.getFreeHeap(),
                uiPresent() ? "present" : "absent");
  Serial.println();
}

// -----------------------------------------------------------------------------
// Command matching.
//
// True if `s` starts with `word` AND the next character is a space or the end
// of the string - so "enc" matches "enc" and "enc on" but not "encrypt". That
// second test is the whole reason this is not just strncmp: without it, every
// command would shadow every longer command that starts with the same letters.
//
// On a match, *rest is left pointing at the first argument character with the
// separating spaces skipped, or at the terminating NUL if there is no argument.
// Callers test *arg to tell "show me" from "set it to".
// -----------------------------------------------------------------------------
static bool matches(const char* s, const char* word, const char** rest) {
  const size_t n = strlen(word);
  if (strncmp(s, word, n) != 0) return false;
  if (s[n] != '\0' && s[n] != ' ') return false;
  const char* r = s + n;
  while (*r == ' ') r++;
  *rest = r;
  return true;
}

// -----------------------------------------------------------------------------
// Run one line. Nothing here touches the radio, the codec or the I2S
// peripheral directly - every command goes through the same entry point the
// buttons use, so the console cannot get the state machine into a position the
// buttons could not.
// -----------------------------------------------------------------------------
static void execute(char* cmd) {
  // Trim leading spaces, then trailing spaces and the carriage return that
  // terminals send ahead of the newline. Without the \r strip, "enc on\r"
  // would compare against "on\r" and never match "on".
  while (*cmd == ' ') cmd++;
  char* end = cmd + strlen(cmd);
  while (end > cmd && (end[-1] == ' ' || end[-1] == '\r')) *--end = '\0';
  if (*cmd == '\0') return;   // bare newline: no complaint, no output

  const char* arg = nullptr;

  if (matches(cmd, "help", &arg) || matches(cmd, "?", &arg)) {
    printHelp();

  } else if (matches(cmd, "stat", &arg)) {
    printStat();

  } else if (matches(cmd, "presets", &arg)) {
    printPresets();

  // `ch` kept as an alias: it is what the sibling lab and every radio in the
  // world calls this, even though a preset here is more than a frequency.
  } else if (matches(cmd, "preset", &arg) || matches(cmd, "ch", &arg)) {
    if (*arg) {
      const int n = atoi(arg);
      if (n < 0 || n >= VOICE_PRESET_COUNT) {
        Serial.printf("preset must be 0..%d (try `presets`)\n",
                      VOICE_PRESET_COUNT - 1);
      } else {
        linkSetPreset((uint8_t)n);
        // linkSetPreset() is asynchronous - it hands the change to linkTask.
        // Give it a moment so the readback below reports the new preset
        // rather than the old one.
        delay(50);
      }
    }
    {
      const VoicePreset& cur = linkCurrentPreset();
      if (cur.kind == PRESET_RADIO) {
        Serial.printf("preset %u = %s, %.3f MHz, %d dBm, limit %.0f%% (%s)\n",
                      (unsigned)linkPresetIndex(), cur.label,
                      (double)cur.freqMHz, (int)cur.powerDbm,
                      (double)cur.dutyLimit, cur.note);
      } else {
        Serial.printf("preset %u = %s - %s\n", (unsigned)linkPresetIndex(),
                      cur.label, cur.note);
      }
    }

  } else if (matches(cmd, "enc", &arg)) {
    if (*arg) {
      const bool on = (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
      if (!linkSetEncryption(on) && on) {
        Serial.printf("cannot arm: %s\n", cryptoKeyProblem());
      }
    }
    Serial.printf("encryption %s, key %s\n", linkEncryption() ? "ON" : "off",
                  cryptoFingerprint());

  } else if (matches(cmd, "screen", &arg)) {
    if (*arg) uiSetScreen((UiScreen)atoi(arg));
    else      uiNextScreen();
    Serial.printf("screen %d\n", (int)uiScreen());

  } else if (matches(cmd, "ptt", &arg)) {
    uint32_t ms = *arg ? (uint32_t)atol(arg) : 3000UL;
    if (ms > 30000UL) ms = 30000UL;   // this thing transmits; cap it
    buttonsInject(BTN_PTT_DOWN);
    pttReleaseAt = millis() + ms;
    if (linkSelfTestMode()) {
      Serial.printf("recording for %lu ms, then playing it back\n",
                    (unsigned long)ms);
    } else {
      Serial.printf("keyed for %lu ms\n", (unsigned long)ms);
    }

  } else if (matches(cmd, "tone", &arg)) {
    if (*arg) audioForceTestSignal(strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
    Serial.printf("test signal %s%s\n",
                  audioTestSignalActive() ? "active" : "off",
                  audioMicPresent() ? "" : " (no microphone fitted)");

  } else if (matches(cmd, "beep", &arg)) {
    if (*arg) audioSetCues(strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
    Serial.printf("cue tones %s\n", audioCuesEnabled() ? "on" : "off");
    if (audioCuesEnabled()) {
      // Play all four so you can learn what they mean by ear rather than from
      // the table in config.h. They are local: nothing goes on the air here,
      // on any preset, so this is safe to run as often as you like.
      //
      // Requested rather than played, because voiceTask owns the I2S
      // peripheral and this is the loop task. It happens as soon as the
      // handset is idle - which, if a transmission is arriving right now, is
      // when that transmission finishes.
      Serial.println("  keyup, incoming, roger, lost");
      appRequestCueDemo();
    }

  } else if (matches(cmd, "reboot", &arg)) {
    Serial.println("rebooting");
    Serial.flush();
    ESP.restart();

  } else {
    Serial.printf("unknown command: %s (try `help`)\n", cmd);
  }
}

void consoleBegin() {
  Serial.println("[console] type `help` for commands");
}

void consolePoll() {
  // A scripted PTT press releases itself here rather than in a delay(), so the
  // console keeps answering while the radio is keyed - which is the one time
  // you might want to type `stat`.
  if (pttReleaseAt && (int32_t)(millis() - pttReleaseAt) >= 0) {
    pttReleaseAt = 0;
    buttonsInject(BTN_PTT_UP);
    Serial.println("released");
  }

  // Accumulate characters until a line terminator. Serial.available() returns
  // immediately when the buffer is empty, so this costs nothing on the idle
  // path and never blocks the loop task.
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      // Terminals send \r\n, so the second terminator arrives with fill == 0
      // and is ignored rather than running an empty command.
      if (fill > 0) {
        line[fill] = '\0';
        fill = 0;
        execute(line);
      }
    } else if (fill < sizeof(line) - 1) {
      line[fill++] = c;
    }
    // Overlong lines drop their tail rather than overflowing the buffer. The
    // command still runs on the prefix, which for a typo is the right amount
    // of unhelpful.
  }
}
