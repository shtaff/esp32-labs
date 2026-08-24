// =============================================================================
// list_api.cpp - way 1: ask the library.
//
// ESP-IDF parses the partition table once, early in boot, and caches it as a
// linked list. esp_partition_find() walks that cache; nothing here touches the
// flash chip at all, which is why this listing is instant and cannot fail.
//
// The iteration idiom is a little dated but worth knowing:
//
//   it = esp_partition_find(type, subtype, label);   // returns an iterator
//   while (it) { p = esp_partition_get(it); it = esp_partition_next(it); }
//   esp_partition_iterator_release(it);              // free it when you break early
//
// ESP_PARTITION_TYPE_ANY / ESP_PARTITION_SUBTYPE_ANY plus a NULL label means
// "everything". Note that ANY is not 0xFF-as-a-wildcard by accident - passing a
// concrete type here is how you'd ask for, say, only the OTA app slots.
// =============================================================================
#include <Arduino.h>

#include "esp_partition.h"
#include "esp_ota_ops.h"     // esp_ota_get_running_partition() and friends
#include "esp_flash.h"       // esp_flash_get_size(), for the total-flash line

#include "partitions.h"

// One table row. Deliberately laid out the same way as the raw listing so that
// any difference you spot in the serial log is a real difference in the data,
// not a difference in formatting.
static void printRow(int index,
                     const char* label,
                     uint8_t type,
                     uint8_t subtype,
                     uint32_t offset,
                     uint32_t size,
                     bool encrypted,
                     const char* note) {
  char human[8];
  formatSize(size, human, sizeof(human));
  Serial.printf("  %2d  %-16s %-5s %-9s 0x%06X  0x%06X %6s  %-3s  %s\n",
                index, label,
                partitionTypeName(type), partitionSubtypeName(type, subtype),
                (unsigned)offset, (unsigned)size, human,
                encrypted ? "yes" : "no", note);
}

void listPartitionsWithApi() {
  Serial.println();
  Serial.println("=== WAY 1: esp_partition API ===");

  // Which slot are we executing from, and which one will the bootloader pick
  // next time? On a single-app build these are the same partition; after an OTA
  // update they differ until the next reboot. The raw listing below cannot tell
  // you this - it is runtime state, not table content.
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot    = esp_ota_get_boot_partition();

  Serial.printf("  running from : %s @ 0x%06X\n",
                running ? running->label : "?",
                running ? (unsigned)running->address : 0);
  Serial.printf("  boot slot    : %s\n", boot ? boot->label : "?");

  // The app image header carries what was compiled into it. Handy for proving
  // which build is actually on the board.
  const esp_app_desc_t* desc = esp_ota_get_app_description();
  if (desc) {
    Serial.printf("  app image    : \"%s\" built %s %s (IDF %s)\n",
                  desc->project_name, desc->date, desc->time, desc->idf_ver);
  }
  Serial.println();

  Serial.println("   #  Label            Type  SubType   Offset    Size             Enc");
  Serial.println("  --  ---------------- ----- --------- --------  --------------- ---  ----------");

  int count = 0;
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

  while (it != NULL) {
    const esp_partition_t* p = esp_partition_get(it);

    // The struct carries one thing the raw bytes do not: "encrypted" is not a
    // copy of the table's flag byte, it is the effective answer after the
    // bootloader has considered whether flash encryption is switched on at all.
    // Being the currently running slot is runtime state too.
    const char* note = (p == running) ? "<- running" : "";

    printRow(count, p->label, (uint8_t)p->type, (uint8_t)p->subtype,
             (uint32_t)p->address, (uint32_t)p->size, p->encrypted, note);

    count++;
    it = esp_partition_next(it);
  }
  // esp_partition_next() releases the iterator when it walks off the end, so
  // there is nothing left to free here. Release it yourself only if you break.

  uint32_t flashSize = 0;
  esp_flash_get_size(esp_flash_default_chip, &flashSize);

  char human[8];
  formatSize(flashSize, human, sizeof(human));
  Serial.printf("\n  %d partitions, %s flash total\n", count, human);

  // Small taste of the other half of the API: instead of iterating, you can ask
  // for one partition by label and get a handle you can read/write/erase
  // through, with all offsets relative to the partition start. That bounds
  // check is the main reason to prefer this API over raw flash access.
  const esp_partition_t* userdata =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                               (esp_partition_subtype_t)0x40, "userdata");
  if (userdata) {
    Serial.printf("  found \"userdata\" by label: %u bytes at 0x%06X\n",
                  (unsigned)userdata->size, (unsigned)userdata->address);
  }
}
