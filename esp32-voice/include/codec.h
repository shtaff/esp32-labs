// =============================================================================
// codec.h - the Codec2 speech codec, wrapped.
//
// One CODEC2 instance serves both directions. That is safe here only because
// the handset is half duplex and both codecEncode() and codecDecode() are
// called from the same task (voiceTask) - a Codec2 instance carries mutable
// analysis and synthesis state and is not thread safe. Nothing else may touch
// these functions.
//
// The frame geometry is queried from the library rather than hard-coded, so
// changing VOICE_CODEC_MODE in config.h is genuinely a one-line change; the
// only fixed numbers in the firmware are the buffer bounds in config.h.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// Creates the codec instance. False means codec2_create() refused the mode -
// almost always because VOICE_CODEC_MODE is not enabled in the
// CODEC2_MODE_*_EN flags in platformio.ini.
bool codecBegin();
bool codecReady();

// Geometry of one frame, for the mode actually in use.
int      codecSamplesPerFrame();   // 160 (3200) or 320 (1600, 700C)
int      codecBytesPerFrame();     // 8 (3200, 1600) or 4 (700C)
uint32_t codecFrameMs();           // 20 or 40
const char* codecModeName();       // "1600", "700C", ...

// A small dense id for the mode, carried in the packet header so a receiver
// built for a different bit rate can say so instead of playing noise. Not the
// Codec2 mode constant: those go up to 11 and would not fit in a nibble
// alongside anything else.
uint8_t     codecId();
const char* codecNameForId(uint8_t id);

// bits[] must have room for codecBytesPerFrame(), samples[] for
// codecSamplesPerFrame(). Both record their own execution time.
void codecEncode(uint8_t* bits, const int16_t* samples);
void codecDecode(int16_t* samples, const uint8_t* bits);

// Rolling mean execution time, microseconds. This is the number that decides
// whether a codec mode is usable on this hardware: encode has to finish inside
// one frame period (40 ms at 1600) with room for everything else, or the
// microphone DMA overruns and the audio stutters. Shown on the AUDIO screen.
uint32_t codecEncodeUs();
uint32_t codecDecodeUs();

// Worst case seen since boot. A mean that looks fine can still hide a
// pathological frame, and Codec2's pitch estimator does have bad days.
uint32_t codecEncodePeakUs();
uint32_t codecDecodePeakUs();
