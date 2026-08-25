// =============================================================================
// ui.h - SSD1306 output, and the status LED.
//
// The UI holds no state of its own beyond which screen is showing. uiTask
// wakes four times a second and reads the current values straight out of the
// other modules; there is no snapshot struct to keep in step and no display
// code anywhere near the audio or radio paths.
//
// Screens advance on a long press of the mode button, or the `screen` serial
// command. There is no auto-cycle: on a handset you want the screen you chose
// to still be there when you look down at it.
//
// A board with no display, or one that will not answer on I2C, is not an
// error. uiBegin() returns false, uiTask never starts, and everything the
// screens would have shown is available over the serial port with `stat`.
// =============================================================================
#pragma once

#include <stdint.h>

enum UiScreen {
  UI_SCREEN_MAIN = 0,   // state, channel, encryption, signal, VU meter
  UI_SCREEN_LINK,       // packet counters, loss, RSSI/SNR, duty cycle
  UI_SCREEN_AUDIO,      // codec mode and cost, microphone, underruns
  UI_SCREEN_SYS,        // heap, uptime, key fingerprint, build settings
  UI_SCREEN_COUNT,
};

bool uiBegin();
bool uiPresent();

// Starts uiTask. Separate from uiBegin() so the boot banner can be drawn
// before the rest of the firmware exists.
void uiStart();

void uiNextScreen();
void uiSetScreen(UiScreen screen);
UiScreen uiScreen();

// Two lines of text, drawn immediately, from the calling context.
//
// ONLY safe before uiStart(), or from the halt path where nothing else will
// ever run again. Two tasks pushing frames at an SSD1306 over the same I2C bus
// corrupts the panel, so anything that wants to say something while the
// firmware is running uses uiFlash() instead.
void uiBanner(const char* line1, const char* line2);

// Two lines of text, shown in place of the current screen for ms milliseconds.
// The text is copied and rendered by uiTask, so this is safe to call from any
// task and returns immediately.
void uiFlash(const char* line1, const char* line2, uint32_t ms);

// -----------------------------------------------------------------------------
// Status LED. On solid while transmitting; a short flash on each received
// packet; dark otherwise. Called from whichever task made the state change,
// which is safe - it is one GPIO write.
// -----------------------------------------------------------------------------
void ledBegin();
void ledTransmitting(bool on);
void ledPacketFlash();
