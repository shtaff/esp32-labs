// =============================================================================
// app.h - the handset state machine.
//
// Three states and a single task (voiceTask) that owns the codec and the I2S
// peripheral. Keeping capture and playback in one task is not a shortcut: a
// Codec2 instance is not thread safe, and there is exactly one I2S peripheral
// to point in one direction at a time. Half duplex is not a limitation being
// worked around here, it is the design.
//
//                     PTT down
//        IDLE ----------------------> TX
//         ^  ^                        |
//         |  |   PTT up, tail sent    |
//         |  +------------------------+
//         |                           ^
//   stream ends or                    | PTT down (transmitting always wins;
//   goes quiet                        |  the far end is interrupted, exactly
//         |                           |  like every other walkie-talkie)
//         |          packet arrives   |
//         +--------> RX --------------+
//
// =============================================================================
#pragma once

#include <stdint.h>

enum VoiceState {
  VOICE_IDLE = 0,   // listening, radio in receive, I2S off
  VOICE_TX,         // PTT held: capture, encode, transmit
  VOICE_RX,         // a transmission is arriving: decode and play
  VOICE_TEST_REC,   // self-test preset, PTT held: capture and encode to RAM
  VOICE_TEST_PLAY,  // self-test preset, PTT released: decode and play it back
};

// Starts voiceTask and wires the button handler. Everything it depends on -
// codec, audio, link, buttons - must already be up.
void appBegin();

VoiceState  appState();
const char* appStateName();

// millis() at which the current state was entered. The display uses it to show
// how long a transmission has been running, which matters more than it sounds:
// it is the only feedback that stops you sitting on PTT for two minutes.
uint32_t appStateSinceMs();

// Frames of silence inserted because the jitter buffer ran dry mid-stream.
//
// This is the number that says whether VOICE_PREROLL_MS is big enough. A few
// at the very start of a transmission are normal; a steady climb through an
// over means the pre-roll is too short for the link, or the transmitter is not
// keeping up and the whole stream is arriving late.
uint32_t appUnderruns();

// Asks voiceTask to play all four cue tones, next time it is idle.
//
// A request rather than a call, because the console runs in the Arduino loop
// task and voiceTask owns the I2S peripheral and the codec outright. Playing
// them directly from the console would mean two tasks driving the same
// peripheral - and, worse, one of them reinstalling the driver underneath the
// other mid-transmission. The one-owner rule is the thing keeping the audio
// path simple; a debug convenience is not a reason to put a hole in it.
void appRequestCueDemo();

// Milliseconds of audio held in the self-test buffer, and how far through
// playing it back we are. Both zero outside the self-test states. The display
// uses them for the REC / PLAY counters, which is the only feedback that the
// recording is running and not simply hung.
uint32_t appSelfTestRecordedMs();
uint32_t appSelfTestPlayedMs();
