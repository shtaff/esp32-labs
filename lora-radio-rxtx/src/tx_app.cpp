// =============================================================================
// tx_app.cpp - transmitter. Compiled only into the `tx` environment.
//
// The transmitter has exactly one job: put the right packet on the air at the
// right UTC instant. It gets UTC from NTP over WiFi, then powers WiFi down and
// runs off the anchored clock in schedule.cpp, re-syncing every
// NTP_RESYNC_INTERVAL_S during the idle tail of a round.
//
// It deliberately refuses to transmit before it has a clock. An unsynchronised
// transmitter would only be spraying packets into slots the receiver is not
// listening on, which produces confusing data rather than no data.
// =============================================================================
#include <Arduino.h>

#if defined(APP_MODE_TX)

#include <WiFi.h>
#include <time.h>
#include <sys/time.h>

#include "app.h"
#include "config.h"
#include "packet.h"
#include "radio_hw.h"
#include "radio_profiles.h"
#include "schedule.h"
#include "storage.h"
#include "ui.h"

namespace {

const uint8_t  kEventsPerRound = RADIO_PROFILE_COUNT * REPEATS_PER_PROFILE;

// How early we start getting ready for a transmission, and how late an event
// can be before we abandon it (which happens after a boot mid-round).
const uint32_t kPreloadMs = 300;
const uint32_t kLateMs    = 250;

bool     g_radioOk       = false;
int16_t  g_appliedSlot   = -1;
uint32_t g_currentRound  = UINT32_MAX;
uint8_t  g_nextEvent     = 0;
uint32_t g_sequence      = 0;
uint32_t g_sent          = 0;
uint32_t g_failed        = 0;
uint32_t g_lastNtpSyncS  = 0;
char     g_statusLine[24] = "booting";

bool credentialsConfigured() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 &&
         strcmp(WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") != 0;
}

// Connect, sync from NTP, disconnect. Blocking, but only ever called while the
// schedule is idle, and never for longer than budgetMs.
//
// The budget matters. The idle tail of a round is only about 17 s at the
// default slot timing, so a fixed 35 s worth of timeouts would never fit and
// the transmitter would simply never re-sync - drifting roughly 72 ms an hour
// against the GPS-locked receiver until its packets fall outside the arrival
// window and get logged as losses that never happened.
bool syncTimeFromNtp(uint32_t budgetMs) {
  if (!credentialsConfigured()) {
    Serial.println("[ntp] WiFi credentials are still placeholders - see README");
    snprintf(g_statusLine, sizeof(g_statusLine), "no wifi cfg");
    return false;
  }

  Serial.printf("[ntp] connecting to %s\n", WIFI_SSID);
  snprintf(g_statusLine, sizeof(g_statusLine), "wifi...");
  uiInvalidate();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t deadline = millis() + budgetMs;

  // Two thirds of whatever we have goes to associating, the rest to the actual
  // time query, capped by the configured maxima.
  uint32_t connectBudget = budgetMs * 2 / 3;
  if (connectBudget > WIFI_CONNECT_TIMEOUT_MS) connectBudget = WIFI_CONNECT_TIMEOUT_MS;

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < connectBudget) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ntp] WiFi connect failed");
    snprintf(g_statusLine, sizeof(g_statusLine), "wifi fail");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // UTC, no DST offset - the whole schedule is in UTC.
  configTime(0, 0, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
  snprintf(g_statusLine, sizeof(g_statusLine), "ntp...");
  uiInvalidate();

  bool ok = false;
  start = millis();
  uint32_t queryBudget = (int32_t)(deadline - millis()) > 0
                       ? (uint32_t)(deadline - millis()) : 0;
  if (queryBudget > NTP_SYNC_TIMEOUT_MS) queryBudget = NTP_SYNC_TIMEOUT_MS;

  while ((millis() - start) < queryBudget) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec > 1700000000L) {   // any plausible post-2023 instant
      uint64_t epochMs = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
      clockSet(epochMs, millis(), CLOCK_SOURCE_NTP);
      g_lastNtpSyncS = (uint32_t)tv.tv_sec;
      ok = true;
      break;
    }
    delay(100);
  }

  if (ok) {
    char iso[32];
    clockFormatIso(clockNowMs(), iso, sizeof(iso));
    Serial.printf("[ntp] clock set: %s\n", iso);
    snprintf(g_statusLine, sizeof(g_statusLine), "synced");
  } else {
    Serial.println("[ntp] sync timed out");
    snprintf(g_statusLine, sizeof(g_statusLine), "ntp fail");
  }

  // WiFi off between syncs: it saves power and keeps the 2.4 GHz front end
  // quiet while we are making sub-microvolt measurements at 868 MHz.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return ok;
}

void applySlotProfile(uint8_t slot) {
  int16_t state = radioApplyProfile(radio, RADIO_PROFILES[slot]);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[tx] profile %u apply failed: %d\n", (unsigned)slot, state);
    g_radioOk = false;
    return;
  }
  g_radioOk     = true;
  g_appliedSlot = (int16_t)slot;
}

void transmitEvent(uint8_t slot, uint8_t repeat, uint64_t dueMs) {
  const RadioProfile& profile = RADIO_PROFILES[slot];
  uint32_t toaMs = radioTimeOnAirMs(radio);

  uint8_t buf[PACKET_LEN];
  packetBuild(buf,
              slot,
              repeat,
              scheduleRoundId(dueMs),
              (uint32_t)(dueMs / 1000ULL),
              (uint16_t)(dueMs % 1000ULL),
              g_sequence,
              toaMs);

  uint32_t t0 = millis();
  int16_t  state = radio.transmit(buf, PACKET_LEN);
  uint32_t measured = millis() - t0;

  if (state == RADIOLIB_ERR_NONE) {
    ++g_sent;
    Serial.printf("[tx] r%lu s%u/%u %-16s seq=%lu toa=%lums measured=%lums\n",
                  (unsigned long)scheduleRoundIndex(dueMs),
                  (unsigned)slot, (unsigned)repeat,
                  profile.label,
                  (unsigned long)g_sequence,
                  (unsigned long)toaMs,
                  (unsigned long)measured);
    // Minutes since the last NTP anchor. At ~20 ppm the clock drifts about
    // 72 ms an hour, so this is the number that says whether the schedule can
    // still be trusted against the guard time.
    snprintf(g_statusLine, sizeof(g_statusLine), "ok %lums clk%lum",
             (unsigned long)measured, (unsigned long)(clockAgeS() / 60));
  } else {
    ++g_failed;
    Serial.printf("[tx] r%lu s%u/%u %-16s FAILED code=%d\n",
                  (unsigned long)scheduleRoundIndex(dueMs),
                  (unsigned)slot, (unsigned)repeat,
                  profile.label, state);
    snprintf(g_statusLine, sizeof(g_statusLine), "err %d", state);
  }

  ++g_sequence;
  uiInvalidate();
}

void fillUiState(UiState& s) {
  memset(&s, 0, sizeof(s));
  s.txMode             = true;
  s.clockValid         = clockValid();
  s.clockSourceName    = clockSourceName();
  s.epochMs            = clockNowMs();
  s.roundIndex         = scheduleRoundIndex(s.epochMs);
  s.activeSlot         = scheduleSlotIndex(s.epochMs);
  s.activeProfileShort  = RADIO_PROFILES[s.activeSlot].shortLabel;
  s.storageBackend     = storageBackendName();
  s.storageFreeBytes   = storageFreeBytes();
  s.rowsWritten        = storageRowsWritten();
  s.txSent             = g_sent;
  s.txFailed           = g_failed;
  s.txStatusLine       = g_statusLine;
  s.txLat              = TX_SITE_LAT;
  s.txLon              = TX_SITE_LON;
}

}  // namespace

void appSetup() {
  // The banner is printed by setup() in main.cpp before we get here.
  Serial.printf("round=%lus slots=%u x %lums repeats=%u stride=%lums guard=%lums\n",
                (unsigned long)ROUND_PERIOD_S,
                (unsigned)RADIO_PROFILE_COUNT,
                (unsigned long)SLOT_MS,
                (unsigned)REPEATS_PER_PROFILE,
                (unsigned long)REPEAT_STRIDE_MS,
                (unsigned long)SLOT_GUARD_MS);

  // The transmitter does not use the coordinate for anything but the display -
  // it is never transmitted - but saying so here means a misconfigured pair is
  // caught at whichever board you happen to be looking at.
  if (txSiteConfigured()) {
    Serial.printf("TX site: %.6f, %.6f\n", (double)TX_SITE_LAT, (double)TX_SITE_LON);
  } else {
    Serial.println("*** TX SITE NOT SET - put real values in [site] of secrets.ini");
  }

  uiSplash("TX mode", "TTGO T3 v1.6.1", "SX1276 868MHz");

  g_radioOk = radioHwBegin();
  if (!g_radioOk) {
    uiSplash("TX mode", "RADIO FAIL", "check SPI wiring");
  }

  syncTimeFromNtp(WIFI_CONNECT_TIMEOUT_MS + NTP_SYNC_TIMEOUT_MS);
}

void appLoop() {
  storageShellPoll();

  UiState ui;
  fillUiState(ui);
  uiRender(ui);

  if (!g_radioOk) {
    delay(50);
    return;
  }

  if (!clockValid()) {
    // Retry every 30 s rather than hammering the access point.
    static uint32_t lastAttempt = 0;
    if (lastAttempt == 0 || (millis() - lastAttempt) > 30000UL) {
      lastAttempt = millis();
      syncTimeFromNtp(WIFI_CONNECT_TIMEOUT_MS + NTP_SYNC_TIMEOUT_MS);
    }
    delay(50);
    return;
  }

  uint64_t now   = clockNowMs();
  uint32_t round = scheduleRoundIndex(now);

  if (round != g_currentRound) {
    g_currentRound = round;
    g_nextEvent    = 0;
    Serial.printf("[tx] --- round %lu ---\n", (unsigned long)round);
  }

  // Keep the modem configured for whatever slot the wall clock is in, so the
  // reconfiguration never eats into the guard time before a transmission.
  uint8_t currentSlot = scheduleSlotIndex(now);
  if ((int16_t)currentSlot != g_appliedSlot) {
    applySlotProfile(currentSlot);
    if (!g_radioOk) {
      return;
    }
  }

  if (g_nextEvent >= kEventsPerRound) {
    // Idle tail of the round: a good moment for an NTP re-sync, provided there
    // is enough time left that it cannot spill into the next round's slot 0.
    uint32_t remaining = ROUND_PERIOD_MS - scheduleOffsetInRoundMs(now);
    uint32_t sinceSync = (uint32_t)(now / 1000ULL) - g_lastNtpSyncS;

    // Keep 2 s clear so the attempt cannot bleed into the next round's slot 0,
    // and do not bother below 6 s, which is not enough to associate.
    const uint32_t kTailMarginMs = 2000UL;
    const uint32_t kMinUsefulMs  = 6000UL;

    if (sinceSync >= NTP_RESYNC_INTERVAL_S && remaining > (kMinUsefulMs + kTailMarginMs)) {
      syncTimeFromNtp(remaining - kTailMarginMs);
    }
    delay(20);
    return;
  }

  uint8_t  slot   = g_nextEvent / REPEATS_PER_PROFILE;
  uint8_t  repeat = g_nextEvent % REPEATS_PER_PROFILE;
  uint64_t dueMs  = scheduleTxTimeMs(now, slot, repeat);

  if (now > dueMs + kLateMs) {
    // We booted (or re-synced) past this event. Skip it silently; the receiver
    // will book it as a loss, which is the honest outcome.
    ++g_nextEvent;
    return;
  }

  if (now + kPreloadMs >= dueMs) {
    if ((int16_t)slot != g_appliedSlot) {
      applySlotProfile(slot);
      if (!g_radioOk) {
        return;
      }
    }
    // Tight spin to the exact instant. At most kPreloadMs of busy waiting, and
    // only ever right before a transmission, so nothing else is starved.
    while (clockNowMs() < dueMs) {
      // no yield() here: this is the whole point of the spin
    }
    transmitEvent(slot, repeat, dueMs);
    ++g_nextEvent;
    return;
  }

  delay(5);
}

#endif  // APP_MODE_TX
