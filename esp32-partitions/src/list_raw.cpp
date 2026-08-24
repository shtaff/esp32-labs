// =============================================================================
// list_raw.cpp - way 2: read the flash chip and decode the table by hand.
//
// The partition table is not magic. It is a 3 KB region of SPI flash at a fixed
// offset (0x8000 on every ESP32 variant by default) holding an array of 32-byte
// records. The bootloader parses exactly these bytes; so will we.
//
// Record layout (esp_partition_info_t in the IDF sources):
//
//   offset  size  field
//   0       2     magic    0x50AA for a partition entry
//   2       1     type     0 = app, 1 = data, 0x40+ = custom
//   3       1     subtype
//   4       4     offset   byte address of the partition in flash
//   8       4     size     bytes
//   12      16    label    ASCII, NOT necessarily NUL-terminated
//   28      4     flags    bit0 = encrypted, bit4 = read-only
//
// The array ends at the first record whose magic is not 0x50AA. In practice
// that is either 0xEBEB (an MD5 checksum record the tooling appends) or 0xFFFF
// (erased flash). The API listing never shows you the MD5 record; this one does.
//
// Reading is done with esp_flash_read(), the low-level SPI flash driver. It
// takes an absolute chip address - no partition bounds, no translation, and no
// safety net - and handles disabling the instruction cache around the transfer
// for you, which is the part you really do not want to write yourself.
// =============================================================================
#include <Arduino.h>

#include "esp_flash.h"       // esp_flash_read/read_id/get_size, esp_flash_default_chip

#include "partitions.h"

// Where the table lives and how big the region is. These are build-time
// settings (menuconfig / the bootloader's own header), not values you can
// discover from the chip - which is the first hint that "low level" still means
// "knowing some conventions".
static const uint32_t kTableOffset = 0x8000;
static const size_t   kTableSize   = 0xC00;    // 3 KB = room for 95 entries + MD5
static const size_t   kEntrySize   = 32;

static const uint16_t kMagicEntry = 0x50AA;
static const uint16_t kMagicMd5   = 0xEBEB;

// The exact on-flash record. packed matters: without it the compiler is free to
// pad after the two leading bytes and every field after `type` would be read
// from the wrong place.
struct __attribute__((packed)) RawEntry {
  uint16_t magic;
  uint8_t  type;
  uint8_t  subtype;
  uint32_t offset;
  uint32_t size;
  uint8_t  label[16];
  uint32_t flags;
};
static_assert(sizeof(RawEntry) == kEntrySize, "partition record must be 32 bytes");

// esp_flash_read() needs a DRAM destination, and 3 KB is a lot to put on the
// Arduino task's stack, so the buffer is static.
static uint8_t g_table[kTableSize];

void listPartitionsFromFlash() {
  Serial.println();
  Serial.println("=== WAY 2: raw flash read at 0x8000 (the chip) ===");

  // Identify the chip itself first. The JEDEC ID is three bytes read straight
  // off the SPI device: manufacturer, then a two-byte device ID whose low byte
  // is the log2 of the capacity (0x18 -> 2^24 -> 16 MB).
  uint32_t jedec = 0, flashSize = 0;
  esp_flash_read_id(esp_flash_default_chip, &jedec);
  esp_flash_get_size(esp_flash_default_chip, &flashSize);

  char human[8];
  formatSize(flashSize, human, sizeof(human));
  Serial.printf("  flash chip   : JEDEC 0x%06X (mfr 0x%02X)  %s\n",
                (unsigned)jedec, (unsigned)((jedec >> 16) & 0xFF), human);

  // The read. Address is absolute on the chip; nothing stops us asking for any
  // other offset, which is precisely the power and the danger of this API.
  esp_err_t err = esp_flash_read(esp_flash_default_chip, g_table, kTableOffset, kTableSize);
  if (err != ESP_OK) {
    Serial.printf("  esp_flash_read failed: %d\n", (int)err);
    return;
  }
  Serial.printf("  read %u bytes from 0x%06X\n\n",
                (unsigned)kTableSize, (unsigned)kTableOffset);

  Serial.println("   #  Label            Type  SubType   Offset    Size             Flags");
  Serial.println("  --  ---------------- ----- --------- --------  --------------- ----------");

  int count = 0;
  for (size_t i = 0; i < kTableSize / kEntrySize; i++) {
    // memcpy rather than casting the buffer to RawEntry*: the entries are 32-byte
    // aligned here so a cast would work, but copying into a local is the habit
    // that keeps you out of trouble on unaligned data.
    RawEntry e;
    memcpy(&e, g_table + i * kEntrySize, sizeof(e));

    if (e.magic == kMagicMd5) {
      // Not a partition, so RawEntry does not describe it: this record is the
      // magic, 14 bytes of 0xFF padding, then an MD5 over every entry before it.
      // That puts the digest at byte 16 of the record, which is why we index the
      // buffer here instead of reusing a struct field. The bootloader verifies
      // this; the esp_partition API never shows it to you.
      const uint8_t* md5 = g_table + i * kEntrySize + 16;
      Serial.print("      (md5 checksum record: ");
      for (int b = 0; b < 16; b++) Serial.printf("%02x", md5[b]);
      Serial.println(")");
      continue;
    }
    if (e.magic != kMagicEntry) {
      break;    // 0xFFFF - erased flash, so the table is over
    }

    // The label field is a fixed 16 bytes and a 16-character label leaves no
    // room for a terminator. Copy into a 17-byte buffer and terminate it here.
    char label[17];
    memcpy(label, e.label, 16);
    label[16] = '\0';

    char size[8];
    formatSize(e.size, size, sizeof(size));

    Serial.printf("  %2d  %-16s %-5s %-9s 0x%06X  0x%06X %6s  0x%08X %s\n",
                  count, label,
                  partitionTypeName(e.type), partitionSubtypeName(e.type, e.subtype),
                  (unsigned)e.offset, (unsigned)e.size, size,
                  (unsigned)e.flags,
                  (e.flags & 0x1) ? "encrypted" : "");
    count++;
  }

  Serial.printf("\n  %d entries decoded by hand\n", count);

  // Worth stating out loud: if flash encryption is enabled the partition table
  // is encrypted too, and this raw read returns ciphertext - garbage magics,
  // zero entries listed - while way 1 keeps working, because the bootloader
  // decrypted the table before caching it. That is the sharpest illustration in
  // this lab of what the library is actually doing for you.
}
