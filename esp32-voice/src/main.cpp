// =============================================================================
// main.cpp - bring-up, in the order the failures want to be reported in.
//
// One binary, flashed to both boards. There is no transmitter build and no
// receiver build: every unit is a half-duplex handset that listens until you
// hold PTT.
//
// The boot sequence prints a breadcrumb before each step rather than after it.
// The ESP32 interrupt watchdog fires after 300 ms and takes the board down with
// no panic output at all, so on the bad days "what was the last thing we said"
// is the only diagnostic there is.
//
// See README.md for wiring, the channel plan and the duty-cycle discussion.
// =============================================================================
#include <Arduino.h>

#include "app.h"
#include "audio.h"
#include "codec.h"
#include "config.h"
#include "console.h"
#include "crypto.h"
#include "link.h"
#include "ui.h"

static void bootStep(const char* what) {
  Serial.printf("[boot] %s\n", what);
  Serial.flush();
}

// A failure we cannot continue past. Say so on both the serial port and the
// display and then stop, rather than running on into a state where the symptom
// is something unrelated three modules away.
static void halt(const char* title, const char* detail) {
  Serial.printf("[boot] FATAL: %s - %s\n", title, detail);
  uiBanner(title, detail);
  for (;;) {
    Serial.printf("[boot] halted: %s\n", detail);
    delay(5000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);   // let the USB serial settle so the banner is not lost

  Serial.println();
  Serial.println("=== LoRa digital voice handset ===");
  Serial.flush();

  bootStep("led");
  ledBegin();

  bootStep("display");
  uiBegin();
  uiBanner("VOICE", "starting");

  // Crypto first, because it is the only step whose failure is not fatal but
  // does change what the rest of the firmware is allowed to do. Getting the
  // answer before anything can offer to arm encryption keeps that honest.
  bootStep("crypto");
  cryptoBegin();

  bootStep("codec");
  if (!codecBegin()) {
    halt("NO CODEC", "check CODEC2_MODE_*_EN in platformio.ini");
  }

  // Audio before the radio: the microphone probe clocks the I2S bus for a few
  // hundred milliseconds, and there is no reason to have the receiver open
  // through it collecting packets that nothing is ready to decode yet.
  bootStep("audio");
  if (!audioBegin()) {
    // Not fatal. A board with no working I2S is still a radio, and on this rig
    // that is a legitimate configuration - it is the far end of a link being
    // tested with one set of audio hardware.
    Serial.println("[boot] audio unavailable; continuing as a radio-only handset");
  }

  bootStep("radio");
  if (!linkBegin()) {
    halt("NO RADIO", "SX1276 did not answer on SPI");
  }

  bootStep("application");
  appBegin();

  bootStep("console");
  consoleBegin();

  uiStart();
  bootStep("ready");
}

// Everything real runs in a task. The Arduino loop task is left doing the one
// job that genuinely wants to be lowest priority and is allowed to be slow.
void loop() {
  consolePoll();
  delay(10);
}
