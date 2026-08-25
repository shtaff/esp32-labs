// =============================================================================
// buttons.cpp - GPIO interrupts in, classified events out.
//
// The interrupt does one thing: it posts "pin N changed" to a queue. It does
// not read the pin, it does not look at the clock, and it does not decide
// anything, because all three of those are jobs for somewhere that can afford
// to be wrong for 25 milliseconds while the contacts stop arguing.
//
// buttonsTask blocks on that queue forever when both buttons are at rest, so
// an idle handset spends nothing on its user interface. Once a button is down,
// or a press is waiting to find out whether it is half of a double, the task
// switches to a 20 ms wait so it can time things out - it is still not
// polling the pins for edges, only asking whether a deadline has passed.
// =============================================================================
#include "buttons.h"

#include <Arduino.h>

#include "config.h"

// Index into the per-button state arrays. Not the GPIO number.
enum { BTN_PTT = 0, BTN_MODE = 1, BTN_COUNT };

static const uint8_t BTN_PIN[BTN_COUNT] = { PIN_BTN_PTT, PIN_BTN_MODE };

struct Msg {
  uint8_t kind;    // 0 = a pin changed, 1 = an injected event
  uint8_t value;   // button index, or ButtonEvent
};

static QueueHandle_t  queue = nullptr;
static TaskHandle_t   taskHandle = nullptr;
static ButtonHandler  handler = nullptr;

// Debounced state, owned by buttonsTask.
static bool     level[BTN_COUNT]      = { true, true };   // true = released
static uint32_t lastEdgeMs[BTN_COUNT] = { 0, 0 };

static volatile bool pttPhysical = false;
static volatile bool pttVirtual  = false;

// Mode button classification state.
static uint32_t modeDownMs = 0;
static bool     modeLongFired = false;
static bool     modeSinglePending = false;
static uint32_t modeReleaseMs = 0;

// The interrupt. attachInterruptArg() passes the button index straight through
// as `arg`, so there is no lookup and no branch on which pin fired - which
// matters because this runs from IRAM with the scheduler suspended.
//
// It deliberately does NOT read the pin. At the instant of an edge the
// contacts are still bouncing, so whatever it read would be a coin toss; the
// task reads the pin later, once it has settled.
static void ARDUINO_ISR_ATTR onEdge(void* arg) {
  Msg m = { 0, (uint8_t)(uintptr_t)arg };
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(queue, &m, &woken);
  if (woken) portYIELD_FROM_ISR();
}

static void emit(ButtonEvent e) {
  if (handler) handler(e);
}

// -----------------------------------------------------------------------------
// One debounced transition on one button.
// -----------------------------------------------------------------------------
static void onTransition(int idx, bool released, uint32_t now) {
  if (idx == BTN_PTT) {
    // No classification, no waiting. A push-to-talk button that spent 350 ms
    // deciding whether you might have meant to double-tap it would clip the
    // first word off every transmission.
    pttPhysical = !released;
    emit(released ? BTN_PTT_UP : BTN_PTT_DOWN);
    return;
  }

  if (!released) {
    modeDownMs = now;
    modeLongFired = false;
    return;
  }

  // Released.
  if (modeLongFired) {
    // The long press already fired while the button was still down. Swallow
    // the release so it cannot also count as a short press.
    modeLongFired = false;
    modeSinglePending = false;
    return;
  }

  if (modeSinglePending && (now - modeReleaseMs) <= BTN_DOUBLE_GAP_MS) {
    modeSinglePending = false;
    emit(BTN_MODE_DOUBLE);
    return;
  }

  // Might be the first half of a double. Hold it until the window closes.
  modeSinglePending = true;
  modeReleaseMs = now;
}

// Deadlines that do not depend on an edge arriving: the long press while the
// button is still down, and the double-press window expiring after a release.
static void checkTimeouts(uint32_t now) {
  if (!level[BTN_MODE] && !modeLongFired &&
      (now - modeDownMs) >= BTN_LONG_PRESS_MS) {
    modeLongFired = true;
    // Fires on the hold, not on the release, so there is tactile feedback:
    // the screen changes under your thumb and you know to let go.
    emit(BTN_MODE_LONG);
  }

  if (modeSinglePending && (now - modeReleaseMs) > BTN_DOUBLE_GAP_MS) {
    modeSinglePending = false;
    emit(BTN_MODE_SINGLE);
  }
}

// -----------------------------------------------------------------------------
// The task. One loop handling two different kinds of work:
//
//   - messages: a pin changed, or the console injected a synthetic event
//   - deadlines: a press has been held long enough to be a long press, or a
//     release has waited long enough to be a single rather than a double
//
// The wait time is what switches between them. With both buttons at rest there
// is no deadline outstanding, so it blocks forever and an idle handset spends
// nothing at all on its user interface. The moment a button goes down or a
// release starts its double-press window, it drops to a 20 ms wait so the
// deadlines can be checked - still not polling for edges, only for time.
// -----------------------------------------------------------------------------
static void buttonsTask(void* arg) {
  (void)arg;

  for (;;) {
    // Block forever while everything is at rest; drop to a 20 ms wait only
    // while there is a deadline outstanding.
    const bool busy = !level[BTN_MODE] || modeSinglePending;
    const TickType_t wait = busy ? pdMS_TO_TICKS(20) : portMAX_DELAY;

    Msg m;
    if (xQueueReceive(queue, &m, wait) == pdTRUE) {
      if (m.kind == 1) {
        const ButtonEvent e = (ButtonEvent)m.value;
        if (e == BTN_PTT_DOWN) pttVirtual = true;
        if (e == BTN_PTT_UP)   pttVirtual = false;
        emit(e);
      } else {
        const int idx = m.value;
        const uint32_t now = millis();
        if ((now - lastEdgeMs[idx]) >= BTN_DEBOUNCE_MS) {
          // Read the pin now rather than trusting whatever the interrupt saw.
          // By the time we get here the contacts have settled, so this is the
          // level that actually matters - and it means a burst of bounce
          // edges collapses to one transition instead of several.
          const bool released = digitalRead(BTN_PIN[idx]) != LOW;
          if (released != level[idx]) {
            level[idx] = released;
            lastEdgeMs[idx] = now;
            onTransition(idx, released, now);
          }
        }
      }
    }

    checkTimeouts(millis());
  }
}

bool buttonsBegin(ButtonHandler h) {
  handler = h;

  queue = xQueueCreate(16, sizeof(Msg));
  if (queue == nullptr) {
    Serial.println("[btn] out of memory creating queue");
    return false;
  }

  for (int i = 0; i < BTN_COUNT; i++) {
    pinMode(BTN_PIN[i], INPUT_PULLUP);
    level[i] = digitalRead(BTN_PIN[i]) != LOW;
  }
  pttPhysical = !level[BTN_PTT];

  // Core 0, above the display and below the radio. It has to be well clear of
  // voiceTask on core 1: a PTT release that waits behind a Codec2 frame is a
  // transmitter that stays keyed after you let go.
  if (xTaskCreatePinnedToCore(buttonsTask, "btn", 3072, nullptr, 4,
                              &taskHandle, 0) != pdPASS) {
    Serial.println("[btn] failed to start buttonsTask");
    return false;
  }

  // attachInterruptArg passes the button index straight through, so the ISR
  // needs no lookup and no branch on which pin fired.
  for (int i = 0; i < BTN_COUNT; i++) {
    attachInterruptArg(digitalPinToInterrupt(BTN_PIN[i]), onEdge,
                       (void*)(uintptr_t)i, CHANGE);
  }

  Serial.printf("[btn] PTT on GPIO%d, MODE on GPIO%d, both active low\n",
                PIN_BTN_PTT, PIN_BTN_MODE);
  if (!level[BTN_PTT] || !level[BTN_MODE]) {
    // Either somebody is holding a button through the boot, or a pin is stuck
    // low. Both are worth saying out loud, because a stuck PTT transmits.
    Serial.println("[btn] WARNING: a button reads pressed at boot");
  }
  return true;
}

bool buttonsPttHeld() { return pttPhysical || pttVirtual; }

void buttonsInject(ButtonEvent event) {
  if (queue == nullptr) return;
  Msg m = { 1, (uint8_t)event };
  xQueueSend(queue, &m, 0);
}
