// =============================================================================
// radio_profiles.h - the set of modem configurations under test, plus the code
// that applies one to the SX1276.
//
// Both build modes share this table. It is the single source of truth for what
// a "slot" means: profile i owns slot i of every round.
// =============================================================================
#pragma once

#include <RadioLib.h>
#include <stdint.h>
#include "config.h"

struct RadioProfile {
  const char* label;       // human label, e.g. "LoRa SF12/BW125"
  const char* shortLabel;  // <= 12 chars, for the 128x64 OLED
  bool  isLoRa;            // false => FSK

  // LoRa-only
  uint8_t spreadingFactor;
  float   bandwidthKhz;
  uint8_t codingRate;      // denominator: 5 => 4/5

  // FSK-only
  float bitRateKbps;
  float freqDevKhz;
  float rxBandwidthKhz;
};

extern const RadioProfile RADIO_PROFILES[RADIO_PROFILE_COUNT];

// Apply a profile to the radio: full begin() plus the extras the config structs
// do not cover (CRC, current limit, sync word, fixed/implicit length).
// Returns a RadioLib status code; RADIOLIB_ERR_NONE on success.
int16_t radioApplyProfile(SX1276& radio, const RadioProfile& profile);

// RadioLib's computed airtime for a PACKET_LEN payload under the currently
// applied profile, in milliseconds.
uint32_t radioTimeOnAirMs(SX1276& radio);

// Apply each profile in turn and record its airtime, so the schedule can space
// repeats around the transmission rather than through it. Leaves the radio on
// profile 0. Call once at boot; results go to radioAirtimeTable().
void radioCacheAirtimes(SX1276& radio);

// The cached table, RADIO_PROFILE_COUNT entries. Zeroed until the above runs.
const uint32_t* radioAirtimeTable();

// "SF12/BW125" or "BR100.0k/FD100.0k" - the CSV params column.
void radioProfileParams(const RadioProfile& profile, char* out, size_t outSize);
