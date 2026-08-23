#include <string.h>
#include "packet.h"

uint16_t packetCrc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

void packetBuild(uint8_t* out,
                 uint8_t profileIndex,
                 uint8_t repeatIndex,
                 uint16_t roundId,
                 uint32_t txEpochS,
                 uint16_t txMillis,
                 uint32_t sequence,
                 uint32_t toaMs) {
  TelemetryPacket p;
  memset(&p, 0, sizeof(p));

  p.magic[0] = PACKET_MAGIC_0;
  p.magic[1] = PACKET_MAGIC_1;
  p.magic[2] = PACKET_MAGIC_2;
  p.magic[3] = PACKET_MAGIC_3;
  p.profileIndex = profileIndex;
  p.repeatIndex  = repeatIndex;
  p.roundId      = roundId;
  p.txEpochS     = txEpochS;
  p.txMillis     = txMillis;
  p.sequence     = sequence;
  p.toaMs        = toaMs;

  // Deterministic ramp, so a corrupted packet can be bit-error-counted offline.
  for (size_t i = 0; i < sizeof(p.filler); ++i) {
    p.filler[i] = (uint8_t)i;
  }

  // CRC covers everything ahead of the crc16 field itself.
  p.crc16 = packetCrc16(reinterpret_cast<const uint8_t*>(&p), PACKET_HEADER_LEN);

  memcpy(out, &p, sizeof(p));
}

bool packetParse(const uint8_t* in, size_t len, TelemetryPacket* out) {
  if (len < sizeof(TelemetryPacket)) {
    return false;
  }
  if (in[0] != PACKET_MAGIC_0 || in[1] != PACKET_MAGIC_1 ||
      in[2] != PACKET_MAGIC_2 || in[3] != PACKET_MAGIC_3) {
    return false;
  }

  TelemetryPacket p;
  memcpy(&p, in, sizeof(p));
  if (p.crc16 != packetCrc16(in, PACKET_HEADER_LEN)) {
    return false;
  }

  if (out) {
    *out = p;
  }
  return true;
}

void packetToHex(const uint8_t* in, size_t len, char* out, size_t outSize) {
  static const char kHex[] = "0123456789abcdef";
  size_t o = 0;
  for (size_t i = 0; i < len && o + 2 < outSize; ++i) {
    out[o++] = kHex[in[i] >> 4];
    out[o++] = kHex[in[i] & 0x0F];
  }
  out[o] = '\0';
}
