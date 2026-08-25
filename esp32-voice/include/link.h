// =============================================================================
// link.h - the radio: preset table, packet format, and the task that owns the
// SX1276.
//
// Exactly one task ever touches the radio (linkTask, in link.cpp). It sleeps
// on a task notification and is woken by three things: the DIO0 interrupt,
// which means the chip has finished sending or has a packet waiting; a nudge
// from voiceTask when there is something new to send; and a request to change
// preset. Nothing polls, and nothing calls into RadioLib from any other
// context - SPI plus a shared chip state machine is not a thing to share
// between tasks.
//
// -----------------------------------------------------------------------------
// PACKET FORMAT
//
//   offset  size  field
//   ------  ----  ------------------------------------------------------------
//     0      1    magic      VOICE_PKT_MAGIC. A one-byte filter for the other
//                            traffic on a public band, before anything else is
//                            trusted. Never encrypted.
//     1      1    flags      bit 0    payload is encrypted
//                            bit 1    last packet of this transmission
//                            bits 4-7 codec id, so a receiver configured for a
//                                     different bit rate can say so instead of
//                                     playing noise. Never encrypted.
//     2      4    streamId   random per PTT press, little endian. Identifies
//                            one transmission, resets the receiver's jitter
//                            buffer, and is part of the AES counter block.
//     6      2    seq        packet counter within the stream, little endian,
//                            from zero. Detects loss, and is part of the
//                            counter block.
//     8      1    station    who is talking. Derived from the board's MAC
//                            unless VOICE_STATION_ID pins it. Also folded into
//                            the counter block - see below.
//     9      ..   frames     VOICE_FRAMES_PER_PACKET codec frames, back to
//                            back. Encrypted as one run when armed.
//
// The header stays in clear because the receiver needs streamId, seq and
// station to build the counter block before it can decrypt anything, and
// because the magic and codec id have to be checkable on a packet that is not
// for us. That leaks who is talking to whom and how much - which, on a band
// where the preamble itself is a detectable event, was never hidden anyway.
//
// The station byte in the counter block is not decoration. streamId is a
// 32-bit random draw per press, so two handsets keying up independently have
// about a one-in-four-billion chance of choosing the same one; if they ever
// did, and both started at seq 0, they would encrypt two different packets
// with the same keystream and hand an eavesdropper both plaintexts XORed
// together. Mixing in a value that differs per board removes that case
// entirely, and costs one byte of a nonce that was full of zeros.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "config.h"

#define VOICE_PKT_MAGIC   0x56    // 'V'
#define VOICE_PKT_HEADER  9

#define VOICE_FLAG_ENCRYPTED  0x01
#define VOICE_FLAG_END        0x02
#define VOICE_FLAG_CODEC_MASK 0xF0
#define VOICE_FLAG_CODEC_SHIFT 4

// Largest packet the link will ever build, so buffers are fixed size.
//
// This has to stay under 64 bytes or the FSK presets stop working: the SX1276's
// FSK FIFO is 64 bytes, and RadioLib refuses anything that does not fit in one
// go rather than managing FIFO refills mid-packet. link.cpp has a
// static_assert on it - if you raise VOICE_FRAMES_PER_PACKET far enough to
// trip it, you have to either lower it again or delete the FSK presets.
#define VOICE_PKT_MAX (VOICE_PKT_HEADER + \
                       VOICE_FRAMES_PER_PACKET * VOICE_MAX_BYTES_PER_FRAME)

// -----------------------------------------------------------------------------
// Presets
//
// A preset is a whole operating point, not just a frequency: the modem, its
// parameters, the transmit power the sub-band allows, and the duty-cycle limit
// that applies there. The mode button steps through the table.
//
// PRESET_SELFTEST is the odd one out - it has no radio settings at all,
// because on it the radio sits in standby and PTT records and plays back
// locally instead of transmitting. See runSelfTest() in app.cpp.
// -----------------------------------------------------------------------------
enum PresetKind {
  PRESET_RADIO = 0,
  PRESET_SELFTEST,
};

enum ModemKind {
  MODEM_LORA = 0,
  MODEM_FSK,
};

struct VoicePreset {
  const char* label;      // short, for the display: "868.1 LoRa"
  const char* note;       // one line of why you would pick it
  PresetKind  kind;
  ModemKind   modem;
  float       freqMHz;
  int8_t      powerDbm;   // what the sub-band allows, capped by the chip
  VoiceBand   band;       // which rolling-hour budget this spends
  float       dutyLimit;  // percent, from EN 300 220 for that sub-band
};

// The table lives in link.cpp. Index with a preset number; out-of-range wraps.
const VoicePreset& linkPreset(uint8_t index);

// -----------------------------------------------------------------------------
// Counters. Written by linkTask, read by uiTask and the console without a
// lock: they are 32-bit scalars on a 32-bit machine, every one of them is
// display-only, and a torn read costs a wrong number on one 250 ms frame.
// -----------------------------------------------------------------------------
struct LinkStats {
  uint32_t packetsTx;      // packets actually put on the air
  uint32_t packetsRx;      // packets accepted and handed to the audio path
  uint32_t txDropped;      // outbound packets discarded because the queue was
                           // full - the transmitter is not keeping up
  uint32_t rxForeign;      // wrong magic: somebody else's traffic, or noise
                           // that got through the CRC
  uint32_t rxCodecMismatch;// right magic, wrong codec id - the far end is
                           // built for a different bit rate
  uint32_t rxNoKey;        // encrypted traffic arriving with no key loaded.
                           // Its own counter because it is its own mistake:
                           // one handset armed and the other not looks exactly
                           // like a dead link otherwise
  uint32_t rxMalformed;    // length not a whole number of codec frames
  uint32_t rxErrors;       // CRC failures and RadioLib read errors
  uint32_t rxLost;         // packets missing from a stream, by sequence gap
  float    lastRssi;       // dBm, last accepted packet
  float    lastSnr;        // dB, last accepted packet. LoRa only - the SX1276
                           // has no SNR estimator in FSK mode, so this is left
                           // at zero there and the display says so
  uint8_t  lastStation;    // who sent the last accepted packet
  uint32_t txAirtimeMs;    // cumulative, since boot, across all bands
};

// Brings up SPI and the SX1276 and starts linkTask. False means the radio did
// not answer - almost always wiring or a dead module.
bool linkBegin();

// This board's station id, 1-254. Derived from the factory MAC unless
// VOICE_STATION_ID pins it, so two boards flashed from one image still differ.
uint8_t linkStationId();

// -----------------------------------------------------------------------------
// Preset and encryption. Both are set from the button task and read
// everywhere; both take effect on the next packet.
// -----------------------------------------------------------------------------

// Switches preset. Safe at any time: it hands the request to linkTask rather
// than touching the chip, so it cannot collide with a transmission in
// progress. Because it is asynchronous, linkPresetIndex() lags this call by up
// to a few milliseconds - report the index you asked for, not the current one.
void    linkSetPreset(uint8_t index);
uint8_t linkPresetIndex();
const VoicePreset& linkCurrentPreset();

// True when the active preset is the local self test, i.e. the radio is parked
// and PTT means "record" rather than "transmit".
bool linkSelfTestMode();

// Arming with no key configured is refused and returns false, so the display
// can never show ENC while sending in clear.
bool linkSetEncryption(bool on);
bool linkEncryption();

// -----------------------------------------------------------------------------
// Transmit
// -----------------------------------------------------------------------------

// Starts a new transmission: fresh random streamId, sequence back to zero.
void linkStreamBegin();

// Queues one packet's worth of codec frames. `frames` is count *
// codecBytesPerFrame() bytes. False means either the outbound queue was full
// and the OLDEST queued packet was dropped to make room - in a live voice
// stream stale audio is worse than missing audio - or the active preset is the
// self test, which has no radio to send on.
bool linkSendFrames(const uint8_t* frames, uint8_t count, bool endOfStream);

// True while a packet is on the air or waiting to be. voiceTask uses this to
// hold the microphone open until the tail of a transmission has actually gone
// out, rather than cutting the last word off.
bool linkTxBusy();

// Blocks until linkTxBusy() clears or the timeout expires.
void linkWaitTxIdle(uint32_t timeoutMs);

// -----------------------------------------------------------------------------
// Receive
//
// linkTask decrypts each packet, splits it into codec frames and pushes them
// into a queue. voiceTask pops them one at a time; the queue is the jitter
// buffer, and its depth in frames is the whole of the buffering strategy.
// -----------------------------------------------------------------------------

// Pops one codec frame. dst needs codecBytesPerFrame() bytes. False on
// timeout, which is how voiceTask notices a transmission has ended.
bool linkTakeRxFrame(uint8_t* dst, uint32_t timeoutMs);

// Frames currently buffered. voiceTask waits for the pre-roll before it starts
// playing, and watches this for underruns afterwards.
uint16_t linkRxFrameCount();

// Throws away buffered audio. Called when PTT interrupts an incoming
// transmission: the moment you key up, what the other station said is stale.
void linkFlushRx();

// millis() of the last accepted packet, and whether it was encrypted. The
// second one is what puts the padlock on the display for RECEIVED traffic,
// which is a different question from whether we are transmitting encrypted.
uint32_t linkLastRxMs();
bool     linkRxWasEncrypted();

// True once a packet arrived carrying VOICE_FLAG_END. voiceTask uses it to
// stop as soon as the far end let go, instead of waiting out the timeout.
// Cleared by linkFlushRx() and by the start of any new stream.
bool linkRxStreamEnded();

// -----------------------------------------------------------------------------
// Measurements
// -----------------------------------------------------------------------------

// Airtime of one full packet on the ACTIVE preset, as measured by RadioLib
// rather than computed here. Recalculated on every preset change, because
// changing modem changes it by an order of magnitude.
uint32_t linkAirtimeMs();

// Duty while keyed, percent: airtime per packet against the audio each packet
// carries. This is a property of the configuration, not a measurement of
// behaviour - it is what the duty cycle WOULD be if you never let go of PTT.
uint8_t linkKeyedDutyPercent();

// Transmit duty cycle over the last rolling hour, for the sub-band the active
// preset sits in. Separate budgets per band, because that is how EN 300 220
// assesses it - time spent on 868.1 does not count against 869.525.
float linkDutyPercent();

// The limit that applies to the active preset's sub-band, percent.
float linkDutyLimit();

// Seconds of talking left in this hour before the active preset's sub-band
// budget is spent, given how much airtime each second of speech costs.
//
// This is the number that means something in the hand. "0.42 % of 1 %" needs
// arithmetic; "39 s left" does not. Returns a large number on the 100 % band,
// where the budget is not a real constraint.
uint32_t linkTalkSecondsLeft();

const LinkStats& linkStats();
