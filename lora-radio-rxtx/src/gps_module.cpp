#include "gps_module.h"

#include <Arduino.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>

#include "config.h"
#include "schedule.h"

namespace {

HardwareSerial g_gpsSerial(2);
TinyGPSPlus    g_gps;

uint32_t g_lastClockSyncMs = 0;
bool     g_everSynced      = false;

// Re-anchor the clock from GPS this often. Drift over the interval is far
// below the slot guard time, so this is about keeping the anchor fresh rather
// than about correcting real error.
const uint32_t kClockResyncMs = 10000UL;

// Reject obviously wrong dates from a module that has not really locked yet.
const int kMinPlausibleYear = 2024;

#ifdef GPS_PPS_ENABLED
volatile uint32_t g_ppsMillis  = 0;
volatile bool     g_ppsPending = false;

void IRAM_ATTR ppsIsr() {
  g_ppsMillis  = millis();
  g_ppsPending = true;
}
#endif

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// algorithm). Avoids depending on mktime/timegm and on the current TZ.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  const int      era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

}  // namespace

void gpsBegin() {
  // GPIO34 is input-only, which is fine for the UART RX line. GPIO4 is the TX
  // line back to the module and is only used if you send it UBX commands.
  g_gpsSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

#ifdef GPS_PPS_ENABLED
  pinMode(PIN_GPS_PPS, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_GPS_PPS), ppsIsr, RISING);
#endif

  Serial.printf("[gps] UART2 up: RX=GPIO%d TX=GPIO%d @ %d baud\n",
                PIN_GPS_RX, PIN_GPS_TX, GPS_BAUD);
}

void gpsPoll() {
  while (g_gpsSerial.available() > 0) {
    g_gps.encode((char)g_gpsSerial.read());
  }

  if (!g_gps.date.isValid() || !g_gps.time.isValid()) {
    return;
  }
  if (g_gps.date.year() < kMinPlausibleYear) {
    return;
  }

  // Only re-anchor from a sentence that is still fresh, and not more often than
  // kClockResyncMs once we already have a clock.
  uint32_t age = g_gps.time.age();
  if (age > 1000) {
    return;
  }
  uint32_t now = millis();
  if (g_everSynced && (now - g_lastClockSyncMs) < kClockResyncMs) {
    return;
  }

  int64_t days = daysFromCivil(g_gps.date.year(), g_gps.date.month(), g_gps.date.day());
  int64_t secs = days * 86400LL
               + (int64_t)g_gps.time.hour() * 3600LL
               + (int64_t)g_gps.time.minute() * 60LL
               + (int64_t)g_gps.time.second();

  uint64_t epochMs;
  uint32_t atMillis;

#ifdef GPS_PPS_ENABLED
  if (g_ppsPending) {
    // The 1PPS rising edge marks the exact start of the UTC second that the
    // sentence we just parsed describes. Anchoring to the edge removes the
    // serial transport delay entirely.
    noInterrupts();
    atMillis     = g_ppsMillis;
    g_ppsPending = false;
    interrupts();
    epochMs = (uint64_t)secs * 1000ULL;
  } else {
    epochMs  = (uint64_t)secs * 1000ULL + (uint64_t)g_gps.time.centisecond() * 10ULL;
    atMillis = now - age;
  }
#else
  // Without PPS the anchor carries the module's NMEA output latency, typically
  // well under 100 ms. That is an order of magnitude inside SLOT_GUARD_MS, so
  // it does not affect slot alignment. Wire PPS and define GPS_PPS_ENABLED if
  // you want the arrival-offset column to be genuinely precise.
  epochMs  = (uint64_t)secs * 1000ULL + (uint64_t)g_gps.time.centisecond() * 10ULL;
  atMillis = now - age;
#endif

  clockSet(epochMs, atMillis, CLOCK_SOURCE_GPS);

  if (!g_everSynced) {
    char iso[32];
    clockFormatIso(epochMs, iso, sizeof(iso));
    Serial.printf("[gps] clock acquired from GPS: %s\n", iso);
  }
  g_everSynced      = true;
  g_lastClockSyncMs = now;
}

GpsSnapshot gpsSnapshot() {
  GpsSnapshot s;
  s.locationValid = g_gps.location.isValid() && g_gps.location.age() < 5000;
  s.lat           = s.locationValid ? g_gps.location.lat() : 0.0;
  s.lon           = s.locationValid ? g_gps.location.lng() : 0.0;
  s.altitudeM     = g_gps.altitude.isValid() ? g_gps.altitude.meters() : 0.0;
  s.speedKmh      = g_gps.speed.isValid() ? g_gps.speed.kmph() : 0.0;
  s.courseDeg     = g_gps.course.isValid() ? g_gps.course.deg() : 0.0;
  s.hdop          = g_gps.hdop.isValid() ? g_gps.hdop.hdop() : 0.0;
  s.satellites    = g_gps.satellites.isValid() ? g_gps.satellites.value() : 0;
  s.locationAgeMs = g_gps.location.age();
  return s;
}

double gpsDistanceMeters(double lat1, double lon1, double lat2, double lon2) {
  return TinyGPSPlus::distanceBetween(lat1, lon1, lat2, lon2);
}

double gpsBearingDeg(double lat1, double lon1, double lat2, double lon2) {
  return TinyGPSPlus::courseTo(lat1, lon1, lat2, lon2);
}
