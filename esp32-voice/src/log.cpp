#include "log.h"

#include <Arduino.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <string.h>

// The record must stay exactly 16 bytes: the ring arithmetic assumes a fixed
// stride, the RTC budget assumes this size, and an offline decoder reads it as
// a packed array. A silent change here corrupts every dump ever taken.
static_assert(sizeof(LogRecord) == 16, "LogRecord must be exactly 16 bytes");

// -----------------------------------------------------------------------------
// The ring, in RTC slow memory.
//
// RTC_NOINIT_ATTR means the startup code does NOT zero it, which is the whole
// point - it is what lets the log survive a restart. It also means that on a
// cold power-up the contents are whatever the SRAM cells happened to settle
// to, and uninitialised SRAM is perfectly capable of looking like a valid log.
// Hence the magic and the hash below: this data is never trusted, only
// checked.
// -----------------------------------------------------------------------------
#define LOG_MAGIC 0x564C4F47UL   // 'VLOG'

struct LogRing {
  uint32_t  magic;
  uint32_t  hash;      // over every header field except itself
  uint32_t  format;    // LOG_FORMAT_VERSION the ring was written with
  uint32_t  total;     // records ever written; keeps counting past the wrap
  uint32_t  flushed;   // records already written to flash, as a `total` index
  uint32_t  boots;     // boots since the ring was last cleared
  uint16_t  head;      // index the next record goes to
  uint16_t  count;     // records held, <= LOG_CAPACITY
  LogRecord rec[LOG_CAPACITY];
};

static RTC_NOINIT_ATTR LogRing ring;

// Written by logBegin() in normal RAM, because it describes this boot rather
// than the ring.
static bool survived = false;

// Minimum severity that reaches the ring. Lives in normal RAM, not in the
// ring: it is a setting, not a record, and it is reapplied from the config
// store on every boot. Defaults to INFO so that a board whose configuration
// has not loaded yet - which is every board, for the first few milliseconds -
// behaves exactly as it did before this existed.
static LogLevel minLevel = LOG_LVL_INFO;

// The ring is appended to from several tasks, and logWrite() is deliberately
// safe to call from an interrupt. A spinlock is the right primitive: it holds
// for the few instructions needed to bump the head and copy 16 bytes, and
// unlike a mutex it can be taken with the scheduler suspended.
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

// FNV-1a over the header fields. Not cryptographic and not meant to be - its
// only job is to make uninitialised SRAM overwhelmingly unlikely to pass.
static uint32_t headerHash() {
  uint32_t h = 2166136261UL;
  const uint32_t fields[] = {
    ring.magic, ring.format, ring.total, ring.flushed, ring.boots,
    (uint32_t)ring.head, (uint32_t)ring.count,
  };
  for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    const uint32_t v = fields[i];
    for (int b = 0; b < 4; b++) {
      h ^= (v >> (b * 8)) & 0xFF;
      h *= 16777619UL;
    }
  }
  return h;
}

static void reinit() {
  memset(&ring, 0, sizeof(ring));
  ring.magic  = LOG_MAGIC;
  ring.format = LOG_FORMAT_VERSION;
  ring.hash   = headerHash();
}

static bool ringValid() {
  if (ring.magic != LOG_MAGIC) return false;
  if (ring.format != LOG_FORMAT_VERSION) return false;
  // Bounds first: a corrupted head or count would make the reader walk off the
  // end of the array, and that is a crash rather than a wrong answer.
  if (ring.head >= LOG_CAPACITY) return false;
  if (ring.count > LOG_CAPACITY) return false;
  return ring.hash == headerHash();
}

static void flashBegin();
static void writeRecord(LogLevel level, LogModule module, LogEvent code,
                        int32_t a, int32_t b);

void logBegin() {
  survived = ringValid();
  if (!survived) {
    // Either a cold power-up, a format change, or genuine corruption. In every
    // case the safe move is the same: start again rather than decode garbage.
    reinit();
  }

  ring.boots++;
  ring.hash = headerHash();

  // esp_reset_reason() is the single most useful number in the whole log: it
  // separates "somebody pressed reset" from "the watchdog fired" from "it
  // panicked", and it is only available this early.
  //
  // Written unfiltered, through writeRecord() rather than logInfo(). This
  // record is also the boundary between one boot and the next in a dump;
  // without it, several boots' records run together with a timestamp that
  // restarts in the middle and nothing to explain why. A quieter log is worth
  // having, an unreadable one is not.
  writeRecord(LOG_LVL_INFO, LOG_MOD_BOOT, LOG_EV_BOOT,
              (int32_t)esp_reset_reason(), (int32_t)ring.boots);

  // Open the flash tier last, so that the BOOT record above is already in the
  // ring and gets flushed with everything else. On a warm reset this is also
  // where records that survived but had not reached flash get picked up: they
  // are still counted as unflushed, because `flushed` lives in the ring and
  // survived alongside them.
  flashBegin();
}

void logSetMinLevel(LogLevel level) {
  minLevel = (level > LOG_LVL_ERROR) ? LOG_LVL_ERROR : level;
}

LogLevel logMinLevel() { return minLevel; }

// The actual append. Bypasses the level filter, which is why it is private:
// exactly one caller needs that, and it is the BOOT record.
static void writeRecord(LogLevel level, LogModule module, LogEvent code,
                        int32_t a, int32_t b) {
  LogRecord r;
  r.ms     = millis();
  r.level  = (uint8_t)level;
  r.module = (uint8_t)module;
  r.code   = (uint16_t)code;
  r.a      = a;
  r.b      = b;

  portENTER_CRITICAL(&lock);
  ring.rec[ring.head] = r;
  ring.head = (uint16_t)((ring.head + 1) % LOG_CAPACITY);
  if (ring.count < LOG_CAPACITY) ring.count++;
  ring.total++;
  ring.hash = headerHash();
  portEXIT_CRITICAL(&lock);
}

void logWrite(LogLevel level, LogModule module, LogEvent code,
              int32_t a, int32_t b) {
  // Discarded here, before the record exists. See the minimum-level note in
  // log.h for why the filter is on the way in rather than on the way out.
  if (level < minLevel) return;
  writeRecord(level, module, code, a, b);
}

uint16_t logCount()         { return ring.count; }
uint32_t logTotalWritten()  { return ring.total; }
uint32_t logBootCount()     { return ring.boots; }
bool     logSurvivedReset() { return survived; }

bool logRead(uint16_t index, LogRecord* out) {
  if (index >= ring.count || out == nullptr) return false;
  // The oldest record is `count` positions behind the head, modulo the ring.
  // Adding LOG_CAPACITY before the modulo keeps the arithmetic in unsigned
  // range when head < count.
  const uint16_t start = (uint16_t)((ring.head + LOG_CAPACITY - ring.count) % LOG_CAPACITY);
  portENTER_CRITICAL(&lock);
  *out = ring.rec[(start + index) % LOG_CAPACITY];
  portEXIT_CRITICAL(&lock);
  return true;
}

void logClear() {
  portENTER_CRITICAL(&lock);
  reinit();
  portEXIT_CRITICAL(&lock);
}

// =============================================================================
// FLASH TIER
//
// The `eventlog` partition, treated as 16 independent erase sectors used as a
// circular buffer at SECTOR granularity. Each sector opens with a 16-byte
// header carrying a monotonic sequence number, followed by 255 records.
//
// Sector-granular rather than record-granular because NOR flash erases in
// sectors and nothing smaller. Wrapping therefore means "erase the oldest
// sector and start filling it", which loses 255 records at a time - the price
// of not having a filesystem, and the reason the partition is sized to hold
// twenty times the RTC ring.
//
// Finding the write position at boot needs no stored pointer, which is the
// property that makes this survivable: a pointer is a thing that can be stale
// or half-written, whereas the sequence numbers and the erased-slot pattern
// are derived from the data itself.
//
//   1. read all 16 sector headers; the valid one with the highest seq is the
//      sector currently being filled
//   2. within it, the first all-0xFF slot is where the next record goes
//
// An interrupted write can leave one record partly programmed. That is
// detected on read rather than prevented on write - see logFlashRead().
// =============================================================================
#define LOG_SECTOR_SIZE    4096
#define LOG_SECTOR_HDR     16
#define LOG_RECS_PER_SECTOR ((LOG_SECTOR_SIZE - LOG_SECTOR_HDR) / (int)sizeof(LogRecord))
#define LOG_SECTOR_MAGIC   0x53474C56UL   // 'VLGS'

struct LogSectorHeader {
  uint32_t magic;
  uint32_t seq;       // monotonic; identifies the newest sector
  uint16_t format;
  uint16_t records;   // capacity, recorded for a reader's benefit
  uint32_t hash;
};
static_assert(sizeof(LogSectorHeader) == LOG_SECTOR_HDR,
              "sector header must be 16 bytes");

static const esp_partition_t* part = nullptr;
static uint16_t sectorCount = 0;
static uint16_t curSector = 0;      // sector being filled
static uint32_t curSeq = 0;         // its sequence number
static uint16_t curSlot = 0;        // next free record slot within it
static uint32_t lastFlushMs = 0;

static uint32_t sectorHash(const LogSectorHeader& h) {
  uint32_t v = 2166136261UL;
  const uint32_t f[] = { h.magic, h.seq,
                         (uint32_t)h.format | ((uint32_t)h.records << 16) };
  for (unsigned i = 0; i < 3; i++) {
    for (int b = 0; b < 4; b++) {
      v ^= (f[i] >> (b * 8)) & 0xFF;
      v *= 16777619UL;
    }
  }
  return v;
}

static bool readSectorHeader(uint16_t sector, LogSectorHeader* h) {
  if (esp_partition_read(part, (size_t)sector * LOG_SECTOR_SIZE,
                         h, sizeof(*h)) != ESP_OK) {
    return false;
  }
  return h->magic == LOG_SECTOR_MAGIC &&
         h->format == LOG_FORMAT_VERSION &&
         h->hash == sectorHash(*h);
}

// Erases a sector and writes its header. This is the only place that erases,
// and it is what makes the sequence numbers monotonic.
static bool openSector(uint16_t sector, uint32_t seq) {
  if (esp_partition_erase_range(part, (size_t)sector * LOG_SECTOR_SIZE,
                                LOG_SECTOR_SIZE) != ESP_OK) {
    return false;
  }
  LogSectorHeader h;
  h.magic   = LOG_SECTOR_MAGIC;
  h.seq     = seq;
  h.format  = LOG_FORMAT_VERSION;
  h.records = (uint16_t)LOG_RECS_PER_SECTOR;
  h.hash    = sectorHash(h);
  if (esp_partition_write(part, (size_t)sector * LOG_SECTOR_SIZE,
                          &h, sizeof(h)) != ESP_OK) {
    return false;
  }
  curSector = sector;
  curSeq    = seq;
  curSlot   = 0;
  return true;
}

static bool slotErased(const LogRecord& r) {
  const uint8_t* p = (const uint8_t*)&r;
  for (unsigned i = 0; i < sizeof(LogRecord); i++) {
    if (p[i] != 0xFF) return false;
  }
  return true;
}

// Scans the partition to work out where to append next. Called once, at boot.
static void flashScan() {
  uint32_t bestSeq = 0;
  int      best    = -1;

  for (uint16_t s = 0; s < sectorCount; s++) {
    LogSectorHeader h;
    if (!readSectorHeader(s, &h)) continue;
    if (best < 0 || h.seq > bestSeq) { bestSeq = h.seq; best = s; }
  }

  if (best < 0) {
    // Nothing valid anywhere: a fresh partition, or one wiped by a reflash.
    openSector(0, 1);
    return;
  }

  curSector = (uint16_t)best;
  curSeq    = bestSeq;

  // First erased slot in that sector is the append point. A linear scan of 255
  // slots is 4 kB of reads, once, at boot - not worth a binary search, and a
  // linear scan is correct even if the sector somehow has a hole in it.
  curSlot = LOG_RECS_PER_SECTOR;
  for (uint16_t i = 0; i < LOG_RECS_PER_SECTOR; i++) {
    LogRecord r;
    const size_t off = (size_t)curSector * LOG_SECTOR_SIZE + LOG_SECTOR_HDR +
                       (size_t)i * sizeof(LogRecord);
    if (esp_partition_read(part, off, &r, sizeof(r)) != ESP_OK) break;
    if (slotErased(r)) { curSlot = i; break; }
  }
}

static bool flashAppend(const LogRecord& r) {
  if (!part) return false;

  if (curSlot >= LOG_RECS_PER_SECTOR) {
    // Full. Move to the next sector, erasing whatever was there - which is the
    // oldest data in the partition, because sectors are filled in order.
    const uint16_t next = (uint16_t)((curSector + 1) % sectorCount);
    if (!openSector(next, curSeq + 1)) return false;
  }

  const size_t off = (size_t)curSector * LOG_SECTOR_SIZE + LOG_SECTOR_HDR +
                     (size_t)curSlot * sizeof(LogRecord);
  if (esp_partition_write(part, off, &r, sizeof(r)) != ESP_OK) return false;
  curSlot++;
  return true;
}

static void flashBegin() {
  // By name rather than by subtype: the subtype is an arbitrary number we
  // picked, and a name is checkable against partitions_voice.csv by eye.
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_ANY, "eventlog");
  if (!part) {
    Serial.println("[log] no `eventlog` partition - history will not survive power off");
    Serial.println("[log] check board_build.partitions in platformio.ini");
    return;
  }
  sectorCount = (uint16_t)(part->size / LOG_SECTOR_SIZE);
  flashScan();
  Serial.printf("[log] flash log: %u sectors, %lu of %lu records used\n",
                (unsigned)sectorCount, (unsigned long)logFlashCount(),
                (unsigned long)logFlashCapacity());
}

bool     logFlashAvailable() { return part != nullptr; }
uint32_t logFlashCapacity()  { return (uint32_t)sectorCount * LOG_RECS_PER_SECTOR; }

// Records stored. Sectors before the current one in sequence order are full;
// the current one holds curSlot.
uint32_t logFlashCount() {
  if (!part) return 0;
  uint32_t full = 0;
  for (uint16_t s = 0; s < sectorCount; s++) {
    LogSectorHeader h;
    if (s != curSector && readSectorHeader(s, &h)) full++;
  }
  return full * LOG_RECS_PER_SECTOR + curSlot;
}

// Walks sectors oldest-first. The oldest is the valid sector with the lowest
// sequence number, which after a wrap is the one immediately after the current
// one - but deriving it from the sequence numbers rather than assuming that
// keeps this correct on a partially filled partition too.
bool logFlashRead(uint32_t index, LogRecord* out, bool* torn) {
  if (!part || !out) return false;
  if (torn) *torn = false;

  // Build the sector visit order: every valid sector, sorted by sequence.
  uint16_t order[64];
  uint32_t seqs[64];
  uint16_t n = 0;
  for (uint16_t s = 0; s < sectorCount && n < 64; s++) {
    LogSectorHeader h;
    if (!readSectorHeader(s, &h)) continue;
    // Insertion sort - at most 16 entries, and it keeps this allocation-free.
    uint16_t p = n;
    while (p > 0 && seqs[p - 1] > h.seq) { seqs[p] = seqs[p - 1]; order[p] = order[p - 1]; p--; }
    seqs[p] = h.seq;
    order[p] = s;
    n++;
  }

  for (uint16_t i = 0; i < n; i++) {
    const uint16_t s = order[i];
    const uint16_t held = (s == curSector) ? curSlot : (uint16_t)LOG_RECS_PER_SECTOR;
    if (index >= held) { index -= held; continue; }

    const size_t off = (size_t)s * LOG_SECTOR_SIZE + LOG_SECTOR_HDR +
                       (size_t)index * sizeof(LogRecord);
    if (esp_partition_read(part, off, out, sizeof(*out)) != ESP_OK) return false;

    // A half-programmed record is flash that was erased to 0xFF and then only
    // partly written. At most one per power cut, always the newest. The level
    // and module fields are the cheapest tell: both have a handful of legal
    // values and 0xFF is not one of them.
    if (torn && (out->level > LOG_LVL_ERROR || out->module >= LOG_MOD_COUNT)) {
      *torn = true;
    }
    return true;
  }
  return false;
}

uint32_t logUnflushed() {
  const uint32_t total = ring.total;
  const uint32_t flushed = ring.flushed;
  return (total > flushed) ? (total - flushed) : 0;
}

uint32_t logFlush() {
  if (!part) return 0;

  uint32_t from = ring.flushed;
  const uint32_t to = ring.total;
  if (from >= to) return 0;

  // Records that fell out of the ring before they were flushed are simply
  // gone. That happens only if more than LOG_CAPACITY records are written
  // between two flushes, which at this event rate means something is very
  // wrong - so clamp rather than walk off the start of the ring.
  if (to - from > ring.count) from = to - ring.count;

  uint32_t written = 0;
  for (uint32_t t = from; t < to; t++) {
    // Ring index of the record whose `total` index is t. The oldest held
    // record has total index (to - count), so index = t - (to - count).
    const uint16_t idx = (uint16_t)(t - (to - ring.count));
    LogRecord r;
    if (!logRead(idx, &r)) break;
    if (!flashAppend(r)) break;
    written++;
  }

  // Advance by what actually reached flash, not by what we set out to write.
  // If flashAppend() failed part way - a bad sector, or the erase before a
  // wrap not completing - marking everything flushed would quietly drop the
  // remainder, and the next flush would carry on from past them.
  portENTER_CRITICAL(&lock);
  ring.flushed = from + written;
  ring.hash = headerHash();
  portEXIT_CRITICAL(&lock);

  lastFlushMs = millis();
  return written;
}

void logService(bool idle) {
  if (!part) return;
  const uint32_t pending = logUnflushed();
  if (pending == 0) return;

  // Two triggers. The count one bounds how much a power cut can lose when the
  // handset is busy; the time one bounds it when it is not.
  const bool urgent = pending >= (LOG_CAPACITY / 2);
  const bool due    = (millis() - lastFlushMs) >= 10000UL;
  if (!urgent && !due) return;

  // Idle only. esp_partition_write disables the instruction cache on BOTH
  // cores while it runs, so a flush during a transmission stalls the codec and
  // glitches the audio. Waiting is free; the records are safe in RTC memory
  // and only a power cut in the next few seconds would lose them.
  //
  // The one exception is `urgent`: if the ring is half full of unflushed
  // records we are losing history either way, so take the glitch.
  if (!idle && !urgent) return;

  const uint32_t n = logFlush();
  if (n) {
    Serial.printf("[log] flushed %lu records to flash (%lu of %lu used)\n",
                  (unsigned long)n, (unsigned long)logFlashCount(),
                  (unsigned long)logFlashCapacity());
  }
}

void logEraseFlash() {
  if (!part) return;
  esp_partition_erase_range(part, 0, part->size);
  curSector = curSlot = 0;
  curSeq = 0;
  openSector(0, 1);
  portENTER_CRITICAL(&lock);
  ring.flushed = ring.total;
  ring.hash = headerHash();
  portEXIT_CRITICAL(&lock);
}

// -----------------------------------------------------------------------------
// Decoding tables.
//
// These never return null and never assert on an unknown value: a dump taken
// from a newer build is decoded against an older table all the time, and
// "event 84" is a far more useful thing to print than a crash.
// -----------------------------------------------------------------------------
const char* logLevelName(uint8_t level) {
  switch (level) {
    case LOG_LVL_DEBUG: return "D";
    case LOG_LVL_INFO:  return "I";
    case LOG_LVL_WARN:  return "W";
    case LOG_LVL_ERROR: return "E";
    default:            return "?";
  }
}

const char* logModuleName(uint8_t module) {
  static const char* const NAMES[LOG_MOD_COUNT] = {
    "boot", "post", "radio", "audio", "codec", "crypto", "config", "app", "ui",
  };
  return (module < LOG_MOD_COUNT) ? NAMES[module] : "?";
}

const char* logEventName(uint16_t code) {
  switch (code) {
    case LOG_EV_BOOT:           return "BOOT";
    case LOG_EV_BOOT_READY:     return "BOOT_READY";
    case LOG_EV_HALT:           return "HALT";
    case LOG_EV_STACK_LOW:      return "STACK_LOW";
    case LOG_EV_POST_ITEM:      return "POST_ITEM";
    case LOG_EV_POST_SUMMARY:   return "POST_SUMMARY";
    case LOG_EV_RADIO_UP:       return "RADIO_UP";
    case LOG_EV_RADIO_FAIL:     return "RADIO_FAIL";
    case LOG_EV_PRESET:         return "PRESET";
    case LOG_EV_TX_START:       return "TX_START";
    case LOG_EV_TX_END:         return "TX_END";
    case LOG_EV_RX_START:       return "RX_START";
    case LOG_EV_RX_END:         return "RX_END";
    case LOG_EV_RX_REJECT:      return "RX_REJECT";
    case LOG_EV_DUTY_OVER:      return "DUTY_OVER";
    case LOG_EV_MIC_PROBE:      return "MIC_PROBE";
    case LOG_EV_AUDIO_CLIP:     return "AUDIO_CLIP";
    case LOG_EV_AUDIO_UNDERRUN: return "AUDIO_UNDERRUN";
    case LOG_EV_SELFTEST:       return "SELFTEST";
    case LOG_EV_CODEC_UP:       return "CODEC_UP";
    case LOG_EV_CODEC_SLOW:     return "CODEC_SLOW";
    case LOG_EV_CRYPTO_KEY:     return "CRYPTO_KEY";
    case LOG_EV_ENC_STATE:      return "ENC_STATE";
    case LOG_EV_CFG_LOAD:       return "CFG_LOAD";
    case LOG_EV_CFG_DEFAULTS:   return "CFG_DEFAULTS";
    case LOG_EV_CFG_SAVE:       return "CFG_SAVE";
    case LOG_EV_CFG_SET:        return "CFG_SET";
    case LOG_EV_CFG_RESET:      return "CFG_RESET";
    case LOG_EV_CFG_BAD:        return "CFG_BAD";
    case LOG_EV_CFG_MIGRATE:      return "CFG_MIGRATE";
    case LOG_EV_CFG_MIGRATE_FAIL: return "CFG_MIGRATE_FAIL";
    default:                    return "";   // caller prints the number
  }
}
