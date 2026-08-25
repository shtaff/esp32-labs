// =============================================================================
// buttons.h - two buttons, no polling.
//
// Both pins carry a GPIO interrupt on CHANGE. The ISR does nothing but stamp
// the edge with micros() and drop it in a queue; all the debouncing and all
// the press classification happens in buttonsTask, which is the only place
// that can afford to block.
//
// PTT is deliberately not classified. It fires on the edge, because a
// push-to-talk button that waits 350 ms to find out whether you meant to
// double-tap it is a broken push-to-talk button. The mode button pays that
// cost instead, and does not care.
//
//   PTT    press          -> BTN_PTT_DOWN, start transmitting
//          release        -> BTN_PTT_UP, stop
//   MODE   short press    -> BTN_MODE_SINGLE, next channel
//          double press   -> BTN_MODE_DOUBLE, encryption on/off
//          hold ~0.9 s    -> BTN_MODE_LONG, next display screen
// =============================================================================
#pragma once

#include <stdint.h>

enum ButtonEvent {
  BTN_PTT_DOWN = 0,
  BTN_PTT_UP,
  BTN_MODE_SINGLE,
  BTN_MODE_DOUBLE,
  BTN_MODE_LONG,
};

// Called from buttonsTask, not from the interrupt. It may block, log, and call
// into the rest of the firmware freely.
typedef void (*ButtonHandler)(ButtonEvent event);

bool buttonsBegin(ButtonHandler handler);

// Debounced level of the PTT button. voiceTask reads this every frame while
// transmitting rather than tracking the up event itself, so a missed event
// cannot leave the transmitter latched on.
bool buttonsPttHeld();

// Injects an event as though a button had produced it. This is what makes the
// serial console's `ptt` command work on a board with no buttons fitted yet,
// and it is how the whole state machine gets exercised on the bench.
void buttonsInject(ButtonEvent event);
