// =============================================================================
// log.h - a fixed-record binary event log in a ring buffer.
//
// This is not a replacement for Serial.printf(). Printf is for a human with a
// terminal attached at the time. This is for the question you ask afterwards:
// the board rebooted in the field an hour ago, what led up to it?
//
// Three properties follow from that, and each one costs something:
//
//   BINARY, FIXED 16-BYTE RECORDS
//     No strings, no formatting, no varargs. An event is an enum plus two
//     signed integers, and what those integers mean is documented per event.
//     That makes a record cheap enough to write from anywhere - including an
//     interrupt - and makes the ring arithmetic trivial, because every record
//     is the same size. The cost is that you cannot log something you did not
//     define an event for, which is the point: it keeps the log a record of
//     things that happen rather than a stream of prose.
//
//   TWO TIERS: RTC MEMORY, THEN FLASH
//     The hot ring is RTC_NOINIT_ATTR, which survives a software restart, a
//     panic and a watchdog reset. It is free to write - no erase, no wear, no
//     cache stall - which is what makes logWrite() cheap enough to call from
//     an interrupt.
//
//     RTC memory does NOT survive power being removed, so records are also
//     flushed to a dedicated 64 kB flash partition. That flush is the exact
//     opposite of cheap: it stalls both CPUs while the cache is disabled and
//     it wears the flash, so it happens on a timer, in a task, and only while
//     the handset is idle - never from the audio path and never from an ISR.
//
//     The result is that a reset loses nothing, and a power cut loses at most
//     the last few seconds. See logService() and docs/02-logging.md.
//
//   VALIDATED ON EVERY BOOT
//     Uninitialised RTC memory on a cold power-up is whatever was in the SRAM
//     cells, which will happily look like a log. The header carries a magic
//     and a hash; if either fails the ring is reinitialised rather than
//     decoded into fiction.
//
// See docs/02-logging.md for the wire format and the offline decoder.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// Bump when the record layout or the event numbering changes incompatibly.
// It is emitted in the `log raw` header so an offline decoder knows what it is
// looking at, and a mismatched ring is discarded rather than misread.
#define LOG_FORMAT_VERSION 1

// Records held. 192 x 16 bytes is 3 kB of the 8 kB of RTC slow RAM, which
// leaves room for the bootloader's own RTC data and anything added later.
#define LOG_CAPACITY 192

enum LogLevel : uint8_t {
  LOG_LVL_DEBUG = 0,
  LOG_LVL_INFO  = 1,
  LOG_LVL_WARN  = 2,
  LOG_LVL_ERROR = 3,
};

// Which subsystem emitted the record. Kept separate from the event code so the
// log can be filtered by area without knowing every event number.
enum LogModule : uint8_t {
  LOG_MOD_BOOT = 0,
  LOG_MOD_POST,
  LOG_MOD_RADIO,
  LOG_MOD_AUDIO,
  LOG_MOD_CODEC,
  LOG_MOD_CRYPTO,
  LOG_MOD_CONFIG,
  LOG_MOD_APP,
  LOG_MOD_UI,
  LOG_MOD_COUNT,
};

// -----------------------------------------------------------------------------
// Events. The meaning of `a` and `b` is per-event and is documented here,
// because in a binary log this comment is the only place it exists.
//
// Numbers are explicit and MUST NOT be reused or renumbered - a log dumped
// from an older build is decoded against a newer table all the time. Retire a
// code by leaving a gap, never by recycling it.
// -----------------------------------------------------------------------------
enum LogEvent : uint16_t {
  LOG_EV_NONE           = 0,

  // boot
  LOG_EV_BOOT           = 1,   // a = esp_reset_reason(), b = boot count
  LOG_EV_BOOT_READY     = 2,   // a = ms taken by setup(), b = free heap kB
  LOG_EV_HALT           = 3,   // a = which fatal step (see main.cpp)
  LOG_EV_STACK_LOW      = 4,   // a = task id, b = bytes never used

  // power-on self test
  LOG_EV_POST_ITEM      = 10,  // a = PostItem, b = PostResult
  LOG_EV_POST_SUMMARY   = 11,  // a = failures, b = warnings

  // radio
  LOG_EV_RADIO_UP       = 20,  // a = frequency kHz, b = dBm
  LOG_EV_RADIO_FAIL     = 21,  // a = RadioLib error code
  LOG_EV_PRESET         = 22,  // a = preset index, b = airtime ms
  LOG_EV_TX_START       = 23,  // a = stream id low 16, b = preset
  LOG_EV_TX_END         = 24,  // a = packets sent, b = ms keyed
  LOG_EV_RX_START       = 25,  // a = station id, b = rssi dBm
  LOG_EV_RX_END         = 26,  // a = packets, b = 1 if signed off, 0 if lost
  LOG_EV_RX_REJECT      = 27,  // a = reason (LinkStats field), b = count
  LOG_EV_DUTY_OVER      = 28,  // a = duty x100, b = limit x100

  // audio
  LOG_EV_MIC_PROBE      = 40,  // a = slot, b = confidence %  (a<0: not found)
  LOG_EV_AUDIO_CLIP     = 41,  // a = clipped samples this transmission
  LOG_EV_AUDIO_UNDERRUN = 42,  // a = underruns this stream
  LOG_EV_SELFTEST       = 43,  // a = frames recorded, b = ms

  // codec
  LOG_EV_CODEC_UP       = 50,  // a = mode id, b = bytes per frame
  LOG_EV_CODEC_SLOW     = 51,  // a = encode us, b = frame budget us

  // crypto
  LOG_EV_CRYPTO_KEY     = 60,  // a = 1 loaded / 0 absent, b = fingerprint
  LOG_EV_ENC_STATE      = 61,  // a = 1 armed / 0 clear

  // configuration
  LOG_EV_CFG_LOAD       = 70,  // a = stored version, b = bytes
  LOG_EV_CFG_DEFAULTS   = 71,  // a = why (ConfigLoadResult)
  LOG_EV_CFG_SAVE       = 72,  // a = version, b = bytes
  LOG_EV_CFG_SET        = 73,  // a = field id, b = new value (or 0 for blobs)
  LOG_EV_CFG_RESET      = 74,
  LOG_EV_CFG_BAD        = 75,  // a = ConfigLoadResult
  LOG_EV_CFG_MIGRATE      = 76,  // a = from version, b = to version
  LOG_EV_CFG_MIGRATE_FAIL = 77,  // a = from version, b = to version (or -1 for
                                 //     a size mismatch, 0 for no migration)
};

// One record. Exactly 16 bytes - see the static_assert in log.cpp.
struct LogRecord {
  uint32_t ms;       // millis() when written; restarts at each boot
  uint8_t  level;    // LogLevel
  uint8_t  module;   // LogModule
  uint16_t code;     // LogEvent
  int32_t  a;
  int32_t  b;
};

// Validates or reinitialises the ring, then records the boot. Call this first,
// before anything that might want to log - which is everything.
void logBegin();

// Append one record. Safe from any task and from an interrupt: it takes a
// spinlock for the handful of instructions needed to bump the head, and does
// no allocation, no formatting and no I/O.
void logWrite(LogLevel level, LogModule module, LogEvent code,
              int32_t a, int32_t b);

// =============================================================================
// Minimum level
//
// Records below the threshold are discarded at logWrite() and never enter the
// ring at all. Filtering on the way IN rather than on the way out is the right
// choice for a fixed-size ring: it means the 192 slots hold 192 records you
// care about, instead of 192 slots of which most are noise. It also keeps
// filtered records off the flash tier entirely, and flash writes are the only
// expensive thing this subsystem does.
//
// The cost is the usual one: a record filtered out is gone, not hidden. Raise
// the threshold to make the log quieter, not to make it faster.
//
// STRUCTURAL RECORDS IGNORE THIS. The per-boot BOOT record is written whatever
// the threshold says, because it carries the reset reason and it is the
// boundary that separates one boot from the next in a dump. Losing it would
// not make the log quieter, it would make it unreadable - several boots'
// records would run together with a timestamp that restarts in the middle and
// nothing to say why.
//
// Set from the `loglevel` configuration field; see docs/03-config.md.
// =============================================================================
void     logSetMinLevel(LogLevel level);
LogLevel logMinLevel();

// Convenience wrappers. The `b` argument defaults to zero because most events
// only carry one number.
//
// Nothing currently emits at DEBUG. The level exists so that adding tracing
// later does not also require inventing a way to switch it off - which is the
// point at which people give up and use Serial.printf instead.
inline void logDebug(LogModule m, LogEvent e, int32_t a = 0, int32_t b = 0) {
  logWrite(LOG_LVL_DEBUG, m, e, a, b);
}
inline void logInfo(LogModule m, LogEvent e, int32_t a = 0, int32_t b = 0) {
  logWrite(LOG_LVL_INFO, m, e, a, b);
}
inline void logWarn(LogModule m, LogEvent e, int32_t a = 0, int32_t b = 0) {
  logWrite(LOG_LVL_WARN, m, e, a, b);
}
inline void logError(LogModule m, LogEvent e, int32_t a = 0, int32_t b = 0) {
  logWrite(LOG_LVL_ERROR, m, e, a, b);
}

// Records currently held (up to LOG_CAPACITY), and the total ever written
// since the ring was last cleared - which keeps counting across the wrap, so
// the difference tells you how many records were lost to it.
uint16_t logCount();
uint32_t logTotalWritten();

// Boots since the ring was last cleared. Because the ring survives a reset,
// this is how you tell "it has restarted forty times" from "it started once".
uint32_t logBootCount();

// True if the ring came back intact from a previous run rather than being
// initialised fresh. False means either a cold power-up or a corrupted ring.
bool logSurvivedReset();

// Reads record `index`, 0 being the OLDEST held. False if out of range.
bool logRead(uint16_t index, LogRecord* out);

// Clears the RTC ring only. The flash history is untouched - see logEraseFlash.
void logClear();

// Names for decoding. Never return null - an unknown code prints as its
// number, because a log from a newer build should still be readable.
const char* logLevelName(uint8_t level);
const char* logModuleName(uint8_t module);
const char* logEventName(uint16_t code);

// =============================================================================
// Flash tier - the half that survives power being removed.
//
// A dedicated 64 kB partition (`eventlog`, see partitions_voice.csv), organised
// as 16 erase sectors. Each sector holds a 16-byte header and 255 records, so
// the partition holds 4080 records - about twenty times the RTC ring.
//
// Wear is not a concern at this rate and it is worth showing why rather than
// asserting it: a sector is erased once per 255 records, so even at 500
// records a day each sector sees an erase about every eight days. NOR flash is
// good for 100,000 cycles, which is over two thousand years. The thing that
// would break that is logging in the audio path, which is why nothing does.
// =============================================================================

// True if the partition was found and opened. False is not fatal - the RTC
// ring still works, the history just stops at the next power cut.
bool logFlashAvailable();

// Records currently stored in flash, and how many the partition can hold.
uint32_t logFlashCount();
uint32_t logFlashCapacity();

// Reads flash record `index`, 0 being the OLDEST stored.
//
// `torn` is set when the record's level or module field is out of range, which
// is what a half-written record looks like: flash that was erased to 0xFF and
// then only partly programmed. At most one record per power cut can be in that
// state, and it is always the newest. Reporting it is better than hiding it.
bool logFlashRead(uint32_t index, LogRecord* out, bool* torn);

// Writes everything the ring holds that has not reached flash yet. Returns the
// number of records written. Safe to call at any time from a task; never from
// an interrupt, and never while audio is running - see logService().
uint32_t logFlush();

// Records written to the ring but not yet to flash.
uint32_t logUnflushed();

// Call periodically from a low-priority context - loop() is the right place.
//
// `idle` must be true only when nothing time-critical is running. Writing to
// flash disables the instruction cache on BOTH cores for the duration, so a
// flush during a transmission would stall the codec and glitch the audio. The
// caller decides, because the caller is the only thing that knows.
void logService(bool idle);

// Erases the whole flash history. Deliberately separate from logClear(), and
// deliberately not called by anything automatic.
void logEraseFlash();
