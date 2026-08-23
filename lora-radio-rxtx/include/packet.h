// =============================================================================
// packet.h - the 64-byte over-the-air payload, shared by both build modes.
//
// The payload is a packed binary struct rather than a text string. It costs the
// same 64 bytes either way, but the binary form lets the receiver check a CRC,
// confirm which profile and repeat a packet belongs to, and recover UTC from a
// packet when it has no GPS fix yet. The raw bytes are still dumped as hex into
// the CSV, so nothing is lost for offline analysis.
//
// NOTHING LOCATION-BEARING TRAVELS OVER THE AIR. The transmitter's coordinate
// used to ride along in bytes 18..25 and was removed: this is an unencrypted
// link on a public band, and anyone with a matching radio could have read the
// site position straight off it. The receiver takes the coordinate from its own
// config instead, which it always did for the distance calculation anyway - the
// transmitted copy was never used for anything.
//
// Layout (little-endian, packed, exactly PACKET_LEN = 64 bytes):
//
//   off  len  field
//   ---  ---  ---------------------------------------------------------------
//     0    4  magic "LRX1"
//     4    1  profile index this packet was sent on
//     5    1  repeat index within the profile slot (0-based)
//     6    2  round id - low 16 bits of (epoch_s / ROUND_PERIOD_S)
//     8    4  tx epoch seconds (UTC)
//    12    2  tx milliseconds within that second
//    14    4  monotonically increasing transmit sequence number
//    18    4  airtime RadioLib computed for this packet, ms
//    22    2  CRC-16/CCITT-FALSE over bytes 0..21
//    24   40  deterministic filler 0x00,0x01,...,0x27
//   ---  ---  ---------------------------------------------------------------
//
// The filler is a known ramp rather than zeroes so that, if you ever want to,
// you can count bit errors in a corrupted packet instead of only seeing that
// the CRC failed. It sits outside the CRC deliberately - see README.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "config.h"

#define PACKET_MAGIC_0 'L'
#define PACKET_MAGIC_1 'R'
#define PACKET_MAGIC_2 'X'
#define PACKET_MAGIC_3 '1'

#define PACKET_HEADER_LEN 22   // bytes covered by the CRC
#define PACKET_FILLER_OFF 24

#pragma pack(push, 1)
struct TelemetryPacket {
  char     magic[4];
  uint8_t  profileIndex;
  uint8_t  repeatIndex;
  uint16_t roundId;
  uint32_t txEpochS;
  uint16_t txMillis;
  uint32_t sequence;
  uint32_t toaMs;
  uint16_t crc16;
  uint8_t  filler[PACKET_LEN - PACKET_FILLER_OFF];
};
#pragma pack(pop)

static_assert(sizeof(TelemetryPacket) == PACKET_LEN,
              "TelemetryPacket must be exactly PACKET_LEN bytes");

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor.
uint16_t packetCrc16(const uint8_t* data, size_t len);

// Fill `out` (PACKET_LEN bytes) with a valid packet, CRC included.
void packetBuild(uint8_t* out,
                 uint8_t profileIndex,
                 uint8_t repeatIndex,
                 uint16_t roundId,
                 uint32_t txEpochS,
                 uint16_t txMillis,
                 uint32_t sequence,
                 uint32_t toaMs);

// Validate magic and CRC. Returns true and fills `out` on success.
bool packetParse(const uint8_t* in, size_t len, TelemetryPacket* out);

// Render `len` bytes as lowercase hex into `out`, which must hold 2*len+1.
void packetToHex(const uint8_t* in, size_t len, char* out, size_t outSize);
