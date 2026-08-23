#include "ui.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "radio_profiles.h"
#include "schedule.h"

namespace {

// -1: we drive the OLED reset line ourselves in uiBegin(), before the I2C probe,
// so that a display held in reset cannot be mistaken for a missing one.
Adafruit_SSD1306 g_display(128, 64, &Wire, -1);

bool     g_present   = false;
UiScreen g_screen    = UI_SCREEN_ROUND;
uint32_t g_lastDraw  = 0;
uint32_t g_lastCycle = 0;
bool     g_dirty     = true;

void header(const UiState& s, const char* title) {
  g_display.setCursor(0, 0);
  g_display.print(title);

  // The round number rides in the header so the body gets all six rows.
  if (!s.txMode && s.roundValid) {
    g_display.printf(" r%lu", (unsigned long)s.displayedRound);
  }

  // Right-aligned clock source, or a lock indicator when overriding.
  const char* right = s.profileLocked ? "LOCK" : s.clockSourceName;
  int16_t x = 128 - (int16_t)(strlen(right) * 6);
  g_display.setCursor(x, 0);
  g_display.print(right);

  g_display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
}

void drawRound(const UiState& s) {
  header(s, s.txMode ? "TX" : "ROUND");

  if (s.txMode) {
    g_display.setCursor(0, 13);
    g_display.printf("round %lu slot %u", (unsigned long)s.roundIndex, (unsigned)s.activeSlot);
    g_display.setCursor(0, 23);
    g_display.print(s.activeProfileShort ? s.activeProfileShort : "-");
    g_display.setCursor(0, 36);
    g_display.printf("sent %lu", (unsigned long)s.txSent);
    g_display.setCursor(0, 46);
    g_display.printf("fail %lu", (unsigned long)s.txFailed);
    g_display.setCursor(0, 56);
    g_display.print(s.txStatusLine ? s.txStatusLine : "");
    return;
  }

  if (!s.roundValid) {
    g_display.setCursor(0, 20);
    g_display.println("waiting for the");
    g_display.setCursor(0, 30);
    g_display.println("first full round");
    g_display.setCursor(0, 46);
    g_display.printf("now: slot %u", (unsigned)s.activeSlot);
    g_display.setCursor(0, 56);
    g_display.print(s.activeProfileShort ? s.activeProfileShort : "");
    return;
  }

  // One profile per row, full width.
  //
  // This used to be two columns of three, which did not fit: at 6 px per
  // character a row like "SF12  2/2 -102" is ~84 px, but each column was only
  // 64 px wide. Adafruit_GFX wraps text by default, so the overflow reappeared
  // on the following line and collided with it. Six full-width rows at 9 px
  // pitch fit the panel exactly, and setTextWrap(false) in uiBegin() means any
  // future overrun is clipped rather than smeared across the screen.
  //
  // Widest row: "SF12 BW125 2/2 -102" = 19 chars = 114 px of the 128 available.
  for (uint8_t i = 0; i < RADIO_PROFILE_COUNT; ++i) {
    const UiProfileResult& r = s.results[i];
    g_display.setCursor(0, 12 + i * 9);

    if (r.received == 0) {
      g_display.printf("%-10s -/%u", RADIO_PROFILES[i].shortLabel,
                       (unsigned)r.expected);
    } else {
      g_display.printf("%-10s %u/%u %d", RADIO_PROFILES[i].shortLabel,
                       (unsigned)r.received, (unsigned)r.expected,
                       (int)r.bestRssi);
    }
  }
}

void drawGps(const UiState& s) {
  header(s, "GPS");

  g_display.setCursor(0, 13);
  if (s.gpsValid) {
    g_display.printf("fix OK  sats %lu", (unsigned long)s.gpsSatellites);
    g_display.setCursor(0, 24);
    g_display.printf("lat %.5f", s.gpsLat);
    g_display.setCursor(0, 34);
    g_display.printf("lon %.5f", s.gpsLon);
    g_display.setCursor(0, 44);
    g_display.printf("hdop %.1f", s.gpsHdop);
    g_display.setCursor(0, 55);
    if (s.distanceToTxM >= 1000.0) {
      g_display.printf("dist %.2f km", s.distanceToTxM / 1000.0);
    } else {
      g_display.printf("dist %.0f m", s.distanceToTxM);
    }
  } else {
    g_display.printf("no fix, sats %lu", (unsigned long)s.gpsSatellites);
    g_display.setCursor(0, 30);
    g_display.println("TX site:");
    g_display.setCursor(0, 41);
    g_display.printf("%.4f", s.txLat);
    g_display.setCursor(0, 51);
    g_display.printf("%.4f", s.txLon);
  }
}

void drawPacket(const UiState& s) {
  header(s, "PACKET");

  if (!s.lastPacketValid) {
    g_display.setCursor(0, 28);
    g_display.println("nothing received");
    g_display.setCursor(0, 38);
    g_display.println("yet");
    return;
  }

  g_display.setCursor(0, 13);
  g_display.print(s.lastPacketProfile ? s.lastPacketProfile : "");
  g_display.setCursor(0, 24);
  g_display.printf("rssi %.1f dBm", s.lastRssi);
  g_display.setCursor(0, 34);
  if (s.lastSnrValid) {
    g_display.printf("snr  %.1f dB", s.lastSnr);
  } else {
    g_display.print("snr  n/a (fsk)");
  }
  g_display.setCursor(0, 44);
  g_display.printf("toa  %lu ms", (unsigned long)s.lastToaMs);
  g_display.setCursor(0, 54);
  g_display.printf("off %+ld ms seq%lu",
                   (long)s.lastOffsetMs, (unsigned long)s.lastSequence);
}

void drawStatus(const UiState& s) {
  header(s, "STATUS");

  char iso[32];
  clockFormatIso(s.epochMs, iso, sizeof(iso));

  g_display.setCursor(0, 13);
  if (s.clockValid) {
    // Only the time part fits; the date is in the log.
    g_display.printf("utc %.8s", iso + 11);
  } else {
    g_display.print("utc unset");
  }
  g_display.setCursor(0, 24);
  g_display.printf("clk %s", s.clockSourceName);
  g_display.setCursor(0, 34);
  g_display.printf("log %s", s.storageBackend);
  g_display.setCursor(0, 44);
  g_display.printf("free %lu kB",
                   (unsigned long)(s.storageFreeBytes / 1024ULL));
  g_display.setCursor(0, 54);
  g_display.printf("rows %lu", (unsigned long)s.rowsWritten);
}

}  // namespace

namespace {

// Unstick an I2C bus where a device is mid-transfer and holding SDA low - the
// state a reset leaves behind if it interrupts a transaction. The remedy is the
// one from the I2C spec: pulse SCL until the device releases SDA, then issue a
// manual STOP. Done with plain GPIO, before the I2C peripheral is initialised,
// because the peripheral itself cannot recover from this.
void i2cBusRecover() {
  pinMode(PIN_OLED_SCL, INPUT_PULLUP);
  pinMode(PIN_OLED_SDA, INPUT_PULLUP);
  delayMicroseconds(10);

  if (digitalRead(PIN_OLED_SDA) != LOW) {
    return;   // bus is idle, nothing to do
  }

  Serial.println("[ui] I2C SDA stuck low, clocking the bus free");
  pinMode(PIN_OLED_SCL, OUTPUT);
  for (int i = 0; i < 9 && digitalRead(PIN_OLED_SDA) == LOW; ++i) {
    digitalWrite(PIN_OLED_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_OLED_SCL, HIGH);
    delayMicroseconds(5);
  }

  // Manual STOP: SDA rises while SCL is high.
  pinMode(PIN_OLED_SDA, OUTPUT);
  digitalWrite(PIN_OLED_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_OLED_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_OLED_SDA, HIGH);
  delayMicroseconds(5);

  pinMode(PIN_OLED_SDA, INPUT_PULLUP);
  pinMode(PIN_OLED_SCL, INPUT_PULLUP);
}

bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

}  // namespace

void uiBegin() {

#if defined(NO_DISPLAY)
  Serial.println("[ui] display disabled at build time, running headless");
  g_present = false;
  return;
#else

  // Reset the panel ourselves, before anything talks to it. A display still
  // held in reset will not acknowledge, and would otherwise look identical to
  // one that is not fitted.
#if PIN_OLED_RST >= 0
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(1);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(10);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(10);
#endif

  i2cBusRecover();

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);

  // Bound every transaction. Without this a bus with nothing on it - no
  // display fitted, or no pull-ups - can wedge the I2C peripheral, and the
  // 300 ms ESP32 interrupt watchdog then resets the board with no output at
  // all, which is a thoroughly unpleasant thing to diagnose.
  Wire.setTimeOut(50);

  if (!i2cProbe(OLED_I2C_ADDR)) {
    Serial.printf("[ui] no SSD1306 at 0x%02X, running headless\n", OLED_I2C_ADDR);
    g_present = false;
    return;
  }

  // reset=false  - we pulsed the reset line ourselves above.
  // periphBegin=false - CRITICAL. The default is true, which makes the driver
  //   call the no-argument Wire.begin(), re-initialising the I2C peripheral we
  //   just set up on GPIO21/22. On ESP32 that deinit/reinit can hang outright.
  g_present = g_display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR, false, false);
  if (!g_present) {
    Serial.println("[ui] SSD1306 present but would not initialise, running headless");
    return;
  }

  g_display.clearDisplay();
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_WHITE);

  // Clip overruns instead of wrapping them. Wrapping is the default, and it is
  // what turned a too-wide row into text smeared over the row below it.
  g_display.setTextWrap(false);

  g_display.display();
  Serial.println("[ui] SSD1306 ready");
#endif  // NO_DISPLAY
}

void uiSplash(const char* line1, const char* line2, const char* line3) {
  if (!g_present) {
    Serial.printf("[ui] %s | %s | %s\n",
                  line1 ? line1 : "", line2 ? line2 : "", line3 ? line3 : "");
    return;
  }
  g_display.clearDisplay();
  g_display.setCursor(0, 8);
  if (line1) g_display.println(line1);
  g_display.setCursor(0, 24);
  if (line2) g_display.println(line2);
  g_display.setCursor(0, 40);
  if (line3) g_display.println(line3);
  g_display.display();
  g_dirty = true;
}

UiScreen uiScreen() {
  return g_screen;
}

void uiNextScreen() {
  g_screen = (UiScreen)((g_screen + 1) % UI_SCREEN_COUNT);
  g_dirty  = true;
}

void uiInvalidate() {
  g_dirty = true;
}

void uiRender(const UiState& state) {
  if (!g_present) {
    return;
  }
  uint32_t now = millis();

#if SCREEN_AUTO_CYCLE_S > 0
  // Timed rotation. This is the only way screens advance on the device itself,
  // the button having turned out to be unusable on this hardware.
  if ((now - g_lastCycle) >= (SCREEN_AUTO_CYCLE_S * 1000UL)) {
    g_lastCycle = now;
    uiNextScreen();
  }
#endif

  if (!g_dirty && (now - g_lastDraw) < DISPLAY_REFRESH_MS) {
    return;
  }
  g_lastDraw = now;
  g_dirty    = false;

  g_display.clearDisplay();
  switch (g_screen) {
    case UI_SCREEN_GPS:    drawGps(state);    break;
    case UI_SCREEN_PACKET: drawPacket(state); break;
    case UI_SCREEN_STATUS: drawStatus(state); break;
    case UI_SCREEN_ROUND:
    default:               drawRound(state);  break;
  }
  g_display.display();
}
