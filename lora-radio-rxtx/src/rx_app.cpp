// =============================================================================
// rx_app.cpp - receiver. Compiled only into the `rx` environment.
//
// The receiver follows the same UTC slot schedule as the transmitter, which is
// what makes the interesting measurement possible: because it knows which
// packets *should* arrive in the slot it is currently listening on, it can log
// a row for a packet that never came. A receiver that only logged what it heard
// could never tell you the packet loss rate.
//
// Reception is interrupt-driven (DIO0 -> RxDone). The loop stays free to pump
// the GPS, the serial shell and the display, and the arrival instant is
// captured in the ISR rather than whenever the loop happens to notice.
//
// Clock sources, in order of preference:
//   GPS     - normal operation, and the reason the receiver needs no WiFi
//   packet  - bootstrap: a single received packet carries the transmitter's UTC
//   none    - the receiver parks on BOOTSTRAP_PROFILE_INDEX and just listens
// =============================================================================
#include <Arduino.h>

#if defined(APP_MODE_RX)

#include <string.h>

#include "app.h"
#include "config.h"
#include "gps_module.h"
#include "packet.h"
#include "radio_hw.h"
#include "radio_profiles.h"
#include "schedule.h"
#include "storage.h"
#include "ui.h"

namespace {

// ---- interrupt ---------------------------------------------------------------

volatile bool     g_irqFlag   = false;
volatile uint32_t g_irqMillis = 0;

void IRAM_ATTR onPacketReceived() {
  g_irqMillis = millis();
  g_irqFlag   = true;
}

// ---- per-slot expectation bookkeeping ---------------------------------------

struct RepeatState {
  bool received;    // a valid, in-slot packet arrived for this repeat
  bool evaluated;   // its arrival window has closed and a row has been written

  // Something identifiable turned up for this repeat, whether or not it was
  // usable - a good packet, a CRC failure, a foreign frame. Kept separate from
  // `received` because it answers a different question: `received` drives the
  // success tally, `accounted` stops the miss evaluator writing a second row
  // for a transmission that already has one. Without it, one damaged packet
  // produces both a crc_error row and a fail row, and anything counting rows
  // double-counts the loss.
  bool accounted;
};

RepeatState g_repeats[REPEATS_PER_PROFILE];

// ---- round tallies, for the display ------------------------------------------

UiProfileResult g_thisRound[RADIO_PROFILE_COUNT];
UiProfileResult g_lastRound[RADIO_PROFILE_COUNT];
bool            g_lastRoundValid = false;
uint32_t        g_lastRoundIndex = 0;

// ---- general state -----------------------------------------------------------

bool     g_radioOk      = false;
int16_t  g_appliedSlot  = -1;
uint32_t g_currentRound = UINT32_MAX;
uint32_t g_slotToaMs    = 0;

// UTC instant at which we actually started listening on the current slot. Used
// to suppress bogus misses when the board boots, or unlocks, part way through a
// slot: a packet we were never configured to hear is not a lost packet.
uint64_t g_slotEnteredMs = 0;

bool    g_profileLocked = false;
uint8_t g_lockedSlot    = 0;

uint32_t g_lastGpsLogS = 0;

// Last-packet detail for the UI.
bool     g_lastPacketValid = false;
char     g_lastPacketLabel[20] = "";
float    g_lastRssi = 0.0f;
float    g_lastSnr  = 0.0f;
bool     g_lastSnrValid = false;
uint32_t g_lastToaMs = 0;
int32_t  g_lastOffsetMs = 0;
uint32_t g_lastSequence = 0;

char g_paramsBuf[40] = "";

// -----------------------------------------------------------------------------

void resetRepeatState() {
  for (uint8_t i = 0; i < REPEATS_PER_PROFILE; ++i) {
    g_repeats[i].received  = false;
    g_repeats[i].evaluated = false;
    g_repeats[i].accounted = false;
  }
}

void resetRoundTallies(UiProfileResult* dst) {
  for (uint8_t i = 0; i < RADIO_PROFILE_COUNT; ++i) {
    dst[i].received = 0;
    dst[i].expected = REPEATS_PER_PROFILE;
    dst[i].bestRssi = 0.0f;
    dst[i].hasRssi  = false;
  }
}

// Apply a profile and go straight back into continuous receive. begin() resets
// the chip, so the interrupt action has to be re-registered every time.
void applySlotProfile(uint8_t slot) {
  int16_t state = radioApplyProfile(radio, RADIO_PROFILES[slot]);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[rx] profile %u apply failed: %d\n", (unsigned)slot, state);
    g_radioOk = false;
    return;
  }

  radio.setPacketReceivedAction(onPacketReceived);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[rx] startReceive failed: %d\n", state);
    g_radioOk = false;
    return;
  }

  g_radioOk       = true;
  g_appliedSlot   = (int16_t)slot;
  g_slotToaMs     = radioTimeOnAirMs(radio);
  g_irqFlag       = false;
  g_slotEnteredMs = clockNowMs();

  radioProfileParams(RADIO_PROFILES[slot], g_paramsBuf, sizeof(g_paramsBuf));

  Serial.printf("[rx] listening: %-16s %s toa=%lums\n",
                RADIO_PROFILES[slot].label, g_paramsBuf,
                (unsigned long)g_slotToaMs);
  uiInvalidate();
}

// Populate the fields every row shares, whether it is a hit or a miss.
void fillCommonRow(RxLogRow& row, uint8_t slot, uint8_t repeat, uint64_t epochMs) {
  const RadioProfile& p = RADIO_PROFILES[slot];
  GpsSnapshot gps = gpsSnapshot();

  memset(&row, 0, sizeof(row));
  row.epochMs      = epochMs;
  row.roundIndex   = scheduleRoundIndex(epochMs);
  row.slot         = slot;
  row.repeat       = repeat;
  row.modem        = p.isLoRa ? "lora" : "fsk";
  row.profileLabel = p.label;
  row.params       = g_paramsBuf;
  row.toaCalcMs    = g_slotToaMs;

  row.txLat = TX_SITE_LAT;
  row.txLon = TX_SITE_LON;

  // Satellite count, HDOP and fix age are recorded even without a usable
  // position: they are what tells you whether a row with no coordinates was
  // taken under a bridge or with the antenna unplugged.
  row.gpsValid      = gps.locationValid;
  row.gpsSatellites = gps.satellites;
  row.gpsHdop       = gps.hdop;
  row.gpsAgeMs      = gps.locationAgeMs;

  if (gps.locationValid) {
    row.rxLat        = gps.lat;
    row.rxLon        = gps.lon;
    row.gpsAltitudeM = gps.altitudeM;
    row.gpsSpeedKmh  = gps.speedKmh;
    row.gpsCourseDeg = gps.courseDeg;
    row.distanceM    = gpsDistanceMeters(gps.lat, gps.lon, TX_SITE_LAT, TX_SITE_LON);
    row.bearingDeg   = gpsBearingDeg(gps.lat, gps.lon, TX_SITE_LAT, TX_SITE_LON);
  }
}

// A packet that was expected and never arrived. This is the row that makes the
// log usable as a loss measurement rather than just a reception diary.
void logMiss(uint8_t slot, uint8_t repeat, uint64_t deadlineMs) {
  RxLogRow row;
  fillCommonRow(row, slot, repeat, deadlineMs);
  row.outcome    = OUTCOME_FAIL;
  row.payloadHex = nullptr;
  row.payloadLen = 0;
  storageAppendRx(row);

  Serial.printf("[rx] MISS r%lu s%u/%u %s\n",
                (unsigned long)row.roundIndex,
                (unsigned)slot, (unsigned)repeat,
                RADIO_PROFILES[slot].label);
}

// Close out any expected arrival whose window has passed without a packet.
// Skipped while the profile lock is engaged and pointing somewhere other than
// the slot the schedule says is live, because in that case nothing is due.
void evaluateMisses(uint64_t now, uint8_t slot, bool scheduleAligned) {
  if (!scheduleAligned) {
    return;
  }
  for (uint8_t r = 0; r < REPEATS_PER_PROFILE; ++r) {
    if (g_repeats[r].evaluated) {
      continue;
    }
    uint64_t due      = scheduleTxTimeMs(now, slot, r);
    uint64_t deadline = due + g_slotToaMs + ARRIVAL_TOLERANCE_MS;
    if (now <= deadline) {
      continue;
    }
    g_repeats[r].evaluated = true;

    // If we only tuned in after this transmission had already started, we were
    // never in a position to hear it. Retire the expectation without logging a
    // loss we cannot honestly claim, and drop it from the round tally so the
    // display does not show it as a failure either.
    if (g_slotEnteredMs > due) {
      if (g_thisRound[slot].expected > 0) {
        --g_thisRound[slot].expected;
      }
      continue;
    }
    // `accounted`, not `received`: a repeat that arrived damaged already has
    // a crc_error / crc_filler / bad_payload row describing it. Adding a fail
    // row on top would describe the same transmission twice.
    if (!g_repeats[r].accounted) {
      logMiss(slot, r, deadline);
    }
  }
}

// Which repeat does an arrival at `rxMs` belong to? Used when the packet
// cannot be parsed and so cannot tell us itself: pick the repeat whose
// scheduled completion instant is nearest. The repeats are seconds apart, so
// this is unambiguous in practice.
uint8_t inferRepeat(uint64_t rxMs, uint8_t slot) {
  uint8_t  best      = 0;
  uint64_t bestDelta = UINT64_MAX;
  for (uint8_t r = 0; r < REPEATS_PER_PROFILE; ++r) {
    uint64_t end   = scheduleTxTimeMs(rxMs, slot, r) + g_slotToaMs;
    uint64_t delta = (rxMs > end) ? (rxMs - end) : (end - rxMs);
    if (delta < bestDelta) {
      bestDelta = delta;
      best      = r;
    }
  }
  return best;
}

// Read the packet the ISR told us about, classify it, log it.
void handleIrq(uint8_t slot, bool scheduleAligned) {
  noInterrupts();
  uint32_t irqMillis = g_irqMillis;
  g_irqFlag = false;
  interrupts();

  bool isLoRa = RADIO_PROFILES[slot].isLoRa;

  // RSSI must be sampled BEFORE readData(), and the two modems need different
  // calls:
  //
  //   LoRa - the chip latches a per-packet RSSI register at RxDone, so
  //          getRSSI() returns a true packet measurement (RadioLib also folds
  //          in a negative SNR, which is what lets LoRa report below the noise
  //          floor).
  //
  //   FSK  - there is no packet RSSI register. The bare getRSSI() would drop
  //          the chip into standby, re-enter receive, and sample the *current*
  //          level, i.e. the noise floor once the packet has already gone.
  //          getRSSI(false, true) reads the running RSSI register in place,
  //          without touching the mode, while the burst has only just ended.
  //          It is an approximation, and the FSK rows should be read as such.
  float rssi = isLoRa ? radio.getRSSI() : radio.getRSSI(false, true);

  uint8_t buf[PACKET_LEN];
  int16_t state = radio.readData(buf, PACKET_LEN);

  // Both of these read registers latched at RxDone, and both are LoRa-only.
  float snr     = isLoRa ? radio.getSNR() : 0.0f;
  float freqErr = isLoRa ? radio.getFrequencyError() : 0.0f;

  // Reconstruct when the packet actually landed, from the ISR timestamp rather
  // than from whenever this function happens to run.
  uint64_t nowMs  = clockNowMs();
  uint32_t lagMs  = (uint32_t)millis() - irqMillis;
  uint64_t rxMs   = (nowMs > lagMs) ? (nowMs - lagMs) : nowMs;

  RxLogRow row;
  fillCommonRow(row, slot, 0, rxMs);
  row.rssiDbm        = rssi;
  row.snrDb          = snr;
  row.snrValid       = isLoRa;      // SNR is a LoRa-only concept
  row.freqErrorHz    = freqErr;
  row.freqErrorValid = isLoRa;

  char hex[PACKET_LEN * 2 + 1] = "";
  char outcomeBuf[16];
  const char* outcome;
  TelemetryPacket pkt;
  bool parsed = false;

  if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    // Keep the corrupted bytes. RadioLib fills the buffer and only then
    // reports the CRC failure, so what we have here is the packet as it
    // actually arrived - the single most interesting artefact this rig
    // produces. The filler is a known 0x00..0x1F ramp precisely so that these
    // rows can be bit-error-counted offline.
    packetToHex(buf, PACKET_LEN, hex, sizeof(hex));
    row.payloadHex = hex;
    row.payloadLen = PACKET_LEN;

    // The frame CRC covers all 64 bytes; the payload's own CRC covers only
    // bytes 0..29. So if the magic and the payload CRC still check out, the
    // damage must lie in bytes 32..63 - the filler - and everything we
    // actually wanted from this packet arrived intact.
    //
    // Half the packet is filler, so this is where a lone bit error is most
    // likely to land, and it is a genuinely different result from a packet
    // that is simply garbage. Splitting the two costs nothing here and cannot
    // be reconstructed afterwards from the hex alone without redoing this
    // check offline.
    parsed = packetParse(buf, PACKET_LEN, &pkt);
    if (parsed && pkt.profileIndex == slot) {
      outcome = OUTCOME_CRC_FILLER;
    } else {
      // Either not ours, or the header itself is damaged. Do not trust any of
      // the parsed fields in that case.
      parsed  = false;
      outcome = OUTCOME_CRC;
    }

  } else if (state != RADIOLIB_ERR_NONE) {
    snprintf(outcomeBuf, sizeof(outcomeBuf), "rx_err%d", state);
    outcome = outcomeBuf;
  } else {
    packetToHex(buf, PACKET_LEN, hex, sizeof(hex));
    row.payloadHex = hex;
    row.payloadLen = PACKET_LEN;

    parsed = packetParse(buf, PACKET_LEN, &pkt);
    if (!parsed) {
      outcome = OUTCOME_BAD_MAGIC;
    } else if (pkt.profileIndex != slot) {
      // Decodable but from a slot we are not supposed to be hearing. Almost
      // always means the two clocks have drifted apart.
      outcome = OUTCOME_MISMATCH;
    } else {
      outcome = OUTCOME_OK;
    }
  }
  row.outcome = outcome;

  if (parsed) {
    row.repeat          = pkt.repeatIndex;
    row.txSequence      = pkt.sequence;
    row.txRoundId       = pkt.roundId;
    row.toaTxReportedMs = pkt.toaMs;

    // Offset against the instant the transmission should have *finished*, i.e.
    // scheduled start plus airtime. Zero means the two clocks agree perfectly;
    // the sign tells you which board is ahead.
    uint64_t expectedEnd = scheduleTxTimeMs(rxMs, pkt.profileIndex, pkt.repeatIndex)
                         + pkt.toaMs;
    row.arrivalOffsetMs      = (int32_t)((int64_t)rxMs - (int64_t)expectedEnd);
    row.arrivalOffsetValid   = true;

    // Bootstrap: if we have no clock at all, adopt the transmitter's. GPS will
    // take over and correct it as soon as there is a fix.
    if (!clockValid()) {
      uint64_t txEpochMs = (uint64_t)pkt.txEpochS * 1000ULL + pkt.txMillis;
      clockSet(txEpochMs + pkt.toaMs, irqMillis, CLOCK_SOURCE_PACKET);
      Serial.println("[rx] clock bootstrapped from a received packet");

      // fillCommonRow ran before we had a clock, so this row would otherwise be
      // the one and only row in the log with no timestamp on it. Backfill it.
      rxMs               = clockNowMs() - ((uint32_t)millis() - irqMillis);
      row.epochMs        = rxMs;
      row.roundIndex     = scheduleRoundIndex(rxMs);
      expectedEnd        = scheduleTxTimeMs(rxMs, pkt.profileIndex, pkt.repeatIndex)
                         + pkt.toaMs;
      row.arrivalOffsetMs = (int32_t)((int64_t)rxMs - (int64_t)expectedEnd);
    }
  }

  storageAppendRx(row);

  // Attribute this arrival to a repeat so the miss evaluator does not also
  // write a fail row for it. A parsed packet names its own repeat; anything
  // unparseable is placed by its timing.
  if (scheduleAligned) {
    uint8_t attributed = (parsed && pkt.repeatIndex < REPEATS_PER_PROFILE)
                       ? pkt.repeatIndex
                       : inferRepeat(rxMs, slot);
    g_repeats[attributed].accounted = true;
    if (!parsed) {
      row.repeat = attributed;
    }
  }

  if (strcmp(outcome, OUTCOME_OK) == 0) {
    if (scheduleAligned && pkt.repeatIndex < REPEATS_PER_PROFILE) {
      g_repeats[pkt.repeatIndex].received = true;
    }
    UiProfileResult& tally = g_thisRound[slot];
    if (tally.received < 255) {
      ++tally.received;
    }
    if (!tally.hasRssi || rssi > tally.bestRssi) {
      tally.bestRssi = rssi;
      tally.hasRssi  = true;
    }

    g_lastPacketValid = true;
    snprintf(g_lastPacketLabel, sizeof(g_lastPacketLabel), "%s",
             RADIO_PROFILES[slot].shortLabel);
    g_lastRssi      = rssi;
    g_lastSnr       = snr;
    g_lastSnrValid  = isLoRa;
    g_lastToaMs     = g_slotToaMs;
    g_lastOffsetMs  = row.arrivalOffsetMs;
    g_lastSequence  = pkt.sequence;
  }

  Serial.printf("[rx] %-11s r%lu s%u/%u rssi=%.1f snr=%s off=%+ld seq=%lu\n",
                outcome,
                (unsigned long)row.roundIndex,
                (unsigned)slot, (unsigned)row.repeat,
                rssi,
                isLoRa ? String(snr, 1).c_str() : "n/a",
                (long)row.arrivalOffsetMs,
                (unsigned long)row.txSequence);

  uiInvalidate();

  // Back into receive for the rest of the slot.
  radio.startReceive();
}

void logGpsIfDue(uint64_t now) {
  uint32_t nowS = (uint32_t)(now / 1000ULL);
  if (g_lastGpsLogS != 0 && (nowS - g_lastGpsLogS) < GPS_LOG_INTERVAL_S) {
    return;
  }
  g_lastGpsLogS = nowS;

  GpsSnapshot gps = gpsSnapshot();
  GpsLogRow row;
  memset(&row, 0, sizeof(row));
  row.epochMs    = now;
  row.fixValid   = gps.locationValid;
  row.satellites = gps.satellites;
  row.hdop       = gps.hdop;
  if (gps.locationValid) {
    row.lat           = gps.lat;
    row.lon           = gps.lon;
    row.altitudeM     = gps.altitudeM;
    row.speedKmh      = gps.speedKmh;
    row.courseDeg     = gps.courseDeg;
    row.distanceToTxM  = gpsDistanceMeters(gps.lat, gps.lon, TX_SITE_LAT, TX_SITE_LON);
    row.bearingToTxDeg = gpsBearingDeg(gps.lat, gps.lon, TX_SITE_LAT, TX_SITE_LON);
  }
  storageAppendGps(row);
}

void fillUiState(UiState& s, uint8_t activeSlot) {
  GpsSnapshot gps = gpsSnapshot();

  memset(&s, 0, sizeof(s));
  s.txMode            = false;
  s.clockValid        = clockValid();
  s.clockSourceName   = clockSourceName();
  s.epochMs           = clockNowMs();
  s.roundIndex        = scheduleRoundIndex(s.epochMs);
  s.activeSlot        = activeSlot;
  s.activeProfileShort = RADIO_PROFILES[activeSlot].shortLabel;
  s.storageBackend    = storageBackendName();
  s.storageFreeBytes  = storageFreeBytes();
  s.rowsWritten       = storageRowsWritten();

  s.roundValid     = g_lastRoundValid;
  s.displayedRound = g_lastRoundIndex;
  memcpy(s.results, g_lastRound, sizeof(s.results));

  s.lastPacketValid   = g_lastPacketValid;
  s.lastPacketProfile = g_lastPacketLabel;
  s.lastRssi          = g_lastRssi;
  s.lastSnr           = g_lastSnr;
  s.lastSnrValid      = g_lastSnrValid;
  s.lastToaMs         = g_lastToaMs;
  s.lastOffsetMs      = g_lastOffsetMs;
  s.lastSequence      = g_lastSequence;

  s.gpsValid      = gps.locationValid;
  s.gpsSatellites = gps.satellites;
  s.gpsLat        = gps.lat;
  s.gpsLon        = gps.lon;
  s.gpsHdop       = gps.hdop;
  s.txLat         = TX_SITE_LAT;
  s.txLon         = TX_SITE_LON;
  s.distanceToTxM = gps.locationValid
                  ? gpsDistanceMeters(gps.lat, gps.lon, TX_SITE_LAT, TX_SITE_LON)
                  : 0.0;

  s.profileLocked = g_profileLocked;
}

// Extra serial commands, registered with the shell in storage.cpp.
const char* kExtraHelp =
    "  lock <n>      pin the receiver to profile n, ignoring the schedule\r\n"
    "  unlock        follow the schedule again\r\n"
    "  screen        cycle the display to the next screen\r\n"
    "  profiles      list the profile table\r\n";

bool shellHook(const char* line) {
  if (strncmp(line, "lock ", 5) == 0) {
    int n = atoi(line + 5);
    if (n < 0 || n >= RADIO_PROFILE_COUNT) {
      Serial.printf("profile index must be 0..%d\n", RADIO_PROFILE_COUNT - 1);
      return true;
    }
    g_profileLocked = true;
    g_lockedSlot    = (uint8_t)n;
    Serial.printf("locked to %s\n", RADIO_PROFILES[n].label);
    uiInvalidate();
    return true;
  }
  if (strcmp(line, "unlock") == 0) {
    g_profileLocked = false;
    Serial.println("following the schedule");
    uiInvalidate();
    return true;
  }
  if (strcmp(line, "screen") == 0) {
    uiNextScreen();
    static const char* kNames[] = {"round", "gps", "packet", "status"};
    Serial.printf("screen: %s\n", kNames[uiScreen() % UI_SCREEN_COUNT]);
    return true;
  }
  if (strcmp(line, "profiles") == 0) {
    char params[40];
    for (uint8_t i = 0; i < RADIO_PROFILE_COUNT; ++i) {
      radioProfileParams(RADIO_PROFILES[i], params, sizeof(params));
      Serial.printf("  %u  %-16s %s\n", (unsigned)i, RADIO_PROFILES[i].label, params);
    }
    return true;
  }
  return false;
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
  if (txSiteConfigured()) {
    Serial.printf("TX site: %.6f, %.6f\n", (double)TX_SITE_LAT, (double)TX_SITE_LON);
  } else {
    Serial.println("*** TX SITE NOT SET - put real values in [site] of");
    Serial.println("*** secrets.ini. distance_m and bearing_deg will be");
    Serial.println("*** meaningless until you do, and nothing else will say so.");
  }
  Serial.println("type 'help' for the log shell");

  uiSplash("RX mode", "TTGO T3 v1.6.1", "SX1276 + NEO-6M");

  gpsBegin();
  storageShellSetHook(shellHook, kExtraHelp);

  resetRoundTallies(g_thisRound);
  resetRoundTallies(g_lastRound);
  resetRepeatState();

  g_radioOk = radioHwBegin();
  if (!g_radioOk) {
    uiSplash("RX mode", "RADIO FAIL", "check SPI wiring");
    return;
  }

  // Park on the bootstrap profile until the clock arrives, so a single packet
  // from the transmitter is enough to get going even without a GPS fix.
  applySlotProfile(BOOTSTRAP_PROFILE_INDEX);
}

void appLoop() {
  gpsPoll();
  storageShellPoll();

  if (!g_radioOk) {
    UiState ui;
    fillUiState(ui, 0);
    uiRender(ui);
    delay(50);
    return;
  }

  // --- no clock yet: listen on the bootstrap profile and wait for one --------
  if (!clockValid()) {
    if (g_appliedSlot != BOOTSTRAP_PROFILE_INDEX) {
      applySlotProfile(BOOTSTRAP_PROFILE_INDEX);
    }
    if (g_irqFlag) {
      handleIrq((uint8_t)g_appliedSlot, false);
    }
    UiState ui;
    fillUiState(ui, (uint8_t)g_appliedSlot);
    uiRender(ui);
    return;
  }

  uint64_t now   = clockNowMs();
  uint32_t round = scheduleRoundIndex(now);

  // --- the moment a clock first appears -------------------------------------
  // Until now we had no idea what was due. Treat this instant as the start of
  // our listening window so the partial slot we are standing in does not
  // immediately book its already-passed transmissions as losses.
  static bool s_hadClock = false;
  if (!s_hadClock) {
    s_hadClock      = true;
    g_slotEnteredMs = now;
    resetRepeatState();
  }

  // --- round rollover: freeze the tallies for the display -------------------
  if (round != g_currentRound) {
    if (g_currentRound != UINT32_MAX) {
      memcpy(g_lastRound, g_thisRound, sizeof(g_lastRound));
      g_lastRoundIndex = g_currentRound;
      g_lastRoundValid = true;

      uint8_t got = 0, want = 0;
      for (uint8_t i = 0; i < RADIO_PROFILE_COUNT; ++i) {
        got  += g_lastRound[i].received;
        want += g_lastRound[i].expected;
      }
      Serial.printf("[rx] === round %lu complete: %u/%u packets ===\n",
                    (unsigned long)g_currentRound, (unsigned)got, (unsigned)want);
    }
    g_currentRound = round;
    resetRoundTallies(g_thisRound);
    uiInvalidate();
  }

  // --- pick the slot: the schedule, unless the operator has overridden it ----
  uint8_t scheduledSlot = scheduleSlotIndex(now);
  uint8_t slot          = g_profileLocked ? g_lockedSlot : scheduledSlot;
  bool    aligned       = (slot == scheduledSlot);

  if ((int16_t)slot != g_appliedSlot) {
    applySlotProfile(slot);
    resetRepeatState();
    if (!g_radioOk) {
      return;
    }
  }

  if (g_irqFlag) {
    handleIrq(slot, aligned);
  }

  evaluateMisses(now, slot, aligned);
  logGpsIfDue(now);

  UiState ui;
  fillUiState(ui, slot);
  uiRender(ui);
}

#endif  // APP_MODE_RX
