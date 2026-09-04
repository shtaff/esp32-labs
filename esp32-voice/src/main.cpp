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

// =============================================================================
// setup() runs on Arduino's loopTask, whose stack is 8 kB by default. That is
// not enough, and the way you find out is brutal:
//
//     [boot] codec
//     ***ERROR*** A stack overflow in task loopTask has been detected.
//
// The culprit is codec2_create(). It calls make_analysis_window(), which puts
// TWO COMP[512] arrays - the FFT working set - on the stack as locals. COMP is
// two floats, so that is 4 kB each: 8 kB in a single frame, which is the whole
// default stack before setup() has even called anything else.
//
// Nothing about this is marginal or timing-dependent; it overflows on every
// board on every boot. It did not show up until real hardware because the
// build is perfectly happy - stack depth is a runtime property.
//
// voiceTask already gets 28 kB for exactly this reason (see appBegin), but the
// codec is CREATED during setup(), before that task exists, so it is loopTask
// that has to survive it. 16 kB leaves about 7 kB of headroom over the peak.
//
// After setup() returns, loopTask only runs the serial console, which needs
// almost none of this - the cost is 8 kB of RAM held for the life of the
// board, out of roughly 290 kB free. Worth it to keep boot in one place.
//
// SET_LOOP_TASK_STACK_SIZE overrides a weak function in the Arduino core, so
// this has to sit at file scope in the translation unit that defines setup().
// =============================================================================
SET_LOOP_TASK_STACK_SIZE(16384);

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

  // How close the boot sequence came to the edge of loopTask's stack.
  //
  // This is here because a stack overflow is not a bug you debug, it is a bug
  // you are told about after the fact by a corrupted backtrace - and the thing
  // that overflowed it (codec2_create) is a library call whose appetite is not
  // visible from any of our own code. Printing the margin turns the next
  // regression from a mystery reset into a number that was getting smaller.
  //
  // On ESP-IDF this is in BYTES, unlike vanilla FreeRTOS where it is words.
  Serial.printf("[boot] loopTask stack: %u bytes never used of %u\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL),
                (unsigned)getArduinoLoopTaskStackSize());

  bootStep("ready");
}

// Everything real runs in a task. The Arduino loop task is left doing the one
// job that genuinely wants to be lowest priority and is allowed to be slow.
void loop() {
  consolePoll();
  delay(10);
}
