// =============================================================================
// schedule.h - the shared notion of "what should be on the air right now".
//
// Two parts:
//
//   1. A monotonic UTC clock. Neither board can trust millis() alone (no epoch)
//      or time() alone (1-second resolution). So we keep an anchor: an epoch
//      instant paired with the millis() reading at that instant, and derive
//      millisecond-resolution UTC from the pair. millis() overflow is handled
//      by doing the subtraction in uint32_t, which wraps correctly.
//
//   2. Pure slot arithmetic on top of it. Both boards run identical code here,
//      which is exactly why they stay in step without ever negotiating.
//
// Slot layout inside one ROUND_PERIOD_MS round - see config.h for the diagram.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "config.h"

enum ClockSource {
  CLOCK_SOURCE_NONE = 0,
  CLOCK_SOURCE_NTP,
  CLOCK_SOURCE_GPS,
  CLOCK_SOURCE_PACKET,   // bootstrapped from a received packet
};

// ---- clock -------------------------------------------------------------------

// Anchor the clock. `epochMs` is UTC milliseconds since 1970; `atMillis` is the
// millis() value that corresponds to it (pass millis() if it is happening now,
// or millis() - age if the reading is known to be `age` ms stale).
void clockSet(uint64_t epochMs, uint32_t atMillis, ClockSource source);

bool        clockValid();
const char* clockSourceName();

// UTC milliseconds since 1970. Returns 0 when the clock has never been set.
uint64_t clockNowMs();

// Seconds since the clock was last anchored - a staleness indicator for the UI.
uint32_t clockAgeS();

// "2026-08-22T14:03:07.412Z" into a caller-provided buffer (needs >= 25 bytes).
void clockFormatIso(uint64_t epochMs, char* out, size_t outSize);

// ---- slot arithmetic ---------------------------------------------------------

// Check that every profile's airtime actually fits the share of the slot its
// repeats are given, and report the schedule at boot so the two boards can be
// compared at a glance. Purely a validator: the offsets themselves are a fixed
// function of the config and need no airtime knowledge at all.
void scheduleReportAirtimes(const uint32_t* airtimeMs, uint8_t count);

// Round counter: epoch seconds divided by the round period. Monotonic, shared.
uint32_t scheduleRoundIndex(uint64_t epochMs);

// Low 16 bits of the above - what travels in the packet.
uint16_t scheduleRoundId(uint64_t epochMs);

// UTC ms at which the round containing `epochMs` began.
uint64_t scheduleRoundStartMs(uint64_t epochMs);

// Milliseconds elapsed inside the current round, 0 .. ROUND_PERIOD_MS-1.
uint32_t scheduleOffsetInRoundMs(uint64_t epochMs);

// Which profile owns the moment `epochMs`. Always valid, 0 .. COUNT-1.
uint8_t scheduleSlotIndex(uint64_t epochMs);

// Offset within the round at which `slot` begins.
uint32_t scheduleSlotStartMs(uint8_t slot);

// Offset within the round at which repeat `repeat` of `slot` is transmitted.
uint32_t scheduleTxOffsetMs(uint8_t slot, uint8_t repeat);

// Absolute UTC ms of that transmission, within the round containing `epochMs`.
uint64_t scheduleTxTimeMs(uint64_t epochMs, uint8_t slot, uint8_t repeat);
