// =============================================================================
// ui.cpp - SSD1306 rendering and the status LED.
//
// uiTask runs at four frames a second on core 0 and pulls whatever it needs
// straight out of the other modules. Nothing pushes state at it, so there is
// no snapshot to keep in step and no display code anywhere near the audio or
// radio paths.
//
// The task starts even when there is no display, because it is also what
// turns the LED off again: ledPacketFlash() is called from the radio task,
// which is the last place that can afford to sit in a delay() waiting for a
// blink to finish.
// =============================================================================
#include "ui.h"

#include <Arduino.h>

#ifndef NO_DISPLAY
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#endif

#include "app.h"
#include "audio.h"
#include "codec.h"
#include "config.h"
#include "crypto.h"
#include "link.h"

#ifndef NO_DISPLAY
static Adafruit_SSD1306 oled(128, 64, &Wire, PIN_OLED_RST);
#endif

static bool     present = false;
static UiScreen screen = UI_SCREEN_MAIN;

// Transient message, set from any task and rendered by uiTask. Copied rather
// than referenced: the caller's string is very often a stack buffer that is
// gone by the time the next frame is drawn.
static char     flashL1[20] = "";
static char     flashL2[24] = "";
static volatile uint32_t flashUntil = 0;

// LED state, written from several tasks and read only by uiTask.
static volatile bool     ledTx = false;
static volatile uint32_t ledFlashUntil = 0;

void ledBegin() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
}

void ledTransmitting(bool on) { ledTx = on; }
void ledPacketFlash()         { ledFlashUntil = millis() + 60; }

static void ledService() {
  const bool on = ledTx || ((int32_t)(millis() - ledFlashUntil) < 0);
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

bool uiPresent()        { return present; }
UiScreen uiScreen()     { return screen; }
void uiSetScreen(UiScreen s) { screen = (UiScreen)(s % UI_SCREEN_COUNT); }
void uiNextScreen()     { screen = (UiScreen)((screen + 1) % UI_SCREEN_COUNT); }

bool uiBegin() {
#ifdef NO_DISPLAY
  Serial.println("[ui] built with -DNO_DISPLAY; use the `stat` command instead");
  return false;
#else
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    // Not fatal. A handset with no display is still a handset, and everything
    // the screens show is on the serial port anyway.
    Serial.println("[ui] no SSD1306 at 0x3C; running headless");
    return false;
  }
  present = true;
  oled.setTextColor(SSD1306_WHITE);
  oled.clearDisplay();
  oled.display();
  return true;
#endif
}

void uiBanner(const char* line1, const char* line2) {
#ifndef NO_DISPLAY
  if (!present) return;
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setCursor(0, 8);
  oled.println(line1);
  oled.setTextSize(1);
  oled.setCursor(0, 34);
  oled.println(line2);
  oled.display();
#else
  (void)line1; (void)line2;
#endif
}

void uiFlash(const char* line1, const char* line2, uint32_t ms) {
  strncpy(flashL1, line1 ? line1 : "", sizeof(flashL1) - 1);
  strncpy(flashL2, line2 ? line2 : "", sizeof(flashL2) - 1);
  flashL1[sizeof(flashL1) - 1] = '\0';
  flashL2[sizeof(flashL2) - 1] = '\0';
  flashUntil = millis() + ms;
  // Say it on the serial port too. On a headless board this is the only place
  // the message can go, and it is the board most likely to need it.
  Serial.printf("[ui] %s: %s\n", flashL1, flashL2);
}

#ifndef NO_DISPLAY

// =============================================================================
// THE LAYOUT GRID - what all the magic numbers below mean.
//
// The panel is 128 x 64 pixels. Adafruit_GFX's built-in font is 6 x 8 pixels at
// setTextSize(1) and 12 x 16 at setTextSize(2), including the one-pixel gap
// between characters. So:
//
//     size 1  ->  21 characters across, 8 rows down
//     size 2  ->  10 characters across, 4 rows down
//
// Every setCursor(x, y) below is the TOP-LEFT of the text, not the baseline.
// The vertical positions are on a 10-pixel pitch - 13, 23, 33, 43, 53 - which
// gives 8-pixel text with 2 pixels of air between rows and leaves y=0..9 for a
// header. That is why the numbers look arbitrary and are not.
//
// Anything wider than 21 characters at size 1 silently wraps and corrupts the
// row below it, so the printf formats here are all counted, not estimated.
// =============================================================================

// A horizontal bar, used for the VU meter. Outline plus fill, so an empty bar
// is still visibly a bar rather than a blank patch of screen.
static void drawBar(int x, int y, int w, int h, uint8_t pct) {
  oled.drawRect(x, y, w, h, SSD1306_WHITE);
  const int fill = (w - 2) * (pct > 100 ? 100 : pct) / 100;
  if (fill > 0) oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

// Shared top line for the three detail screens: which screen you are on, and
// which preset the radio is on. The preset is repeated on every screen on
// purpose - it is the one setting that silently stops the handset working if
// the two boards disagree, so it should never be more than a glance away.
static void drawHeader(const char* title) {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("%-7s %u %s", title, (unsigned)linkPresetIndex(),
              linkCurrentPreset().label);
  oled.drawFastHLine(0, 9, 128, SSD1306_WHITE);   // rule under the header
}

// -----------------------------------------------------------------------------
// MAIN - the screen you operate from. Everything here answers a question you
// have while holding the radio, not a question you have while debugging it.
// -----------------------------------------------------------------------------
static void drawMain() {
  const VoiceState st = appState();
  const VoicePreset& p = linkCurrentPreset();

  // Top line: which preset we are on. The label carries the frequency and the
  // modem, because on this rig those two travel together - see link.cpp.
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.printf("%u %s", (unsigned)linkPresetIndex(), p.label);

  // The padlock is the single most important thing on this screen, so it gets
  // the corner and an inverted block rather than three more small letters.
  if (linkEncryption()) {
    oled.fillRect(104, 0, 24, 9, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    oled.setCursor(107, 1);
    oled.print("ENC");
    oled.setTextColor(SSD1306_WHITE);
  } else {
    oled.setCursor(101, 1);
    oled.print("clear");
  }

  // Middle: the state, big enough to read at arm's length.
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  switch (st) {
    case VOICE_TX:        oled.print(audioTestSignalActive() ? "TX tone" : "TX"); break;
    case VOICE_RX:        oled.print("RX");    break;
    case VOICE_TEST_REC:  oled.print("REC");   break;
    case VOICE_TEST_PLAY: oled.print("PLAY");  break;
    default:              oled.print(p.kind == PRESET_SELFTEST ? "test" : "idle"); break;
  }

  // Seconds in state. On transmit this is the only thing telling you how long
  // you have been holding the button down, which on a link that is over its
  // duty budget is worth knowing.
  oled.setTextSize(1);
  if (st != VOICE_IDLE) {
    const uint32_t secs = (millis() - appStateSinceMs()) / 1000UL;
    oled.setCursor(96, 20);
    oled.printf("%2lus", (unsigned long)secs);
  }

  // Third line: signal, or the self-test progress counters.
  oled.setCursor(0, 34);
  const LinkStats& s = linkStats();
  if (st == VOICE_TEST_REC || st == VOICE_TEST_PLAY) {
    oled.printf("%lu.%lus of %lus",
                (unsigned long)(appSelfTestRecordedMs() / 1000UL),
                (unsigned long)((appSelfTestRecordedMs() % 1000UL) / 100UL),
                (unsigned long)(VOICE_SELFTEST_MS / 1000UL));
  } else if (p.kind == PRESET_SELFTEST) {
    oled.print("hold PTT to record");
  } else if (s.packetsRx > 0) {
    // SNR is LoRa-only - the SX1276 has no estimator for it in FSK mode - so
    // on an FSK preset show who was talking instead of a number that would be
    // meaningless.
    if (p.modem == MODEM_LORA) {
      oled.printf("%4d dBm  SNR %+.0f", (int)s.lastRssi, (double)s.lastSnr);
    } else {
      oled.printf("%4d dBm  from %u", (int)s.lastRssi, (unsigned)s.lastStation);
    }
  } else {
    oled.print("no traffic yet");
  }

  oled.setCursor(0, 46);
  oled.print("VU");
  drawBar(18, 45, 110, 9, audioLevel());

  // Bottom line: whichever of these matters most right now.
  oled.setCursor(0, 57);
  if (st == VOICE_RX) {
    oled.printf("from %u %s buf %2u", (unsigned)s.lastStation,
                linkRxWasEncrypted() ? "enc" : "   ",
                (unsigned)linkRxFrameCount());
  } else if (p.kind == PRESET_SELFTEST) {
    oled.printf("%s  no radio", codecModeName());
  } else {
    // Talk-time remaining is the duty cycle in the form you can act on. "39s
    // left" needs no arithmetic; "0.42% of 1%" does.
    oled.printf("%s  %lus left/h", codecModeName(),
                (unsigned long)linkTalkSecondsLeft());
  }
}

// -----------------------------------------------------------------------------
// LINK - is the radio working, and how hard is it having to try?
//
// The counters are cumulative since boot and are never reset, so what you read
// is a ratio rather than a rate: rx climbing while lost stays flat is a healthy
// link, and crc climbing faster than rx means you are at the edge of range.
// -----------------------------------------------------------------------------
static void drawLink() {
  const LinkStats& s = linkStats();
  drawHeader("LINK");
  oled.setTextSize(1);

  oled.setCursor(0, 13);
  oled.printf("tx %-6lu rx %lu", (unsigned long)s.packetsTx,
              (unsigned long)s.packetsRx);
  oled.setCursor(0, 23);
  oled.printf("lost %-5lu drop %lu", (unsigned long)s.rxLost,
              (unsigned long)s.txDropped);
  oled.setCursor(0, 33);
  oled.printf("crc %-6lu alien %lu", (unsigned long)s.rxErrors,
              (unsigned long)s.rxForeign);
  oled.setCursor(0, 43);
  if (s.packetsRx == 0) {
    oled.print("rssi --   snr --");
  } else if (linkCurrentPreset().modem == MODEM_LORA) {
    oled.printf("rssi %d  snr %+.1f", (int)s.lastRssi, (double)s.lastSnr);
  } else {
    // No SNR estimator in FSK mode. Say so rather than print a stale figure
    // left over from the last LoRa preset.
    oled.printf("rssi %d  snr n/a", (int)s.lastRssi);
  }

  oled.setCursor(0, 53);
  // The measured rolling hour against the limit that actually applies to the
  // sub-band this preset sits in. Showing them together is the whole point of
  // measuring it - and the limit is 1 % on some presets and 100 % on others,
  // so a hardcoded number here would be wrong most of the time.
  oled.printf("duty %.2f%% of %.0f%%", (double)linkDutyPercent(),
              (double)linkDutyLimit());
}

// -----------------------------------------------------------------------------
// AUDIO - is the codec keeping up, and is there a microphone?
//
// The encode percentage is the number that decides whether a Codec2 mode is
// usable on this hardware at all. It has to finish inside one frame period,
// with room left for the radio, the display and the packet handling; anything
// approaching 100 % means the microphone DMA overruns and the audio breaks up.
// -----------------------------------------------------------------------------
static void drawAudio() {
  drawHeader("AUDIO");
  oled.setTextSize(1);

  oled.setCursor(0, 13);
  oled.printf("codec2 %s  %lums", codecModeName(), (unsigned long)codecFrameMs());

  oled.setCursor(0, 23);
  // Encode time against the frame period is the number that decides whether a
  // codec mode is usable at all. Over 100 % and the microphone overruns.
  const uint32_t budget = codecFrameMs() * 1000UL;
  oled.printf("enc %lu/%lu us %lu%%",
              (unsigned long)codecEncodeUs(), (unsigned long)budget,
              (unsigned long)(budget ? codecEncodeUs() * 100UL / budget : 0));

  oled.setCursor(0, 33);
  oled.printf("dec %lu us pk %lu", (unsigned long)codecDecodeUs(),
              (unsigned long)codecDecodePeakUs());

  oled.setCursor(0, 43);
  if (audioMicPresent()) {
    oled.printf("mic %luk slot%u %u%%",
                (unsigned long)(audioCaptureRate() / 1000UL),
                (unsigned)audioMicSlot(), (unsigned)audioMicConfidence());
  } else {
    oled.print("mic none - test tone");
  }

  // "clip" is the one to watch while talking. A count that climbs means the
  // microphone is saturating, which sounds like a fault and is not one - see
  // VOICE_MIC_GAIN_SHIFT in config.h.
  oled.setCursor(0, 53);
  oled.printf("clip %lu und %lu ovr %lu", (unsigned long)audioClipCount(),
              (unsigned long)appUnderruns(),
              (unsigned long)audioCaptureTimeouts());
}

// -----------------------------------------------------------------------------
// SYS - identity and configuration. The screen you read out over the phone to
// the person holding the other handset when nothing is working.
// -----------------------------------------------------------------------------
static void drawSys() {
  drawHeader("SYS");
  oled.setTextSize(1);

  const uint32_t up = millis() / 1000UL;
  oled.setCursor(0, 13);
  oled.printf("up %luh%02lum%02lus", (unsigned long)(up / 3600),
              (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));

  oled.setCursor(0, 23);
  // Station id first: with more than one handset in the air it is the thing
  // you need when somebody asks "was that you?".
  oled.printf("station %-4u heap %luk", (unsigned)linkStationId(),
              (unsigned long)(ESP.getFreeHeap() / 1024));

  oled.setCursor(0, 33);
  // Two handsets showing the same fingerprint hold the same key. It is the
  // only way to tell without trying to talk to each other.
  oled.printf("key %s %s", cryptoFingerprint(),
              cryptoKeyAvailable() ? "" : "(none)");

  // The modem line depends on which modem the preset selected - the two have
  // nothing in common to print.
  oled.setCursor(0, 43);
  const VoicePreset& p = linkCurrentPreset();
  if (p.kind != PRESET_RADIO) {
    oled.print("radio parked");
  } else if (p.modem == MODEM_LORA) {
    oled.printf("SF%d BW%.0f CR4/%d", (int)VOICE_SF, (double)VOICE_BW_KHZ,
                (int)VOICE_CR);
  } else {
    oled.printf("FSK %.0fk dev%.0f", (double)VOICE_FSK_BR_KBPS,
                (double)VOICE_FSK_FDEV_KHZ);
  }

  oled.setCursor(0, 53);
  oled.printf("%d dBm  %lums air", (int)p.powerDbm,
              (unsigned long)linkAirtimeMs());
}

#endif  // NO_DISPLAY

// -----------------------------------------------------------------------------
// The task. Two jobs at two different rates, which is why the loop is shaped
// the way it is - see the comment at the bottom of it.
// -----------------------------------------------------------------------------
static void uiTask(void* arg) {
  (void)arg;
  for (;;) {
    ledService();

#ifndef NO_DISPLAY
    if (present) {
      oled.clearDisplay();
      if ((int32_t)(millis() - flashUntil) < 0) {
        oled.setTextSize(2);
        oled.setCursor(0, 12);
        oled.print(flashL1);
        oled.setTextSize(1);
        oled.setCursor(0, 40);
        oled.print(flashL2);
      } else {
        switch (screen) {
          case UI_SCREEN_LINK:  drawLink();  break;
          case UI_SCREEN_AUDIO: drawAudio(); break;
          case UI_SCREEN_SYS:   drawSys();   break;
          default:              drawMain();  break;
        }
      }
      oled.display();
    }
#endif

    // The LED wants servicing faster than the screen wants redrawing, and a
    // 60 ms packet flash would be invisible at 4 Hz. Run the loop at 20 Hz and
    // only push pixels every fifth pass.
    for (int i = 0; i < 4; i++) {
      vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS / 5));
      ledService();
    }
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS / 5));
  }
}

void uiStart() {
  // Lowest priority of the four tasks, and on core 0 with the radio: redrawing
  // the screen must never delay a TxDone, and an I2C transaction to the panel
  // takes long enough to matter if it did.
  if (xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 2, nullptr, 0)
      != pdPASS) {
    Serial.println("[ui] failed to start uiTask");
  }
}
