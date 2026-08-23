#include "radio_profiles.h"

#include <stdio.h>

// -----------------------------------------------------------------------------
// The profile table. Slot i of every round belongs to RADIO_PROFILES[i], so the
// ORDER OF THIS TABLE IS PART OF THE OVER-THE-AIR CONTRACT. If you reorder or
// resize it, reflash both boards.
//
// FSK receiver bandwidth is not a free parameter. The signal needs roughly the
// Carson bandwidth,  2*fdev + bitrate,  and the SX1276 snaps whatever you ask
// for to its nearest supported step, capped at 250 kHz:
//
//   15.2 kbps / 15.2 kHz dev -> Carson  45.6 kHz -> 50 kHz is the next step up
//   100  kbps / 100  kHz dev -> Carson 300   kHz -> clipped to the 250 kHz max
//
// The second one is therefore deliberately under-filtered. It still decodes at
// short range but loses sensitivity, and that is a property of the chip, not a
// bug in this table. It is worth knowing when you read the RSSI column.
// -----------------------------------------------------------------------------
const RadioProfile RADIO_PROFILES[RADIO_PROFILE_COUNT] = {
  // label              short        isLoRa  SF  BW      CR   BR      FDEV    RXBW
  {"LoRa SF6/BW125",   "SF6 BW125",  true,    6, 125.0f,  5,   0.0f,   0.0f,    0.0f},
  {"LoRa SF6/BW500",   "SF6 BW500",  true,    6, 500.0f,  5,   0.0f,   0.0f,    0.0f},
  {"LoRa SF12/BW125",  "SF12 BW125", true,   12, 125.0f,  5,   0.0f,   0.0f,    0.0f},
  {"LoRa SF12/BW500",  "SF12 BW500", true,   12, 500.0f,  5,   0.0f,   0.0f,    0.0f},
  {"FSK 15.2k",        "FSK 15k2",   false,   0,   0.0f,  0,  15.2f,  15.2f,   50.0f},
  {"FSK 100k",         "FSK 100k",   false,   0,   0.0f,  0, 100.0f, 100.0f,  250.0f},
};

static_assert(sizeof(RADIO_PROFILES) / sizeof(RADIO_PROFILES[0]) == RADIO_PROFILE_COUNT,
              "RADIO_PROFILE_COUNT in config.h does not match RADIO_PROFILES[]");

int16_t radioApplyProfile(SX1276& radio, const RadioProfile& profile) {
  int16_t state;

  if (profile.isLoRa) {
    ConfigLoRa_t cfg;
    cfg.frequency       = RADIO_FREQ_MHZ;
    cfg.bandwidth       = profile.bandwidthKhz;
    cfg.spreadingFactor = profile.spreadingFactor;
    cfg.codingRate      = profile.codingRate;
    cfg.syncWord        = LORA_SYNC_WORD;
    cfg.power           = RADIO_POWER_DBM;
    cfg.preambleLength  = LORA_PREAMBLE_SYMBOLS;

    // begin() switches the chip into LoRa mode and writes the whole config.
    state = radio.begin(cfg);
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }

    // SF6 is only legal in implicit-header mode. RadioLib flips the header bit
    // for us inside setSpreadingFactor(), but the receiver still needs to be
    // told how long the packet is, because there is no header to carry it.
    if (profile.spreadingFactor == 6) {
      state = radio.implicitHeader(PACKET_LEN);
    } else {
      state = radio.explicitHeader();
    }
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }

  } else {
    ConfigFSK_t cfg;
    cfg.frequency          = RADIO_FREQ_MHZ;
    cfg.bitRate            = profile.bitRateKbps;
    cfg.frequencyDeviation = profile.freqDevKhz;
    cfg.receiverBandwidth  = profile.rxBandwidthKhz;
    cfg.power              = RADIO_POWER_DBM;
    cfg.preambleLength     = FSK_PREAMBLE_BITS;

    state = radio.beginFSK(cfg);
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }

    // Gaussian filtering, BT = 0.5. Keeps the 100 kbps profile's spectrum from
    // splattering further than it already does.
    state = radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }

    uint8_t syncWord[2] = {FSK_SYNC_BYTE_0, FSK_SYNC_BYTE_1};
    state = radio.setSyncWord(syncWord, sizeof(syncWord));
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }

    // Fixed length, matching the LoRa side: no length byte on the air, and the
    // receiver knows in advance exactly how many bytes to expect.
    state = radio.fixedPacketLengthMode(PACKET_LEN);
    if (state != RADIOLIB_ERR_NONE) {
      return state;
    }
  }

  // Common to both modems. Neither is covered by the config structs.
  state = radio.setCRC(true);
  if (state != RADIOLIB_ERR_NONE) {
    return state;
  }

  return radio.setCurrentLimit(RADIO_CURRENT_LIMIT_MA);
}

uint32_t radioTimeOnAirMs(SX1276& radio) {
  // RadioLib returns microseconds; round to the nearest millisecond.
  RadioLibTime_t us = radio.getTimeOnAir(PACKET_LEN);
  return (uint32_t)((us + 500) / 1000);
}

static uint32_t g_airtimes[RADIO_PROFILE_COUNT] = {0};

void radioCacheAirtimes(SX1276& radio) {
  for (uint8_t i = 0; i < RADIO_PROFILE_COUNT; ++i) {
    if (radioApplyProfile(radio, RADIO_PROFILES[i]) == RADIOLIB_ERR_NONE) {
      g_airtimes[i] = radioTimeOnAirMs(radio);
    } else {
      // Leave it at zero rather than guessing; the schedule falls back to the
      // naive spacing for that slot and the failure is already logged.
      g_airtimes[i] = 0;
    }
  }
  radioApplyProfile(radio, RADIO_PROFILES[0]);
}

const uint32_t* radioAirtimeTable() {
  return g_airtimes;
}

void radioProfileParams(const RadioProfile& profile, char* out, size_t outSize) {
  if (profile.isLoRa) {
    snprintf(out, outSize, "SF%u/BW%.0f/CR4-%u",
             (unsigned)profile.spreadingFactor,
             profile.bandwidthKhz,
             (unsigned)profile.codingRate);
  } else {
    snprintf(out, outSize, "BR%.1fk/FD%.1fk/RXBW%.0fk",
             profile.bitRateKbps,
             profile.freqDevKhz,
             profile.rxBandwidthKhz);
  }
}
