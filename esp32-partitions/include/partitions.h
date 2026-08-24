// =============================================================================
// partitions.h - the two listings, plus the byte-to-name decoding both share.
//
// The whole point of this lab is that the same table is printed twice from two
// very different places:
//
//   listPartitionsWithApi()   asks the ESP-IDF partition component. It hands
//                             back a linked list of structs that were parsed
//                             during boot. Easy, safe, and slightly filtered.
//
//   listPartitionsFromFlash() reads the raw bytes out of the SPI flash chip at
//                             offset 0x8000 and decodes them by hand. Nothing
//                             is hidden, including entries the API skips.
//
// See README.md for the walkthrough of why they can disagree.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

void listPartitionsWithApi();
void listPartitionsFromFlash();

// -----------------------------------------------------------------------------
// Type / subtype decoding.
//
// In the flash table these are two plain bytes. The API wraps them in enums
// (esp_partition_type_t / esp_partition_subtype_t), but the numeric values are
// identical - the enums *are* these bytes. So one decoder serves both listings,
// and the raw path stays free of any dependency on the partition component.
//
// The full list lives in esp_partition.h; this covers what you actually meet on
// an Arduino-flavoured ESP32. Anything else is printed as its hex value, which
// is exactly what you want when you invent your own subtype (we do - see
// partitions.csv).
// -----------------------------------------------------------------------------

inline const char* partitionTypeName(uint8_t type) {
  switch (type) {
    case 0x00: return "app";
    case 0x01: return "data";
    default:   return "?";     // 0x40..0xFE are yours to define
  }
}

inline const char* partitionSubtypeName(uint8_t type, uint8_t subtype) {
  if (type == 0x00) {                      // app
    switch (subtype) {
      case 0x00: return "factory";
      case 0x10: return "ota_0";           // ota_0..ota_15 are 0x10..0x1F
      case 0x11: return "ota_1";
      case 0x20: return "test";
      default:   return "?";
    }
  }
  if (type == 0x01) {                      // data
    switch (subtype) {
      case 0x00: return "ota";             // the otadata partition itself
      case 0x01: return "phy";             // RF calibration data
      case 0x02: return "nvs";             // key/value store (WiFi creds live here)
      case 0x03: return "coredump";        // panic dumps
      case 0x04: return "nvs_keys";        // NVS encryption keys
      case 0x05: return "efuse";           // efuse emulation
      case 0x06: return "undefined";
      case 0x80: return "esphttpd";
      case 0x81: return "fat";             // FFat
      case 0x82: return "spiffs";          // also what LittleFS images are tagged as
      case 0x83: return "littlefs";
      default:   return "?";
    }
  }
  return "?";
}

// 0x00005000 -> "20K". Sizes are always whole KB in practice, so no rounding
// games are needed; anything that is a whole MB gets the nicer unit.
inline void formatSize(uint32_t bytes, char* out, size_t outLen) {
  if (bytes >= 1024u * 1024u && bytes % (1024u * 1024u) == 0) {
    snprintf(out, outLen, "%luM", (unsigned long)(bytes / (1024u * 1024u)));
  } else if (bytes >= 1024u && bytes % 1024u == 0) {
    snprintf(out, outLen, "%luK", (unsigned long)(bytes / 1024u));
  } else {
    snprintf(out, outLen, "%luB", (unsigned long)bytes);
  }
}
