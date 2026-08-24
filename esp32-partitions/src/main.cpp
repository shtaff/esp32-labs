// =============================================================================
// esp32-partitions - list the flash partition table two ways.
//
//   pio run -t upload -t monitor
//
// Way 1 (src/list_api.cpp) uses the esp_partition API: the table as the
// firmware's own library sees it, already parsed and cached at boot.
//
// Way 2 (src/list_raw.cpp) reads flash offset 0x8000 with esp_flash_read() and
// decodes the 32-byte records by hand: the table as it physically exists on the
// chip.
//
// Run both and compare. They should agree on every partition - and they will
// also differ in a few instructive ways, which README.md walks through.
// =============================================================================
#include <Arduino.h>

#include "partitions.h"

#define CRASH_ON_PURPOSE 1   // set to 1 to crash after listing in the loop

static int& getBadReference() {
    int local_var = 42; // Lives on the stack
    return local_var;   // Dangerous: returns reference to dying variable
}

static void checkAndCrash(uint8_t crash) {
  if (crash == 0) return;
  Serial.printf("\n\n=== CRASHING ON PURPOSE (crash=%d) ===\n\n", crash);
  delay(100);  // give the serial output a chance to finish before we crash

  int& ref = getBadReference(); 
  // The stack frame is gone, ref is dead.
  Serial.printf("  wrote 100 to a dead reference, now reading it back: %d\n", ref);
}

static void printBoth(uint8_t crash) {
  checkAndCrash(crash);
  listPartitionsWithApi();
  listPartitionsFromFlash();
  Serial.println("\n[press any key to list again (or crash if CRASH_ON_PURPOSE is set)]");
}

void setup() {
  Serial.begin(115200);
  delay(500);       // let USB CDC enumerate, or the first lines are lost

  Serial.println("\n\n=== ESP32 partition table lab ===");
  printBoth(0);
}

void loop() {
  // Nothing to do but wait. A keystroke re-runs both listings, which is handy
  // after an OTA update, when the "running" and "boot" slots stop matching.
  if (Serial.available()) {
    while (Serial.available()) Serial.read();   // drain, we do not care what it was
    printBoth(CRASH_ON_PURPOSE);
  }
  delay(50);
}
