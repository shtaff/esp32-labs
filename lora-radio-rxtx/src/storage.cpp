#include "storage.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <LittleFS.h>

#include "config.h"
#include "schedule.h"

namespace {

StorageBackend g_backend = STORAGE_NONE;
uint32_t       g_rows    = 0;

// The SD card lives on HSPI. The radio owns VSPI, and the two never interact.
SPIClass g_sdSpi(HSPI);

ShellHook   g_hook      = nullptr;
const char* g_extraHelp = nullptr;

fs::FS* activeFs() {
  switch (g_backend) {
    case STORAGE_SD:       return &SD;
    case STORAGE_LITTLEFS: return &LittleFS;
    default:               return nullptr;
  }
}

const char* kRxHeader =
    "utc_time,round,slot,repeat,modem,profile,params,"
    "rx_lat,rx_lon,gps_valid,gps_sats,gps_hdop,gps_alt_m,"
    "gps_speed_kmh,gps_course_deg,gps_age_ms,"
    "tx_lat,tx_lon,distance_m,bearing_deg,"
    "outcome,rssi_dbm,snr_db,freq_err_hz,"
    "toa_calc_ms,toa_tx_ms,arrival_offset_ms,"
    "tx_seq,tx_round_id,payload_len,payload_hex";

const char* kGpsHeader =
    "utc_time,fix_valid,satellites,hdop,lat,lon,alt_m,speed_kmh,course_deg,"
    "distance_to_tx_m,bearing_to_tx_deg";

// Create the file with its header row if it does not exist yet.
//
// If it exists but carries a DIFFERENT header, the column layout has changed
// since it was written. Appending new-format rows underneath an old header
// would produce a file that loads without complaint and is quietly wrong, so
// the old one is moved aside instead.
void ensureHeader(const char* path, const char* header) {
  fs::FS* fs = activeFs();
  if (!fs) {
    return;
  }

  if (fs->exists(path)) {
    File existing = fs->open(path, FILE_READ);
    if (!existing) {
      return;
    }
    String firstLine = existing.readStringUntil('\n');
    existing.close();
    firstLine.trim();

    if (firstLine == header) {
      return;   // same schema, keep appending
    }

    char archived[64];
    snprintf(archived, sizeof(archived), "%s.old", path);
    fs->remove(archived);
    if (fs->rename(path, archived)) {
      Serial.printf("[storage] %s had an older column layout, moved to %s\n",
                    path, archived);
    } else {
      Serial.printf("[storage] %s has an older column layout and could not be "
                    "renamed - delete it by hand\n", path);
      return;
    }
  }

  File f = fs->open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[storage] could not create %s\n", path);
    return;
  }
  f.println(header);
  f.close();
}

bool appendLine(const char* path, const char* line) {
  fs::FS* fs = activeFs();
  if (!fs) {
    return false;
  }
  File f = fs->open(path, FILE_APPEND);
  if (!f) {
    Serial.printf("[storage] could not append to %s\n", path);
    return false;
  }
  f.println(line);
  f.close();
  ++g_rows;
  return true;
}

void shellLs() {
  fs::FS* fs = activeFs();
  if (!fs) {
    Serial.println("no storage");
    return;
  }
  File root = fs->open("/");
  if (!root) {
    Serial.println("cannot open /");
    return;
  }
  File entry = root.openNextFile();
  while (entry) {
    Serial.printf("%-24s %10u %s\n",
                  entry.name(),
                  (unsigned)entry.size(),
                  entry.isDirectory() ? "<dir>" : "");
    entry = root.openNextFile();
  }
  root.close();
}

void shellCat(const char* path) {
  fs::FS* fs = activeFs();
  if (!fs) {
    Serial.println("no storage");
    return;
  }
  File f = fs->open(path, FILE_READ);
  if (!f) {
    Serial.printf("cannot open %s\n", path);
    return;
  }
  // Markers let a host-side script find the exact file boundaries in the
  // serial stream, so you can pipe the monitor output straight into a .csv.
  Serial.printf("---- BEGIN %s (%u bytes) ----\n", path, (unsigned)f.size());
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    Serial.write(buf, n);
  }
  f.close();
  Serial.printf("\n---- END %s ----\n", path);
}

void shellHelp() {
  Serial.println(F("commands:"));
  Serial.println(F("  help          this text"));
  Serial.println(F("  ls            list files"));
  Serial.println(F("  cat <path>    dump a file"));
  Serial.println(F("  rm <path>     delete a file"));
  Serial.println(F("  df            backend and free space"));
  Serial.println(F("  clock         current UTC and clock source"));
  Serial.println(F("  stats         rows written this session"));
  Serial.println(F("  format        erase and prepare the internal LittleFS"));
  if (g_extraHelp) {
    Serial.print(g_extraHelp);
  }
}

void shellExecute(char* line) {
  // Trim trailing whitespace.
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\t')) {
    line[--len] = '\0';
  }
  if (len == 0) {
    return;
  }

  if (g_hook && g_hook(line)) {
    return;
  }

  if (strcmp(line, "help") == 0) {
    shellHelp();
  } else if (strcmp(line, "ls") == 0) {
    shellLs();
  } else if (strncmp(line, "cat ", 4) == 0) {
    shellCat(line + 4);
  } else if (strncmp(line, "rm ", 3) == 0) {
    fs::FS* fs = activeFs();
    if (fs && fs->remove(line + 3)) {
      Serial.printf("removed %s\n", line + 3);
    } else {
      Serial.printf("could not remove %s\n", line + 3);
    }
  } else if (strcmp(line, "df") == 0) {
    Serial.printf("backend=%s free=%llu bytes\n",
                  storageBackendName(), (unsigned long long)storageFreeBytes());
  } else if (strcmp(line, "clock") == 0) {
    char iso[32];
    clockFormatIso(clockNowMs(), iso, sizeof(iso));
    Serial.printf("utc=%s source=%s age=%us round=%lu slot=%u\n",
                  clockValid() ? iso : "unset",
                  clockSourceName(),
                  (unsigned)clockAgeS(),
                  (unsigned long)scheduleRoundIndex(clockNowMs()),
                  (unsigned)scheduleSlotIndex(clockNowMs()));
  } else if (strcmp(line, "format") == 0) {
    // Deliberately manual: this erases 1.44 MB with the flash cache off, which
    // takes several seconds and may itself trip the interrupt watchdog and
    // reset the board. That is survivable when you asked for it and are
    // watching the serial port; it is not survivable as a silent boot step.
    Serial.println("formatting LittleFS - this takes several seconds.");
    Serial.println("if the board resets during it, just power-cycle and retry;");
    Serial.println("the format resumes from where the erase got to.");
    Serial.flush();
    if (LittleFS.format() && LittleFS.begin(false)) {
      g_backend = STORAGE_LITTLEFS;
      ensureHeader(LOG_PATH_RX, kRxHeader);
      ensureHeader(LOG_PATH_GPS, kGpsHeader);
      Serial.println("format complete, LittleFS mounted");
    } else {
      Serial.println("format failed");
    }
  } else if (strcmp(line, "stats") == 0) {
    Serial.printf("rows=%lu backend=%s\n",
                  (unsigned long)g_rows, storageBackendName());
  } else {
    Serial.printf("unknown command: %s (try 'help')\n", line);
  }
}

}  // namespace

StorageBackend storageBegin() {
  g_sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  if (SD.begin(PIN_SD_CS, g_sdSpi)) {
    uint8_t type = SD.cardType();
    if (type != CARD_NONE) {
      g_backend = STORAGE_SD;
      Serial.printf("[storage] microSD mounted, %llu MB\n",
                    (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
    } else {
      SD.end();
    }
  }

  if (g_backend == STORAGE_NONE) {
    // Mount WITHOUT format-on-failure.
    //
    // The tempting idiom here is LittleFS.begin(true), which formats the
    // partition if it will not mount. On this partition table that is a 1.44 MB
    // erase, and esp_partition_erase_range runs with the flash cache disabled -
    // far longer than the 300 ms ESP32 interrupt watchdog will tolerate. The
    // board resets before it can print a single character, which is a miserable
    // thing to debug. So formatting is an explicit, operator-initiated act via
    // the `format` shell command instead of something boot silently attempts.
    if (LittleFS.begin(false)) {
      g_backend = STORAGE_LITTLEFS;
      Serial.println("[storage] no SD card, using internal LittleFS");
    } else {
      Serial.println("[storage] no SD card, and LittleFS will not mount.");
      Serial.println("[storage] logging is DISABLED. Insert a card, or type");
      Serial.println("[storage] 'format' to prepare the internal filesystem.");
      return g_backend;
    }
  }

  ensureHeader(LOG_PATH_RX, kRxHeader);
  ensureHeader(LOG_PATH_GPS, kGpsHeader);
  return g_backend;
}

StorageBackend storageBackend() {
  return g_backend;
}

const char* storageBackendName() {
  switch (g_backend) {
    case STORAGE_SD:       return "SD";
    case STORAGE_LITTLEFS: return "LittleFS";
    default:               return "none";
  }
}

uint64_t storageFreeBytes() {
  switch (g_backend) {
    case STORAGE_SD:       return SD.totalBytes() - SD.usedBytes();
    case STORAGE_LITTLEFS: return LittleFS.totalBytes() - LittleFS.usedBytes();
    default:               return 0;
  }
}

uint32_t storageRowsWritten() {
  return g_rows;
}

bool storageAppendRx(const RxLogRow& row) {
  char iso[32];
  clockFormatIso(row.epochMs, iso, sizeof(iso));

  // Numeric columns that are not meaningful for a given row are written empty
  // rather than as 0, so that a spreadsheet or pandas read_csv sees NaN instead
  // of a real-looking measurement. SNR for FSK is the main case.
  char snr[16]     = "";
  char freqErr[16] = "";
  char offset[16]  = "";
  char rssi[16]    = "";
  char gpsLat[20]  = "";
  char gpsLon[20]  = "";
  char dist[20]    = "";
  char bearing[12] = "";
  char hdop[12]    = "";
  char alt[16]     = "";
  char speed[12]   = "";
  char course[12]  = "";

  if (row.snrValid)           snprintf(snr,     sizeof(snr),     "%.2f", row.snrDb);
  if (row.freqErrorValid)     snprintf(freqErr, sizeof(freqErr), "%.0f", row.freqErrorHz);
  if (row.arrivalOffsetValid) snprintf(offset,  sizeof(offset),  "%ld",  (long)row.arrivalOffsetMs);
  if (strcmp(row.outcome, OUTCOME_FAIL) != 0)
                              snprintf(rssi,    sizeof(rssi),    "%.1f", row.rssiDbm);
  if (row.gpsValid) {
    snprintf(gpsLat, sizeof(gpsLat), "%.6f", row.rxLat);
    snprintf(gpsLon, sizeof(gpsLon), "%.6f", row.rxLon);
    snprintf(dist,   sizeof(dist),   "%.1f", row.distanceM);
    snprintf(bearing, sizeof(bearing), "%.1f", row.bearingDeg);
    snprintf(alt,    sizeof(alt),    "%.1f", row.gpsAltitudeM);
    snprintf(speed,  sizeof(speed),  "%.2f", row.gpsSpeedKmh);
    snprintf(course, sizeof(course), "%.1f", row.gpsCourseDeg);
  }
  // HDOP is reported by the module whenever it has any solution at all, so it
  // is worth recording even on rows where the position itself is not usable.
  if (row.gpsHdop > 0.0) {
    snprintf(hdop, sizeof(hdop), "%.2f", row.gpsHdop);
  }

  // Two buffers: the fixed part, then the payload appended separately, because
  // 128 hex characters alone would blow a single stack buffer of a sane size.
  char head[448];
  snprintf(head, sizeof(head),
           "%s,%lu,%u,%u,%s,%s,%s,"
           "%s,%s,%d,%lu,%s,%s,%s,%s,%lu,"
           "%.6f,%.6f,%s,%s,"
           "%s,%s,%s,%s,"
           "%lu,%lu,%s,"
           "%lu,%u,%u,",
           iso,
           (unsigned long)row.roundIndex,
           (unsigned)row.slot,
           (unsigned)row.repeat,
           row.modem,
           row.profileLabel,
           row.params,
           gpsLat, gpsLon, row.gpsValid ? 1 : 0,
           (unsigned long)row.gpsSatellites, hdop, alt, speed, course,
           (unsigned long)row.gpsAgeMs,
           row.txLat, row.txLon, dist, bearing,
           row.outcome, rssi, snr, freqErr,
           (unsigned long)row.toaCalcMs,
           (unsigned long)row.toaTxReportedMs,
           offset,
           (unsigned long)row.txSequence,
           (unsigned)row.txRoundId,
           (unsigned)row.payloadLen);

  fs::FS* fs = activeFs();
  if (!fs) {
    return false;
  }
  File f = fs->open(LOG_PATH_RX, FILE_APPEND);
  if (!f) {
    Serial.println("[storage] rx log append failed");
    return false;
  }
  f.print(head);
  if (row.payloadHex) {
    f.print(row.payloadHex);
  }
  f.println();
  f.close();
  ++g_rows;
  return true;
}

bool storageAppendGps(const GpsLogRow& row) {
  char iso[32];
  clockFormatIso(row.epochMs, iso, sizeof(iso));

  char line[224];
  if (row.fixValid) {
    snprintf(line, sizeof(line),
             "%s,1,%lu,%.2f,%.6f,%.6f,%.1f,%.2f,%.1f,%.1f,%.1f",
             iso, (unsigned long)row.satellites, row.hdop,
             row.lat, row.lon, row.altitudeM,
             row.speedKmh, row.courseDeg, row.distanceToTxM,
             row.bearingToTxDeg);
  } else {
    snprintf(line, sizeof(line), "%s,0,%lu,,,,,,,,",
             iso, (unsigned long)row.satellites);
  }
  return appendLine(LOG_PATH_GPS, line);
}

void storageShellSetHook(ShellHook hook, const char* extraHelp) {
  g_hook      = hook;
  g_extraHelp = extraHelp;
}

void storageShellPoll() {
  static char   buf[96];
  static size_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      buf[len] = '\0';
      shellExecute(buf);
      len = 0;
    } else if (len + 1 < sizeof(buf)) {
      buf[len++] = c;
    } else {
      // Overlong line: drop it rather than half-executing it.
      len = 0;
    }
  }
}
