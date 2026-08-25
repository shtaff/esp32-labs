// =============================================================================
// codec.cpp - Codec2, wrapped so the rest of the firmware never sees it.
//
// The wrapper does three things the raw library does not:
//
//   - It queries the frame geometry instead of hard-coding it, so changing
//     VOICE_CODEC_MODE in config.h really is a one-line change.
//   - It checks that geometry against the fixed buffer sizes the rest of the
//     firmware relies on, and refuses to start rather than overflow them.
//   - It times every call, because "does this codec mode fit in the CPU
//     budget" is the question that decides whether a mode is usable at all,
//     and the only way to answer it is to measure on the real hardware.
// =============================================================================
#include "codec.h"

#include <Arduino.h>
#include <codec2.h>

#include "config.h"

static struct CODEC2* c2 = nullptr;
static int  samplesPerFrame = 0;
static int  bytesPerFrame   = 0;
static uint32_t frameMs     = 0;

// Rolling means, as fixed-point accumulators. A plain exponential average with
// a shift of 4 (about a 16-frame time constant): fast enough to react to a
// mode change, slow enough that one unlucky frame does not dominate the
// display. Kept in microseconds, which never overflows here - a frame that
// took 4000 seconds would have tripped the watchdog long before.
static uint32_t encUsAvg = 0, decUsAvg = 0;
static uint32_t encUsPeak = 0, decUsPeak = 0;

static inline void accumulate(uint32_t& avg, uint32_t& peak, uint32_t sample) {
  avg = avg - (avg >> 4) + (sample >> 4);
  if (sample > peak) peak = sample;
}

bool codecBegin() {
  c2 = codec2_create(VOICE_CODEC_MODE);
  if (c2 == nullptr) {
    // Almost always the CODEC2_MODE_*_EN flags: with CODEC2_MODE_EN_DEFAULT=0
    // in platformio.ini, a mode that is not explicitly enabled compiles away
    // and codec2_create() returns NULL for it rather than failing to link.
    Serial.println("[codec] codec2_create failed");
    Serial.println("[codec] enable this mode's CODEC2_MODE_*_EN in platformio.ini");
    return false;
  }

  samplesPerFrame = codec2_samples_per_frame(c2);
  bytesPerFrame   = codec2_bytes_per_frame(c2);
  frameMs         = (uint32_t)samplesPerFrame * 1000UL / VOICE_SAMPLE_RATE;

  // Guard the fixed-size buffers the rest of the firmware relies on. These are
  // compile-time truths about Codec2 rather than anything we control, so if a
  // future library version changes them, stopping here is far better than
  // discovering it as a stack smash inside the audio task.
  if (samplesPerFrame > VOICE_MAX_SAMPLES_PER_FRAME ||
      bytesPerFrame   > VOICE_MAX_BYTES_PER_FRAME) {
    Serial.printf("[codec] frame too large: %d samples, %d bytes\n",
                  samplesPerFrame, bytesPerFrame);
    codec2_destroy(c2);
    c2 = nullptr;
    return false;
  }

  // The post filter costs almost nothing and makes low-rate modes noticeably
  // less buzzy. Gray coding of the quantiser indices means a single bit error
  // moves a parameter by one step instead of anywhere at all, which matters
  // because nothing here retransmits: a damaged frame is played as it landed.
  codec2_set_lpc_post_filter(c2, 1, 0, 0.8f, 0.2f);
  codec2_set_natural_or_gray(c2, 1);

  Serial.printf("[codec] Codec2 %s: %d samples (%lu ms) -> %d bytes per frame\n",
                codecModeName(), samplesPerFrame,
                (unsigned long)frameMs, bytesPerFrame);
  Serial.printf("[codec] %u frames per packet = %lu ms of audio, %d payload bytes\n",
                (unsigned)VOICE_FRAMES_PER_PACKET,
                (unsigned long)(frameMs * VOICE_FRAMES_PER_PACKET),
                bytesPerFrame * VOICE_FRAMES_PER_PACKET);
  return true;
}

bool codecReady()            { return c2 != nullptr; }
int  codecSamplesPerFrame()  { return samplesPerFrame; }
int  codecBytesPerFrame()    { return bytesPerFrame; }
uint32_t codecFrameMs()      { return frameMs; }

// -----------------------------------------------------------------------------
// The packet header carries a 4-bit codec id, not the Codec2 mode constant.
// Only the three modes this firmware builds are represented; anything else is
// reported as unknown rather than silently mapped onto one of them.
// -----------------------------------------------------------------------------
uint8_t codecId() {
#if VOICE_CODEC_MODE == CODEC2_MODE_3200
  return 0;
#elif VOICE_CODEC_MODE == CODEC2_MODE_1600
  return 1;
#elif VOICE_CODEC_MODE == CODEC2_MODE_700C
  return 2;
#else
  return 15;   // "unknown"; two boards built like this still interoperate
#endif
}

const char* codecNameForId(uint8_t id) {
  switch (id) {
    case 0:  return "3200";
    case 1:  return "1600";
    case 2:  return "700C";
    default: return "?";
  }
}

const char* codecModeName() { return codecNameForId(codecId()); }

// Encode one frame. The int16_t/short cast is safe on this toolchain - short
// is 16 bits on xtensa - and is needed because Codec2's API predates stdint
// being used consistently.
void codecEncode(uint8_t* bits, const int16_t* samples) {
  if (c2 == nullptr) {
    memset(bits, 0, bytesPerFrame);
    return;
  }
  uint32_t t0 = micros();
  codec2_encode(c2, bits, (short*)samples);
  accumulate(encUsAvg, encUsPeak, micros() - t0);
}

// Decode one frame. Never fails: a corrupted frame produces noise, not an
// error, because Codec2 has no way to know its input was damaged. Keeping the
// LoRa CRC on is what stops that noise reaching the speaker.
void codecDecode(int16_t* samples, const uint8_t* bits) {
  if (c2 == nullptr) {
    memset(samples, 0, sizeof(int16_t) * samplesPerFrame);
    return;
  }
  uint32_t t0 = micros();
  codec2_decode(c2, (short*)samples, bits);
  accumulate(decUsAvg, decUsPeak, micros() - t0);
}

uint32_t codecEncodeUs()     { return encUsAvg; }
uint32_t codecDecodeUs()     { return decUsAvg; }
uint32_t codecEncodePeakUs() { return encUsPeak; }
uint32_t codecDecodePeakUs() { return decUsPeak; }
