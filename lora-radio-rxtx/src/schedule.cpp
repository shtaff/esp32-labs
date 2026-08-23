#include "schedule.h"

#include <Arduino.h>
#include <time.h>
#include <stdio.h>
#include <sys/time.h>

namespace {

uint64_t    g_anchorEpochMs = 0;
uint32_t    g_anchorMillis  = 0;
ClockSource g_source        = CLOCK_SOURCE_NONE;
bool        g_valid         = false;

}  // namespace

void clockSet(uint64_t epochMs, uint32_t atMillis, ClockSource source) {
  g_anchorEpochMs = epochMs;
  g_anchorMillis  = atMillis;
  g_source        = source;
  g_valid         = true;

  // Mirror into the C library clock too, so anything using time()/gmtime()
  // (including log formatting and the Arduino core) agrees with us.
  struct timeval tv;
  tv.tv_sec  = (time_t)(epochMs / 1000ULL);
  tv.tv_usec = (suseconds_t)((epochMs % 1000ULL) * 1000ULL);
  settimeofday(&tv, nullptr);
}

bool clockValid() {
  return g_valid;
}

const char* clockSourceName() {
  switch (g_source) {
    case CLOCK_SOURCE_NTP:    return "NTP";
    case CLOCK_SOURCE_GPS:    return "GPS";
    case CLOCK_SOURCE_PACKET: return "PKT";
    default:                  return "none";
  }
}

uint64_t clockNowMs() {
  if (!g_valid) {
    return 0;
  }
  // Deliberate uint32_t subtraction: it yields the correct elapsed time even
  // across the ~49.7 day millis() rollover.
  uint32_t elapsed = (uint32_t)millis() - g_anchorMillis;
  return g_anchorEpochMs + (uint64_t)elapsed;
}

uint32_t clockAgeS() {
  if (!g_valid) {
    return 0;
  }
  return ((uint32_t)millis() - g_anchorMillis) / 1000UL;
}

void clockFormatIso(uint64_t epochMs, char* out, size_t outSize) {
  if (epochMs == 0) {
    snprintf(out, outSize, "                        ");
    out[0] = '\0';
    return;
  }
  time_t secs = (time_t)(epochMs / 1000ULL);
  unsigned ms = (unsigned)(epochMs % 1000ULL);

  struct tm tmUtc;
  gmtime_r(&secs, &tmUtc);

  snprintf(out, outSize, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
           tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
           tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec, ms);
}

// -----------------------------------------------------------------------------
// Slot arithmetic. All of it is a pure function of UTC, which is the whole
// trick: the two boards never exchange scheduling information, they just both
// compute the same answer from the same clock.
// -----------------------------------------------------------------------------

uint32_t scheduleRoundIndex(uint64_t epochMs) {
  return (uint32_t)(epochMs / (uint64_t)ROUND_PERIOD_MS);
}

uint16_t scheduleRoundId(uint64_t epochMs) {
  return (uint16_t)(scheduleRoundIndex(epochMs) & 0xFFFFU);
}

uint64_t scheduleRoundStartMs(uint64_t epochMs) {
  return (uint64_t)scheduleRoundIndex(epochMs) * (uint64_t)ROUND_PERIOD_MS;
}

uint32_t scheduleOffsetInRoundMs(uint64_t epochMs) {
  return (uint32_t)(epochMs % (uint64_t)ROUND_PERIOD_MS);
}

uint8_t scheduleSlotIndex(uint64_t epochMs) {
  return (uint8_t)(scheduleOffsetInRoundMs(epochMs) / SLOT_MS);
}

uint32_t scheduleSlotStartMs(uint8_t slot) {
  return (uint32_t)slot * (uint32_t)SLOT_MS;
}

void scheduleReportAirtimes(const uint32_t* airtimeMs, uint8_t count) {
  if (count > RADIO_PROFILE_COUNT) {
    count = RADIO_PROFILE_COUNT;
  }

  Serial.printf("[sched] slot %lums, %u repeats, stride %lums, guard %lums\n",
                (unsigned long)SLOT_MS, (unsigned)REPEATS_PER_PROFILE,
                (unsigned long)REPEAT_STRIDE_MS, (unsigned long)SLOT_GUARD_MS);
  Serial.print("[sched] transmit offsets within every slot (ms):");
  for (uint8_t r = 0; r < REPEATS_PER_PROFILE; ++r) {
    Serial.printf(" %lu", (unsigned long)(SLOT_GUARD_MS + (uint32_t)r * REPEAT_STRIDE_MS));
  }
  Serial.println();

  // The binding constraint is that consecutive repeats' arrival windows must
  // not overlap. Each window spans the airtime plus ARRIVAL_TOLERANCE_MS either
  // side, so a stride has to clear:
  //
  //     airtime + 2 * ARRIVAL_TOLERANCE_MS
  //
  // Overlapping windows would let one packet satisfy the wrong repeat and the
  // loss figures would become fiction, so this is worth shouting about rather
  // than mis-measuring quietly. It also subsumes the weaker requirement that a
  // transmission simply finish inside its own stride.
  for (uint8_t slot = 0; slot < count; ++slot) {
    uint32_t needed = airtimeMs[slot] + 2UL * ARRIVAL_TOLERANCE_MS;
    if (needed >= REPEAT_STRIDE_MS) {
      Serial.printf("[sched] WARNING slot %u needs a stride over %lums "
                    "(airtime %lums + 2x tolerance) but has %lums. Raise "
                    "SLOT_MS, or lower REPEATS_PER_PROFILE or "
                    "ARRIVAL_TOLERANCE_MS.\n",
                    (unsigned)slot, (unsigned long)needed,
                    (unsigned long)airtimeMs[slot],
                    (unsigned long)REPEAT_STRIDE_MS);
    }
  }
}

// Offsets are a pure function of the configuration - no airtime, no state, no
// initialisation order to get wrong. Every profile uses the same ones, which is
// why the two boards agree without exchanging anything.
uint32_t scheduleTxOffsetMs(uint8_t slot, uint8_t repeat) {
  return scheduleSlotStartMs(slot) + SLOT_GUARD_MS
       + (uint32_t)repeat * REPEAT_STRIDE_MS;
}

uint64_t scheduleTxTimeMs(uint64_t epochMs, uint8_t slot, uint8_t repeat) {
  return scheduleRoundStartMs(epochMs) + scheduleTxOffsetMs(slot, repeat);
}
