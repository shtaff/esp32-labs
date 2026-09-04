// =============================================================================
// audio.h - the I2S path: INMP441 in, MAX98357A out.
//
// One I2S peripheral, reconfigured on every direction change. See the pin
// discussion in config.h for why, and audio.cpp for what that costs.
//
// Everything here is called from voiceTask only. audioBegin() is the exception
// and runs once, from setup(), before any task exists.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

enum AudioDir {
  AUDIO_DIR_OFF = 0,   // driver uninstalled, no clocks, amplifier held silent
  AUDIO_DIR_MIC,       // I2S RX: INMP441 -> ESP32
  AUDIO_DIR_SPK,       // I2S TX: ESP32 -> MAX98357A
};

// Probes for a microphone as a side effect - see audioMicPresent(). Returns
// false only if the I2S driver itself refused to install, which is fatal.
bool audioBegin();

// True if an INMP441 answered the probe at boot.
//
// The probe is a real measurement, not a configuration flag: with no part
// fitted, GPIO34 has nothing driving it and every sample comes back identical.
// A live INMP441 always delivers at least a couple of LSBs of self-noise.
bool audioMicPresent();

// False when the build asked for no amplifier (-DNO_AMP). There is no way to
// detect a MAX98357A - it has no readback path - so an absent amplifier is
// simply inaudible, which costs nothing and needs no special case.
bool audioAmpEnabled();

// Reconfigures the peripheral. Idempotent; a no-op if already in that
// direction. Switching into AUDIO_DIR_MIC includes the INMP441 settling delay,
// so it blocks for about VOICE_MIC_SETTLE_MS.
void audioSetDirection(AudioDir dir);
AudioDir audioDirection();

// Blocks until one whole codec frame of microphone samples is available, then
// returns them gain-corrected and saturated to int16.
//
// With no microphone fitted this synthesises a test signal instead, at the
// same rate and with the same blocking behaviour, so the transmit path is
// exercised identically. See audioTestSignalActive().
//
// dst must have room for codecSamplesPerFrame() samples. False means the DMA
// read timed out, which should not happen and is counted.
bool audioCaptureFrame(int16_t* dst);

// Queues one codec frame for playback. i2s_write() blocks once the DMA ring is
// full, and that back-pressure is what paces playback at exactly 8 kHz - there
// is no timer anywhere in the receive path.
void audioPlayFrame(const int16_t* src);

// -----------------------------------------------------------------------------
// Cue tones - the beeps that mark the edges of a transmission.
//
// See the cue-tone section of config.h for what each one means and, more to
// the point, which END of the link generates it. The short version: none of
// them is transmitted. ROGER and LOST are synthesised by the LISTENING handset
// from the end flag that is already in the packet header, which is what lets a
// station that dropped off the air sound different from one that signed off.
// -----------------------------------------------------------------------------
enum AudioCue {
  AUDIO_CUE_KEYUP = 0,  // local: you keyed up, go ahead
  AUDIO_CUE_INCOMING,   // someone has started transmitting
  AUDIO_CUE_ROGER,      // they finished cleanly - your turn
  AUDIO_CUE_LOST,       // they stopped without signing off - you missed the end
};

// Plays one cue and does not return until it has actually come out of the
// speaker.
//
// It switches the peripheral to AUDIO_DIR_SPK itself, so a caller does not
// have to think about direction - but that means a cue in the transmit path
// costs a direction change back to the microphone afterwards.
//
// The wait at the end is not politeness: i2s_write() returns when a sample is
// queued to DMA, not when it has been played, so returning immediately would
// let the caller uninstall the driver and truncate the beep. The most obvious
// symptom of getting that wrong is a roger beep that is audible on a slow
// board and silent on a fast one.
//
// Does nothing when cues are switched off or the build has no amplifier.
void audioPlayCue(AudioCue cue);

// Cue tones on or off at runtime, from the `beep` console command. The
// build-time default is VOICE_CUE_TONES.
void audioSetCues(bool on);
bool audioCuesEnabled();

// True when audioCaptureFrame() is synthesising rather than listening.
bool audioTestSignalActive();

// Forces the synthetic signal on even with a microphone fitted, which is how
// you send a known, repeatable stimulus down the link while you measure the
// other end. The `tone` console command drives this.
void audioForceTestSignal(bool on);

// Which of the two I2S slots the microphone was found on, and how confident
// the probe was, for the AUDIO screen and the boot log. See audio.cpp for what
// the probe actually looks at - it is not a level threshold.
uint8_t audioMicSlot();
uint8_t audioMicConfidence();   // percent of samples with the I2S signature

// Peak level of the last frame through either direction, 0-100, with a decay
// so the OLED bar is readable. Drives the VU meter, and it is the only
// feedback a board with no amplifier gets that the far end is really talking.
uint8_t audioLevel();
void    audioResetLevel();

// Times audioCaptureFrame() found no data ready. Non-zero means the codec is
// not keeping up with the microphone; check codecEncodeUs().
uint32_t audioCaptureTimeouts();

// Microphone samples that saturated on the way to the codec.
//
// This is the number that separates "the microphone is broken" from "you are
// shouting into it", which sound identical and are fixed very differently. A
// climbing count while you talk means VOICE_MIC_GAIN_SHIFT is too high, or the
// microphone is too close to your mouth. Zero, with a VU bar that barely
// moves, means the opposite.
uint32_t audioClipCount();
void     audioResetClipCount();

// The rate the I2S peripheral is actually clocking the microphone at, which is
// VOICE_MIC_OVERSAMPLE times the codec's 8 kHz. See the oversampling section
// of config.h for why it is not simply 8 kHz.
uint32_t audioCaptureRate();
