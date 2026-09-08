// =============================================================================
// post.h - power-on self test.
//
// Runs once at the end of boot, checks eight things, and reports each as PASS,
// WARN or FAIL to both the serial port and the event log.
//
// -----------------------------------------------------------------------------
// THE RULE THIS FOLLOWS
//
// A self test that reports what it configured rather than what it measured is
// worse than no self test, because it manufactures confidence. Every item here
// is one of:
//
//   MEASURED    a real reading from hardware  (battery volts, codec timing,
//               microphone signature, radio register readback)
//   OBSERVED    the result of something that already happened and could have
//               failed  (radio init, display probe, config CRC)
//   ASSERTED    we know it only because we were told  (the amplifier)
//
// The ASSERTED ones say so in their detail line. There is exactly one - the
// MAX98357A has no readback path of any kind - and pretending otherwise would
// be the single most misleading thing this file could do.
//
// -----------------------------------------------------------------------------
// PASS / WARN / FAIL
//
//   FAIL  the handset cannot do its job. Radio dead, codec absent.
//   WARN  it will work, but not as intended, or not for long. No microphone,
//         battery low, no key when encryption is configured on.
//   PASS  measured and within range.
//
// A WARN is never upgraded to a FAIL just because it is inconvenient: a board
// with no microphone is a perfectly good receiver, and this rig is often
// deliberately run that way.
//
// See docs/04-post.md.
// =============================================================================
#pragma once

#include <stdint.h>

enum PostItem {
  POST_CONFIG = 0,   // settings loaded and passed CRC
  POST_LOG,          // event ring intact
  POST_CODEC,        // Codec2 created, and fast enough on this silicon
  POST_RADIO,        // SX1276 answered and configured
  POST_MIC,          // INMP441 found by its I2S signature
  POST_AMP,          // audio output path - see the ASSERTED note above
  POST_POWER,        // battery / supply voltage, measured
  POST_MEMORY,       // free heap and the tightest task stack
  POST_ITEM_COUNT,
};

enum PostResult {
  POST_PASS = 0,
  POST_WARN,
  POST_FAIL,
  POST_SKIP,         // not applicable to this build or configuration
};

// Runs every check. Call it after all the subsystems are up, because most of
// what it reports is the outcome of their initialisation.
void postRun();

// True when at least one item failed. `postWarnings()` counts the WARNs.
bool     postFailed();
uint8_t  postFailures();
uint8_t  postWarnings();

// Per-item results, for the display and for `post` on the console.
PostResult  postItemResult(uint8_t item);
const char* postItemName(uint8_t item);
const char* postItemDetail(uint8_t item);   // the measurement, in words
const char* postResultName(PostResult r);

// One-line verdict for the VERSION screen: "POST 8 pass" or "POST 1 FAIL".
const char* postSummary();

// Battery / supply volts, measured on the divider at GPIO35. Zero if the
// reading was implausible - see post.cpp, this pin lies when USB-powered on
// some board revisions.
float postBatteryVolts();
