// =============================================================================
// main.cpp - common bring-up, then hand over to whichever mode was built.
//
// The mode is chosen at compile time by APP_MODE_TX / APP_MODE_RX from
// platformio.ini. build_src_filter drops the other mode's source file from the
// build entirely, so the transmitter binary contains no receiver code and vice
// versa - the transmitter does not even link TinyGPSPlus.
//
//   pio run -e tx
//   pio run -e rx
//
// See README.md for wiring, the measurement procedure and the log format.
// =============================================================================
#include <Arduino.h>

#include "app.h"
#include "config.h"
#include "storage.h"
#include "ui.h"

#if !defined(APP_MODE_TX) && !defined(APP_MODE_RX)
#error "Build with -DAPP_MODE_TX or -DAPP_MODE_RX (see platformio.ini)"
#endif
#if defined(APP_MODE_TX) && defined(APP_MODE_RX)
#error "APP_MODE_TX and APP_MODE_RX are mutually exclusive"
#endif

// Breadcrumbs. Each one is flushed before the step it announces, so if a step
// hangs or resets the board the last line printed names the culprit. The ESP32
// interrupt watchdog fires after only 300 ms, and it takes the board down
// without any panic output at all, so "what was the last thing we said" is
// often the only diagnostic available.
static void bootStep(const char* what) {
  Serial.printf("[boot] %s\n", what);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(200);   // let the USB CDC settle so the banner is not lost

  Serial.println();
#if defined(APP_MODE_TX)
  Serial.println("=== LoRa/FSK research rig - TRANSMITTER ===");
#else
  Serial.println("=== LoRa/FSK research rig - RECEIVER ===");
#endif
  Serial.flush();

  bootStep("display");
  uiBegin();

  // Only the receiver logs, so only the receiver mounts anything. Bringing a
  // filesystem up on the transmitter would be all cost and no benefit - and on
  // a board with no SD card it means formatting the 1.44 MB data partition,
  // which blocks flash long enough to trip the interrupt watchdog.
#if defined(APP_MODE_RX)
  bootStep("storage");
  storageBegin();
#else
  bootStep("storage skipped (the transmitter writes no logs)");
#endif

  bootStep("application");
  appSetup();

  bootStep("ready");
}

void loop() {
  appLoop();
}
