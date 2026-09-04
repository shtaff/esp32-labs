// =============================================================================
// audio.cpp - one I2S peripheral, pointed in whichever direction is needed.
//
// Two things in here are not obvious and cost real debugging time if you get
// them wrong. Both are worked around by the same decision, so they are
// explained together:
//
// 1. THE MONO CHANNEL FORMAT IS A TRAP ON THE ORIGINAL ESP32.
//
//    I2S_CHANNEL_FMT_ONLY_LEFT in RX mode does not reliably capture the left
//    slot on this silicon - the usual result is that an INMP441 with L/R tied
//    to ground, which by its datasheet transmits in the LEFT slot, is silent
//    unless you ask for ONLY_RIGHT. Which of the two works depends on the chip
//    revision, the IDF version and the phase of the moon, and the failure mode
//    is complete silence with no error anywhere.
//
//    On the transmit side the same enum has a different problem: whether a
//    mono sample is duplicated into both slots or only into one determines
//    whether a MAX98357A with SD_MODE floating - which plays (L+R)/2 - comes
//    out at full level, at half level, or silent.
//
//    So this file never uses the mono formats. Both directions run
//    I2S_CHANNEL_FMT_RIGHT_LEFT and the interleaving is handled in software:
//    capture reads both slots and keeps one, playback writes each sample
//    twice. The cost is 32 kB/s of extra DMA traffic on a part that has no
//    trouble with it, and in exchange there is nothing left to get wrong.
//
// 2. WHICH SLOT THE MICROPHONE IS IN IS DISCOVERED, NOT CONFIGURED.
//
//    Because capture is stereo anyway, the boot probe can simply look at both
//    slots and work out which one the part is driving - see audioProbeMic().
//    That makes the L/R strap on the breakout a don't-care, and it is the same
//    measurement that answers "is there a microphone at all".
//
// The direction switch itself uninstalls and reinstalls the driver, which
// takes about a millisecond. That is only acceptable because the transitions
// are human-speed: they happen when somebody presses or releases PTT.
// =============================================================================
#include "audio.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <driver/i2s.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>

#include "codec.h"
#include "config.h"

#define I2S_PORT I2S_NUM_0

// DMA sizing.
//
// Capture needs enough buffered audio to ride out the encoder: Codec2 takes a
// large fraction of a frame period to run, and the microphone does not stop
// while it does. 120 ms is three codec frames of slack against a 40 ms frame.
//
// The length is derived from the capture rate rather than fixed, so that
// changing VOICE_MIC_OVERSAMPLE changes how many samples are buffered but not
// how much TIME they represent. Hard-coding the frame count instead would make
// the oversampling knob secretly a latency knob - at 1x it would buffer 480 ms
// of microphone, which is most of a syllable of lag on every transmission.
//
// A single DMA buffer must also stay under 4092 bytes, and 32-bit stereo is
// 8 bytes per frame; the static_assert below holds us to it.
#define MIC_DMA_MS    120
#define MIC_DMA_COUNT 8
#define MIC_DMA_LEN   ((MIC_DMA_MS * VOICE_MIC_CAPTURE_RATE / 1000) / MIC_DMA_COUNT)

static_assert(MIC_DMA_LEN * 2 * sizeof(int32_t) <= 4092,
              "microphone DMA buffer over the driver's 4092-byte limit");
static_assert(MIC_DMA_LEN >= 64, "microphone DMA buffer implausibly small");

// Playback needs much less, because decode is cheap and i2s_write() blocking
// on a full ring is exactly the pacing mechanism we want - a deeper ring here
// is pure added latency. 4 x 160 frames at 8 kHz is 80 ms.
#define SPK_DMA_COUNT 4
#define SPK_DMA_LEN   160

// Oversampling has to stay in a range the fixed buffers below were sized for,
// and 1 (no oversampling) has to remain legal so the old behaviour can be
// compared against directly.
static_assert(VOICE_MIC_OVERSAMPLE >= 1 && VOICE_MIC_OVERSAMPLE <= 4,
              "VOICE_MIC_OVERSAMPLE must be 1..4 - see config.h");

static AudioDir currentDir = AUDIO_DIR_OFF;
static bool driverInstalled = false;

static bool    micPresent   = false;
static uint8_t micSlot      = 0;      // 0 or 1, the interleave index to keep
static uint8_t micConfidence = 0;
static bool    forceTestSignal = false;
static uint32_t captureTimeouts = 0;
static uint32_t clipCount = 0;

static uint8_t level = 0;

// DC blocker state, carried between frames. Reset whenever capture starts, so
// the settling transient of one transmission cannot ring into the next.
static float dcPrevIn = 0.0f;
static float dcPrevOut = 0.0f;

// Scratch. Static rather than on the stack: at 4x oversampling this is 10 kB,
// which voiceTask would rather not lose, and there is exactly one caller of
// each. Sized for the worst case - the longest codec frame, both I2S slots,
// and the highest oversampling factor.
static int32_t micRaw[2 * VOICE_MAX_SAMPLES_PER_FRAME * VOICE_MIC_OVERSAMPLE];
static int16_t spkRaw[2 * VOICE_MAX_SAMPLES_PER_FRAME];

// -----------------------------------------------------------------------------
// The amplifier's data line when we are not driving it.
//
// The MAX98357A has no shutdown pin wired here, so while the bit clock is
// running - which it is, in AUDIO_DIR_MIC, because both devices share the
// clocks - it will happily play whatever DIN happens to be doing. Left as an
// undriven I2S output that is a floating pin, and a floating pin into a class
// D amplifier is a hiss at full volume.
//
// Detaching the peripheral from the pad and holding it low feeds the amplifier
// a legitimate stream of zero samples instead, which is silence. The
// esp_rom_gpio_connect_out_signal() call is the part that matters: without it
// the GPIO matrix still routes I2S0's data-out signal to the pad and
// digitalWrite() has no effect.
// -----------------------------------------------------------------------------
static void holdAmpSilent() {
  esp_rom_gpio_connect_out_signal(PIN_I2S_DOUT, SIG_GPIO_OUT_IDX, false, false);
  pinMode(PIN_I2S_DOUT, OUTPUT);
  digitalWrite(PIN_I2S_DOUT, LOW);
}

static void uninstall() {
  if (!driverInstalled) return;
  i2s_driver_uninstall(I2S_PORT);
  driverInstalled = false;
}

static bool install(AudioDir dir) {
  const bool rx = (dir == AUDIO_DIR_MIC);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | (rx ? I2S_MODE_RX : I2S_MODE_TX));
  // Capture is oversampled and averaged down in software; playback is not,
  // because the codec's output is already 8 kHz. See the oversampling section
  // of config.h for why the microphone must not be clocked at 8 kHz.
  cfg.sample_rate = rx ? VOICE_MIC_CAPTURE_RATE : VOICE_SAMPLE_RATE;
  // 32 bits for capture because the INMP441 is a 24-bit part in a 32-bit slot
  // and the top bits are where the audio is; 16 for playback because that is
  // what the codec produces and what the amplifier wants.
  cfg.bits_per_sample = rx ? I2S_BITS_PER_SAMPLE_32BIT : I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;   // never the mono formats
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  // LEVEL1 keeps the I2S interrupt below anything that could be hurt by it,
  // and it is the only priority the driver will accept alongside the SPI and
  // GPIO interrupts the radio and the buttons install.
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = rx ? MIC_DMA_COUNT : SPK_DMA_COUNT;
  cfg.dma_buf_len   = rx ? MIC_DMA_LEN   : SPK_DMA_LEN;
  cfg.use_apll = false;
  // Underflow plays the last buffer again unless it is cleared. On a voice
  // link an underflow is a dropped packet, and a 20 ms fragment of the
  // previous word repeating is far more distracting than 20 ms of silence.
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = 0;

  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("[audio] i2s_driver_install failed");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = PIN_I2S_BCLK;
  pins.ws_io_num    = PIN_I2S_WS;
  pins.data_out_num = rx ? I2S_PIN_NO_CHANGE : PIN_I2S_DOUT;
  pins.data_in_num  = rx ? PIN_I2S_DIN       : I2S_PIN_NO_CHANGE;

  if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("[audio] i2s_set_pin failed");
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }

  driverInstalled = true;
  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

// -----------------------------------------------------------------------------
// Is there a microphone, and which slot is it in?
//
// The tempting test - "read some samples, see if anything moves" - does not
// work, and it fails in the worst direction. GPIO34 is an input-only pin with
// no internal pull-up or pull-down, so with nothing fitted it floats, picks up
// whatever is on the board, and delivers a stream of convincing-looking
// garbage. A level test calls that a microphone every time.
//
// What actually separates the two cases is the shape of the words, not their
// size. The INMP441 sends 24 bits of audio into a 32-bit slot and drives the
// remaining 8 bits low, so every sample from a real part ends in a zero byte.
// A floating pin ends in a zero byte one time in 256.
//
// So: present if nearly every word in a slot has a zero low byte AND the slot
// is not simply stuck at a constant - which catches the other failure, a pin
// tied to a rail. The slot that passes is the slot the microphone is in, which
// makes the L/R strap on the breakout something you no longer have to get
// right.
// -----------------------------------------------------------------------------
static void audioProbeMic() {
  const int frames = 512;    // 64 ms at 8 kHz, two DMA buffers' worth
  static int32_t probe[2 * 512];

  if (!install(AUDIO_DIR_MIC)) return;

  // The part needs its filters to converge before anything it says is
  // meaningful. Clock it, then throw away everything from that window.
  delay(VOICE_MIC_SETTLE_MS);
  size_t got = 0;
  i2s_read(I2S_PORT, probe, sizeof(probe), &got, pdMS_TO_TICKS(200));
  i2s_read(I2S_PORT, probe, sizeof(probe), &got, pdMS_TO_TICKS(200));

  if (got < sizeof(probe)) {
    Serial.println("[audio] microphone probe: no data from I2S at all");
    uninstall();
    return;
  }

  int     zeroLowByte[2] = {0, 0};
  int32_t lo[2] = {INT32_MAX, INT32_MAX};
  int32_t hi[2] = {INT32_MIN, INT32_MIN};

  for (int i = 0; i < frames; i++) {
    for (int s = 0; s < 2; s++) {
      int32_t v = probe[i * 2 + s];
      if ((v & 0xFF) == 0) zeroLowByte[s]++;
      if (v < lo[s]) lo[s] = v;
      if (v > hi[s]) hi[s] = v;
    }
  }

  int best = (zeroLowByte[1] > zeroLowByte[0]) ? 1 : 0;
  int pct  = zeroLowByte[best] * 100 / frames;

  micSlot       = (uint8_t)best;
  micConfidence = (uint8_t)pct;
  micPresent    = (pct >= 90) && (hi[best] != lo[best]);

  Serial.printf("[audio] microphone probe: slot0 %d%% slot1 %d%%, range %ld/%ld\n",
                zeroLowByte[0] * 100 / frames, zeroLowByte[1] * 100 / frames,
                (long)(hi[0] - lo[0]), (long)(hi[1] - lo[1]));
  if (micPresent) {
    Serial.printf("[audio] INMP441 found in slot %d (%d%% confidence)\n", best, pct);
  } else {
    Serial.println("[audio] no microphone detected on GPIO34");
    Serial.println("[audio] PTT will transmit the synthetic test signal instead");
  }

  uninstall();
}

bool audioBegin() {
  holdAmpSilent();

  audioProbeMic();

  // The probe leaves the driver uninstalled. Confirm the peripheral is usable
  // in the other direction too, so a failure surfaces at boot rather than the
  // first time somebody is spoken to.
  if (!install(AUDIO_DIR_SPK)) return false;
  uninstall();
  holdAmpSilent();
  currentDir = AUDIO_DIR_OFF;

#ifdef NO_AMP
  Serial.println("[audio] built with -DNO_AMP: received audio is decoded and metered, not played");
#endif
  return true;
}

bool audioMicPresent()      { return micPresent; }
uint8_t audioMicSlot()      { return micSlot; }
uint8_t audioMicConfidence(){ return micConfidence; }
AudioDir audioDirection()   { return currentDir; }
uint32_t audioCaptureTimeouts() { return captureTimeouts; }
uint32_t audioClipCount()   { return clipCount; }
void     audioResetClipCount() { clipCount = 0; }
uint32_t audioCaptureRate() { return VOICE_MIC_CAPTURE_RATE; }

bool audioAmpEnabled() {
#ifdef NO_AMP
  return false;
#else
  return true;
#endif
}

void audioForceTestSignal(bool on) { forceTestSignal = on; }
bool audioTestSignalActive()       { return forceTestSignal || !micPresent; }

void audioSetDirection(AudioDir dir) {
  if (dir == currentDir) return;

  uninstall();

  if (dir == AUDIO_DIR_OFF) {
    holdAmpSilent();
    currentDir = AUDIO_DIR_OFF;
    return;
  }

  if (!install(dir)) {
    holdAmpSilent();
    currentDir = AUDIO_DIR_OFF;
    return;
  }

  if (dir == AUDIO_DIR_MIC) {
    // The clocks have just restarted, so the INMP441 is settling again and its
    // first samples are a large DC step. Sending that is a thud at the far end
    // on every single over.
    //
    // Waiting is only half the fix. The DMA ring has been filling throughout
    // the wait, so the settling transient is now sitting in it - up to 160 ms
    // of it - and the first thing a blocking read returns is the oldest of it.
    // So wait, then empty the ring with non-blocking reads until it runs dry.
    // The next read then blocks, and blocks on genuinely fresh audio.
    holdAmpSilent();
    delay(VOICE_MIC_SETTLE_MS);

    size_t got = 0;
    for (int i = 0; i < MIC_DMA_COUNT + 2; i++) {
      if (i2s_read(I2S_PORT, micRaw, sizeof(micRaw), &got, 0) != ESP_OK) break;
      if (got < sizeof(micRaw)) break;
    }

    // Start the DC blocker from rest. Carrying state across a direction change
    // would let the settling transient of one transmission ring into the
    // start of the next, which is a thud on exactly the syllable you wanted.
    dcPrevIn = 0.0f;
    dcPrevOut = 0.0f;
  }

  currentDir = dir;
}

// -----------------------------------------------------------------------------
// The synthetic test signal.
//
// This exists so that a board with no microphone is still a transmitter, which
// is the difference between being able to test a link with one set of audio
// hardware and needing two.
//
// It is not a tone. Codec2 models a human vocal tract, and feeding it a sine
// wave produces something between a warble and nothing at all - a useless
// test, because you cannot tell a broken link from a codec that had nothing to
// work with. What comes out of here is a crude vowel: an impulse train at
// speech pitch driving two resonators sitting at the first two formants of a
// sustained "ah". Codec2 encodes it happily, it survives the link recognisably,
// and it is obviously artificial, so nobody mistakes it for a live microphone.
//
// The resonators are two-pole recursive filters excited by an impulse each
// glottal period: two multiplies and an add per sample per formant, versus a
// transcendental per sample if this were built out of sin() and exp().
// -----------------------------------------------------------------------------
struct Formant {
  float y1, y2;
  float a1, a2;
};

static Formant fmt1, fmt2;
static float   pitchPhase = 0.0f;
static uint32_t toneSample = 0;
static bool     toneInit = false;

static void formantSet(Formant& f, float freqHz, float bwHz) {
  const float r = expf(-(float)PI * bwHz / VOICE_SAMPLE_RATE);
  const float w = 2.0f * (float)PI * freqHz / VOICE_SAMPLE_RATE;
  f.a1 = 2.0f * r * cosf(w);
  f.a2 = -r * r;
  f.y1 = f.y2 = 0.0f;
}

static inline float formantStep(Formant& f, float excite) {
  const float y = f.a1 * f.y1 + f.a2 * f.y2 + excite;
  f.y2 = f.y1;
  f.y1 = y;
  return y;
}

static void generateTestSignal(int16_t* dst, int n) {
  if (!toneInit) {
    formantSet(fmt1, 730.0f, 90.0f);    // F1 of "ah"
    formantSet(fmt2, 1090.0f, 110.0f);  // F2 of "ah"
    toneInit = true;
  }

  for (int i = 0; i < n; i++) {
    // Pitch drifts 110 -> 150 Hz over about 1.6 s, then jumps back. A fixed
    // pitch encodes suspiciously well; a moving one exercises the estimator.
    const float cycle = (float)(toneSample % 13000) / 13000.0f;
    const float f0 = 110.0f + 40.0f * cycle;

    // Gate: 600 ms on, 300 ms off. The gaps are what let you hear where the
    // link is dropping frames rather than where the talker paused.
    const bool gated = ((toneSample / 4800) % 3) != 2;

    float excite = 0.0f;
    pitchPhase += f0 / VOICE_SAMPLE_RATE;
    if (pitchPhase >= 1.0f) {
      pitchPhase -= 1.0f;
      if (gated) excite = 1400.0f;
    }

    float v = formantStep(fmt1, excite) + 0.6f * formantStep(fmt2, excite);

    if (v >  32000.0f) v =  32000.0f;
    if (v < -32000.0f) v = -32000.0f;
    dst[i] = (int16_t)v;

    toneSample++;
  }
}

// -----------------------------------------------------------------------------
// Level metering. Peak of the frame, with an asymmetric decay: instant attack
// so a transient shows, slow release so the bar is readable at four frames a
// second.
// -----------------------------------------------------------------------------
static void meter(const int16_t* samples, int n) {
  int32_t peak = 0;
  for (int i = 0; i < n; i++) {
    int32_t a = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
    if (a > peak) peak = a;
  }
  uint8_t pct = (uint8_t)((peak * 100) / 32767);
  if (pct > level) level = pct;
  else if (level > 2) level -= 2;
  else level = 0;
}

void    audioResetLevel() { level = 0; }
uint8_t audioLevel()      { return level; }

bool audioCaptureFrame(int16_t* dst) {
  const int n = codecSamplesPerFrame();

  // One codec frame of output needs VOICE_MIC_OVERSAMPLE input samples each,
  // and every input sample arrives as a stereo pair of 32-bit words.
  const int    raw  = n * VOICE_MIC_OVERSAMPLE;
  const size_t want = (size_t)raw * 2 * sizeof(int32_t);

  bool ok = true;
  if (currentDir == AUDIO_DIR_MIC) {
    size_t got = 0;
    // Four frame periods of patience. The DMA ring holds three codec frames'
    // worth, so anything longer than this means the ring has already overrun
    // and the audio is broken in a way that waiting will not fix.
    esp_err_t rc = i2s_read(I2S_PORT, micRaw, want, &got,
                            pdMS_TO_TICKS(codecFrameMs() * 4));
    if (rc != ESP_OK || got < want) {
      captureTimeouts++;
      memset((uint8_t*)micRaw + got, 0, want - got);
      ok = false;
    }
  } else {
    // No capture direction configured: nothing to pace against, so pace on the
    // clock instead. This only happens if a direction switch failed.
    vTaskDelay(pdMS_TO_TICKS(codecFrameMs()));
    memset(micRaw, 0, want);
    ok = false;
  }

  if (audioTestSignalActive()) {
    // The DMA read above still happened and still took one frame period, so
    // the synthetic path runs at exactly the rate the real one would. Its
    // output is discarded; only the timing was wanted.
    generateTestSignal(dst, n);
  } else {
    // ----------------------------------------------------------------------
    // Decimate, scale, block DC, saturate.
    //
    // The scale factor is the gain shift expressed as a multiply so it can be
    // applied in floating point BEFORE the DC blocker. Shifting first would
    // throw away the low bits the blocker needs to track a slowly drifting
    // offset - which is most of what it is for.
    // ----------------------------------------------------------------------
    const float scale = 1.0f / (float)(1UL << (16 - VOICE_MIC_GAIN_SHIFT));

    for (int i = 0; i < n; i++) {
      // Box-average VOICE_MIC_OVERSAMPLE consecutive samples of the slot the
      // boot probe identified, discarding the other slot. int64 because four
      // full-scale 32-bit samples do not fit in an int32 accumulator.
      int64_t acc = 0;
      for (int k = 0; k < VOICE_MIC_OVERSAMPLE; k++) {
        acc += micRaw[((size_t)(i * VOICE_MIC_OVERSAMPLE + k)) * 2 + micSlot];
      }
      float s = (float)(acc / VOICE_MIC_OVERSAMPLE) * scale;

      // One-pole DC blocker: y = x - x1 + R*y1. Codec2 does not high-pass its
      // own input, and the INMP441 has a real offset, so without this the
      // offset goes straight into the LPC analysis.
      const float y = s - dcPrevIn + VOICE_MIC_DC_POLE * dcPrevOut;
      dcPrevIn  = s;
      dcPrevOut = y;
      s = y;

      // Saturate rather than wrap: a wrapped overload inverts the waveform,
      // and Codec2 renders that as a bark. Count it, because "clipping" and
      // "broken microphone" sound the same and are fixed differently.
      if (s > 32767.0f)       { s = 32767.0f;  clipCount++; }
      else if (s < -32768.0f) { s = -32768.0f; clipCount++; }
      dst[i] = (int16_t)s;
    }
  }

  meter(dst, n);
  return ok;
}

// =============================================================================
// Cue tones.
//
// Unlike everything else that reaches the speaker, these never go through
// Codec2 - they are synthesised straight into PCM here. That is the whole
// point: Codec2 models a vocal tract, and a pure tone pushed through it comes
// out warbling and different every time. A tone generated at the point of
// playback is clean, identical on every board, and obviously not speech.
//
// A cue is a list of segments, each a frequency and a duration. Frequency zero
// is silence, which is how the two-blip cues get their gap.
// =============================================================================
struct CueSegment {
  uint16_t freqHz;   // 0 = silence
  uint16_t ms;
};

// The four cues. Chosen so they are distinguishable from each other with the
// radio at arm's length, and so the two that matter most are opposites:
// ROGER falls (a settled, finished sound) and LOST is a flat low double blip
// (an unresolved, something-went-wrong sound). Do not make LOST pleasant.
static const CueSegment CUE_KEYUP[]    = { {1200, 60} };
static const CueSegment CUE_INCOMING[] = { {1800, 35} };
static const CueSegment CUE_ROGER[]    = { {1600, 55}, {1100, 80} };
static const CueSegment CUE_LOST[]     = { { 700, 60}, {   0, 45}, {700, 60} };

struct CueDef {
  const CueSegment* seg;
  uint8_t count;
};

static const CueDef CUES[] = {
  { CUE_KEYUP,    1 },
  { CUE_INCOMING, 1 },
  { CUE_ROGER,    2 },
  { CUE_LOST,     3 },
};

static bool cuesOn = (VOICE_CUE_TONES != 0);

void audioSetCues(bool on)  { cuesOn = on; }
bool audioCuesEnabled()     { return cuesOn; }

// Total duration of a cue, needed to work out how long to wait for the DMA to
// drain at the end.
static uint32_t cueDurationMs(const CueDef& def) {
  uint32_t total = 0;
  for (uint8_t i = 0; i < def.count; i++) total += def.seg[i].ms;
  return total;
}

// Interleave a block of mono samples into both slots and push it at the DMA.
// Same duplication as audioPlayFrame(), and for the same reason - see the
// channel-format discussion at the top of this file.
static void cueWrite(const int16_t* mono, int n) {
  for (int i = 0; i < n; i++) {
    spkRaw[i * 2]     = mono[i];
    spkRaw[i * 2 + 1] = mono[i];
  }
  size_t written = 0;
  i2s_write(I2S_PORT, spkRaw, (size_t)n * 2 * sizeof(int16_t), &written,
            portMAX_DELAY);
}

void audioPlayCue(AudioCue cue) {
  if (!cuesOn) return;
#ifdef NO_AMP
  // Nothing to hear and no reason to spend the time.
  return;
#else
  const CueDef& def = CUES[cue];

  // Take the peripheral ourselves rather than making every caller do it. This
  // is idempotent, so a cue played while already in playback - the roger beep
  // at the end of a received transmission - costs nothing.
  audioSetDirection(AUDIO_DIR_SPK);
  if (currentDir != AUDIO_DIR_SPK) return;   // the direction change failed

  const int16_t amp = (int16_t)(32767L * VOICE_CUE_LEVEL_PCT / 100);

  // 5 ms of fade at each end of every segment. A hard-gated sine starts and
  // stops mid-cycle, and that step is a click - through a class D amplifier a
  // surprisingly loud one, easily louder than the tone it is bracketing.
  const int fadeLen = 5 * VOICE_SAMPLE_RATE / 1000;

  static int16_t block[160];
  const int blockLen = (int)(sizeof(block) / sizeof(block[0]));

  for (uint8_t s = 0; s < def.count; s++) {
    const int total = (int)((uint32_t)def.seg[s].ms * VOICE_SAMPLE_RATE / 1000);
    const float step = 2.0f * (float)PI * def.seg[s].freqHz / VOICE_SAMPLE_RATE;
    // Phase restarts per segment. That is deliberate for these cues: each
    // segment is separately faded to zero, so there is no discontinuity to
    // carry across, and restarting keeps the two blips of ROGER identical
    // regardless of what came before.
    float phase = 0.0f;
    int done = 0;

    while (done < total) {
      const int n = (total - done < blockLen) ? (total - done) : blockLen;
      for (int i = 0; i < n; i++) {
        const int pos = done + i;

        // Trapezoidal envelope: up over fadeLen, flat, down over fadeLen.
        // Clamped so a segment shorter than two fades still tapers.
        float env = 1.0f;
        const int fade = (fadeLen * 2 > total) ? total / 2 : fadeLen;
        if (fade > 0) {
          if (pos < fade)                 env = (float)pos / fade;
          else if (pos >= total - fade)   env = (float)(total - 1 - pos) / fade;
        }
        if (env < 0.0f) env = 0.0f;

        if (def.seg[s].freqHz == 0) {
          block[i] = 0;
        } else {
          block[i] = (int16_t)(amp * env * sinf(phase));
          phase += step;
          if (phase > 2.0f * (float)PI) phase -= 2.0f * (float)PI;
        }
      }
      meter(block, n);
      cueWrite(block, n);
      done += n;
    }
  }

  // Wait for what is still in the DMA ring to actually be played.
  //
  // i2s_write() returns once a sample is queued, not once it is audible. The
  // ring holds SPK_DMA_COUNT * SPK_DMA_LEN samples, so at the moment the last
  // write returns there is at most that much - or the whole cue, if the cue is
  // shorter than the ring - still outstanding. Waiting the smaller of the two
  // is a safe upper bound, and it is what stops the caller's next direction
  // change from cutting the beep off mid-note.
  const uint32_t ringMs = (uint32_t)SPK_DMA_COUNT * SPK_DMA_LEN * 1000UL /
                          VOICE_SAMPLE_RATE;
  const uint32_t cueMs  = cueDurationMs(def);
  vTaskDelay(pdMS_TO_TICKS(cueMs < ringMs ? cueMs : ringMs));
#endif
}

void audioPlayFrame(const int16_t* src) {
  const int n = codecSamplesPerFrame();
  meter(src, n);

#ifdef NO_AMP
  // Decoded, metered, and deliberately not played. The frame still cost the
  // same CPU, so the timing on the AUDIO screen stays honest, and the VU bar
  // still moves - which on a board with no amplifier is the only way to see
  // that the far end is talking.
  vTaskDelay(pdMS_TO_TICKS(codecFrameMs()));
  return;
#else
  if (currentDir != AUDIO_DIR_SPK) {
    vTaskDelay(pdMS_TO_TICKS(codecFrameMs()));
    return;
  }

  // Duplicate into both slots - see the channel format discussion at the top.
  for (int i = 0; i < n; i++) {
    spkRaw[i * 2]     = src[i];
    spkRaw[i * 2 + 1] = src[i];
  }

  size_t written = 0;
  // portMAX_DELAY on purpose. i2s_write() blocking on a full DMA ring is the
  // clock for the whole receive path: it is what makes playback come out at
  // 8000 samples a second without a timer anywhere.
  i2s_write(I2S_PORT, spkRaw, (size_t)n * 2 * sizeof(int16_t), &written,
            portMAX_DELAY);
#endif
}
