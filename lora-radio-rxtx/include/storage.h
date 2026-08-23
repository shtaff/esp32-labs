// =============================================================================
// storage.h - CSV logging with a microSD primary and a LittleFS fallback, plus
// the serial shell used to get the logs back off the board.
//
// The T3_V1.6.1 has a microSD slot wired to its own SPI bus, so logging never
// contends with the radio. If a card is present the logs go there and you
// retrieve them by pulling the card. If not, the same rows go to the internal
// LittleFS partition and you pull them over USB with the serial shell.
//
// The two backends are deliberately interchangeable: same paths, same rows,
// same shell commands. The only difference you will notice is capacity.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

enum StorageBackend {
  STORAGE_NONE = 0,
  STORAGE_SD,
  STORAGE_LITTLEFS,
};

// One received-or-missing packet. Every field maps to a CSV column; see
// storageRxHeader() in storage.cpp for the authoritative column order.
struct RxLogRow {
  uint64_t    epochMs;        // receiver UTC at the moment of the event
  uint32_t    roundIndex;
  uint8_t     slot;           // == profile index
  uint8_t     repeat;
  const char* modem;          // "lora" or "fsk"
  const char* profileLabel;
  const char* params;         // "SF12/BW125/CR4-5" or "BR100.0k/FD100.0k/..."
  const char* outcome;        // see the OUTCOME_* constants below

  // Full GPS state at the moment of the event, so every row stands on its own:
  // distances can be recomputed against any reference point, and fix quality
  // can be used to weight a measurement, without joining to the GPS log on
  // timestamp.
  bool     gpsValid;
  double   rxLat;
  double   rxLon;
  uint32_t gpsSatellites;
  double   gpsHdop;
  double   gpsAltitudeM;
  double   gpsSpeedKmh;       // matters: a fast-moving receiver smears a slot
  double   gpsCourseDeg;
  uint32_t gpsAgeMs;          // age of the fix; a stale one is worth spotting

  double txLat;               // from config, the configured transmitter site
  double txLon;
  double distanceM;           // receiver -> transmitter site, metres
  double bearingDeg;          // receiver -> transmitter site, deg from true N

  float  rssiDbm;
  float  snrDb;               // LoRa only; written empty for FSK
  bool   snrValid;
  float  freqErrorHz;
  bool   freqErrorValid;

  uint32_t toaCalcMs;         // RadioLib airtime for this profile
  uint32_t toaTxReportedMs;   // airtime the transmitter put in the packet
  int32_t  arrivalOffsetMs;   // actual arrival minus scheduled transmit instant
  bool     arrivalOffsetValid;

  uint32_t    txSequence;
  uint16_t    txRoundId;
  const char* payloadHex;     // may be nullptr for a miss
  size_t      payloadLen;
};

// Outcome column values.
#define OUTCOME_OK        "received"
#define OUTCOME_FAIL      "fail"        // expected, never arrived
#define OUTCOME_CRC       "crc_error"   // bad frame CRC, payload unusable too
#define OUTCOME_BAD_MAGIC "bad_payload" // decoded, but not one of our packets
#define OUTCOME_MISMATCH  "wrong_slot"  // valid packet, unexpected slot

// Frame CRC failed, but the magic and the payload's own CRC over bytes 0..29
// still check out - so every field we care about survived and the damage is
// confined to the 32-byte filler, which is deliberately outside that CRC.
//
// Half the packet is filler, so this is a likely place for a single bit error
// to land. It marks a real and distinct point on the propagation curve: the
// link was good enough to carry the payload intact, and only just not good
// enough to satisfy the radio's own check over the whole frame.
#define OUTCOME_CRC_FILLER "crc_filler"

struct GpsLogRow {
  uint64_t epochMs;
  bool     fixValid;
  uint32_t satellites;
  double   hdop;
  double   lat;
  double   lon;
  double   altitudeM;
  double   speedKmh;
  double   courseDeg;
  double   distanceToTxM;
  double   bearingToTxDeg;
};

// Mount SD, falling back to LittleFS. Writes CSV headers if the files are new.
// Safe to call once from setup(); returns the backend actually in use.
StorageBackend storageBegin();

StorageBackend storageBackend();
const char*    storageBackendName();

// Free space on the active backend, bytes. 0 if unknown.
uint64_t storageFreeBytes();

// Rows appended since boot - shown on the status screen.
uint32_t storageRowsWritten();

bool storageAppendRx(const RxLogRow& row);
bool storageAppendGps(const GpsLogRow& row);

// -----------------------------------------------------------------------------
// Serial shell. Call storageShellPoll() from loop(); it is non-blocking and
// consumes one line at a time.
//
//   help            list commands
//   ls              list files with sizes
//   cat <path>      dump a file to the serial port
//   rm <path>       delete a file
//   df              backend and free space
//   clock           current UTC and its source
//   stats           rows written this session
//
// RX mode adds `lock <n>` / `unlock`; those are registered by rx_app.cpp via
// storageShellSetHook() so this module stays mode-agnostic.
// -----------------------------------------------------------------------------
void storageShellPoll();

// Return true if the hook handled the line.
typedef bool (*ShellHook)(const char* line);
void storageShellSetHook(ShellHook hook, const char* extraHelp);
