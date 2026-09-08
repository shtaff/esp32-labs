// =============================================================================
// configstore.h - persistent, versioned, validated settings in NVS.
//
// config.h holds BUILD-time constants. This holds the handful of settings that
// have to survive a power cycle and be changeable without a toolchain: the
// encryption key above all, because a key you can only change by rebuilding is
// a key nobody ever changes.
//
// -----------------------------------------------------------------------------
// SHAPE
//
// One packed blob under one NVS key, not a key per setting. That is deliberate:
//
//   - it is one write, so a set is one atomic-ish operation rather than eight
//     independent ones that can be interrupted half way through;
//   - a CRC over the whole blob catches a torn or corrupted write, which
//     per-key storage cannot do at all;
//   - the version and size fields sit next to the data they describe, so a
//     blob from an older firmware is recognisable as such rather than being
//     silently reinterpreted through the current struct layout.
//
// -----------------------------------------------------------------------------
// VALIDATION
//
// Every field has a declared type and range in a table (configstore.cpp), and
// `config set` refuses anything outside it. That is not politeness: these
// values reach a shift count, a DMA size and a radio power register, and a
// bad one is somewhere between silence and a chip that will not boot.
//
// The blob is validated again on LOAD, not just on set - flash rots, firmware
// gets downgraded, and a value that was legal under a previous build's table
// may not be legal under this one.
//
// -----------------------------------------------------------------------------
// WHAT IS NOT HERE
//
// Anything that changes the shape of the firmware rather than its behaviour:
// the codec mode, the oversampling factor, the frames per packet, the pin map.
// Those size fixed buffers and static_asserts at compile time, and making them
// runtime-settable would mean sizing every buffer for the worst case and
// losing the compile-time checks that currently catch the mistake.
//
// See docs/03-config.md.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "log.h"   // LogLevel, for configLogLevel()

// Bump when the persisted layout changes, and add a migration for it.
//
// A stored blob with an OLDER version is read through its own frozen struct
// (config_v1.h and friends) and migrated forward field by field, so an upgrade
// keeps every setting including the key. A blob with a NEWER version cannot be
// read at all and falls back to defaults - there is no downgrade path and
// there should not be one.
//
// See docs/07-config-migration.md.
#define CONFIG_VERSION 2

// Largest blob any version has ever been. The migration chain works in buffers
// of this size, so it has to cover the biggest historical layout as well as
// the current one. Raise it before adding a version that would exceed it.
#define CONFIG_MAX_BLOB 128

#define CONFIG_MAGIC   0x47464356UL   // 'VCFG' little-endian

// -----------------------------------------------------------------------------
// The persisted blob.
//
// Field order is chosen for alignment, not for readability: the fixed header
// first, then the 16-byte key, then the scalars grouped by width so the
// compiler inserts no padding it would then have to CRC. Anything added later
// goes immediately before `crc` and bumps CONFIG_VERSION.
// -----------------------------------------------------------------------------
struct VoiceConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t size;        // sizeof(VoiceConfig) as written; catches a layout change
  uint32_t firmware;    // versionNumeric() of the build that wrote it

  // AES-128 key. `keySet` distinguishes "deliberately configured" from "all
  // zeros because nobody has set one" - the key bytes alone cannot, and an
  // all-zero key is a published key.
  uint8_t  key[16];
  uint8_t  keySet;

  uint8_t  stationId;     // 0 = derive from the factory MAC
  uint8_t  preset;        // preset index to come up on
  uint8_t  encryptOnBoot; // arm encryption at boot
  uint8_t  micGainShift;  // left shift applied to microphone samples
  uint8_t  cueTones;      // roger beep and friends

  // Minimum severity that reaches the event log, stored as LogLevel + 1.
  //
  // The offset is what lets this field be added WITHOUT bumping
  // CONFIG_VERSION, and that is worth more than it looks. It was claimed from
  // the reserved padding below, so the struct size and every existing field
  // offset are unchanged - which means a blob written by a build that predates
  // this field still loads, still passes its CRC, and keeps its key.
  //
  // Reserved bytes are memset to zero by fillDefaults(), so an old blob reads
  // 0 here. Encoding zero as "not set, use the default" rather than as
  // "DEBUG" is the whole trick: without the offset, every previously
  // configured board would silently come up logging everything.
  //
  // This is what reserved padding is FOR. The next field to be added has one
  // byte left and then the version has to move - see docs/07-config-migration.
  uint8_t  logLevel;

  // NEW IN v2. How the boot preset is chosen:
  //
  //   0  fixed  - always come up on `preset`, whatever was in use last
  //   1  last   - `preset` is updated automatically whenever the operator
  //               changes preset, so the handset comes back where it was
  //
  // This took the last byte of v1's reserved padding, which is why the struct
  // grew - see `reserved` below.
  uint8_t  presetMode;

  uint16_t prerollMs;     // receive jitter buffer before playback starts
  uint16_t txMaxSeconds;  // hard limit on one transmission; 0 = no limit

  // Reserve for the next few single-byte settings.
  //
  // v1 shipped with two padding bytes; logLevel took one and presetMode took
  // the other, at which point the NEXT field would have forced a version bump
  // regardless. Adding four here means the next three or four settings are
  // free, and it is why v2 is 48 bytes rather than 44.
  //
  // Zeroed by fillDefaults(), so a future field that reads 0 as "not set" can
  // still be added without a bump - the trick logLevel uses; see its comment.
  uint8_t  reserved[4];

  uint32_t crc;           // CRC-32 over every byte before this one.
                          // MUST remain the LAST field: the CRC check for an
                          // older blob finds it at (size - 4) without knowing
                          // that version's layout at all.
};

// Why the running configuration is what it is. Reported by `config` and
// recorded in the log, because "it forgot my settings" and "it never had any"
// are different problems.
enum ConfigLoadResult {
  CFG_LOAD_OK = 0,        // read from NVS and valid
  CFG_LOAD_MIGRATED,      // read from an older version and brought forward
  CFG_LOAD_ABSENT,        // nothing stored yet - first boot on this board
  CFG_LOAD_BAD_MAGIC,     // stored bytes are not ours
  CFG_LOAD_BAD_CRC,       // stored bytes are corrupt
  CFG_LOAD_BAD_SIZE,      // right magic, wrong struct size
  CFG_LOAD_VERSION,       // right magic, a version we cannot read
  CFG_LOAD_OUT_OF_RANGE,  // loaded, but a field failed validation
};

// Field identifiers, used by `config set` and recorded in the log so a dump
// says which setting was changed.
enum ConfigFieldId {
  CFG_F_KEY = 0,
  CFG_F_STATION,
  CFG_F_PRESET,
  CFG_F_ENCRYPT,
  CFG_F_MICGAIN,
  CFG_F_CUES,
  CFG_F_PREROLL,
  CFG_F_TXMAX,
  // Appended, never inserted: the numeric value goes into every CFG_SET log
  // record, and renumbering would make old dumps name the wrong setting.
  CFG_F_LOGLEVEL,
  CFG_F_PRESETMODE,
  CFG_F_COUNT,
};

// Loads from NVS, validates, and falls back to build-time defaults on any
// problem. Never fails: a handset with unreadable settings still has to be a
// handset. Call it early - crypto, radio and audio all read from it.
void configBegin();

// The stored log threshold, decoded. Falls back to INFO for an unset or
// implausible byte rather than to silence - a corrupt value should not be able
// to switch the log off.
LogLevel configLogLevel();

ConfigLoadResult configLoadResult();
const char*      configLoadResultName(ConfigLoadResult r);

// The live configuration. Read freely; write only through configSet*.
const VoiceConfig& config();

// Resets every field to its build-time default and persists that.
bool configReset();

// Sets one field by name, validating it against the field table, and persists
// immediately. `err` receives a human-readable reason on failure; pass null if
// you do not want one.
//
// Equivalent to a batch of one, and implemented as exactly that.
bool configSetByName(const char* name, const char* value, const char** err);

// -----------------------------------------------------------------------------
// BATCHED WRITES - several fields, one flash write, all or nothing.
//
// This is the important one, and not only for convenience.
//
// Setting eight fields one at a time is eight NVS writes, and every one of
// them is a window in which power can be lost. It is also eight chances to
// end up half-configured: a typo in the fifth value leaves the first four
// applied and the rest not, which is a state nobody asked for and nobody can
// see without checking every field.
//
// A batch stages every change against a private copy, validates ALL of them,
// and only then writes once. A rejection anywhere leaves the stored config
// exactly as it was - not partly updated. That collapses N windows into one
// and makes the operation atomic from the operator's point of view.
//
//   configBatchBegin();
//   if (!configBatchStage("station", "7",  &err)) { configBatchAbort(); ... }
//   if (!configBatchStage("preset",  "5",  &err)) { configBatchAbort(); ... }
//   configBatchCommit();     // one CRC, one NVS write
//
// It does not close the window entirely - one write is still one write. See
// docs/08-config-integrity.md for what would, and why measuring comes first.
// -----------------------------------------------------------------------------
void configBatchBegin();
bool configBatchStage(const char* name, const char* value, const char** err);
bool configBatchCommit();
void configBatchAbort();
bool configBatchActive();

// Fields changed by the batch in progress, as a bitmask of (1 << ConfigFieldId).
// The console uses it to push exactly the live settings that actually moved.
uint32_t configBatchChanged();

// Formats one field's current value into `out`. False if the name is unknown.
bool configGetByName(const char* name, char* out, size_t outLen);

// Iteration, for `config` with no arguments.
uint8_t     configFieldCount();
const char* configFieldName(uint8_t index);
const char* configFieldHelp(uint8_t index);
const char* configFieldRange(uint8_t index);
bool        configFieldIsLive(uint8_t index);   // false = takes effect on restart
bool        configFieldValue(uint8_t index, char* out, size_t outLen);
bool        configFieldIsDefault(uint8_t index);

// True once a field marked restart-only has been changed since boot, so the
// console and the display can say "reboot to apply" rather than leaving
// somebody wondering why nothing happened.
bool configRestartPending();

// The version the stored blob was written with, before any migration. Equal
// to CONFIG_VERSION unless configLoadResult() is CFG_LOAD_MIGRATED.
uint16_t configStoredVersion();

// ---------------------------------------------------------------------------
// "Remember the last preset used"
//
// Called whenever the operator changes preset. Does nothing unless
// presetMode is `last`; otherwise it notes the new preset and SCHEDULES a
// save.
//
// Schedules, rather than performs. Cycling through eight presets to reach the
// one you want would otherwise be eight flash writes in as many seconds, all
// but the last of them recording a preset nobody stopped on.
// Synthesises a v1 blob, migrates it, and checks every field survived. Runs on
// the device, against the real struct layouts this compiler produced - which is
// the only place the interesting failure can be observed. `detail` names the
// first field that did not make it. See `config test`.
bool configTestMigration(const char** detail);

void configNotePreset(uint8_t preset);

// Call periodically from a low-priority context - loop(), next to
// logService(). Commits a deferred preset save once the choice has settled.
void configService();
