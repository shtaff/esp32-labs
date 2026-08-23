// =============================================================================
// ui.h - SSD1306 output.
//
// The UI layer holds no application state of its own. Each mode fills in a
// snapshot struct and hands it over; this keeps the display code out of the
// timing-critical radio paths and means the TX and RX builds can share it.
//
// Screens advance on a timer (SCREEN_AUTO_CYCLE_S) or via the `screen` serial
// command. There is no button: GPIO0 is wired into the USB-serial auto-reset
// circuit on this hardware and pressing it resets the board rather than
// reaching the firmware. See README.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "config.h"

enum UiScreen {
  UI_SCREEN_ROUND = 0,   // per-profile hit counts for the last complete round
  UI_SCREEN_GPS,         // GPS fix, position, distance to the transmitter site
  UI_SCREEN_PACKET,      // detail of the most recent packet
  UI_SCREEN_STATUS,      // clock source, storage backend, free space
  UI_SCREEN_COUNT,
};

// Per-profile tally for the round screen.
struct UiProfileResult {
  uint8_t received;      // packets that arrived
  uint8_t expected;      // packets that should have arrived
  float   bestRssi;      // best RSSI seen this round
  bool    hasRssi;
};

struct UiState {
  // Common
  bool        clockValid;
  const char* clockSourceName;
  uint64_t    epochMs;
  uint32_t    roundIndex;
  uint8_t     activeSlot;
  const char* activeProfileShort;
  const char* storageBackend;
  uint64_t    storageFreeBytes;
  uint32_t    rowsWritten;

  // RX round tallies. `roundValid` is false until one full round has elapsed.
  bool            roundValid;
  uint32_t        displayedRound;
  UiProfileResult results[RADIO_PROFILE_COUNT];

  // RX last-packet detail
  bool        lastPacketValid;
  const char* lastPacketProfile;
  float       lastRssi;
  float       lastSnr;
  bool        lastSnrValid;
  uint32_t    lastToaMs;
  int32_t     lastOffsetMs;
  uint32_t    lastSequence;

  // RX GPS
  bool     gpsValid;
  uint32_t gpsSatellites;
  double   gpsLat;
  double   gpsLon;
  double   gpsHdop;
  double   distanceToTxM;
  double   txLat;
  double   txLon;

  // RX manual override
  bool profileLocked;

  // TX
  bool        txMode;
  uint32_t    txSent;
  uint32_t    txFailed;
  const char* txStatusLine;
};

void uiBegin();

// Full-screen message, used for boot and for fatal errors.
void uiSplash(const char* line1, const char* line2, const char* line3);

UiScreen uiScreen();
void     uiNextScreen();

// Redraw, rate-limited internally to DISPLAY_REFRESH_MS. Also advances the
// screen when SCREEN_AUTO_CYCLE_S has elapsed, so call it every loop.
void uiRender(const UiState& state);

// Force the next uiRender() to redraw even if the rate limit has not expired.
void uiInvalidate();
