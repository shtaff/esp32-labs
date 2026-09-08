#include "configstore.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "config_v1.h"
#include "log.h"
#include "version.h"

// The persisted layout, pinned.
//
// `size` is stored in the blob and checked on load, so a layout change is
// already detected at runtime - but detected means "your settings are gone".
// This catches it at compile time instead, which is the only point at which it
// can still be reconsidered.
//
// If this fires, you changed the struct. Either put the field back in reserved
// padding (see logLevel in configstore.h for how, and why it is worth the
// contortion), or bump CONFIG_VERSION and read docs/07-config-migration.md
// before shipping it to a board whose key you cannot recreate.
static_assert(sizeof(VoiceConfig) == 48,
              "VoiceConfig layout changed - bump CONFIG_VERSION and add a "
              "migration; see docs/07-config-migration.md");
static_assert(sizeof(VoiceConfig) <= CONFIG_MAX_BLOB,
              "VoiceConfig no longer fits the migration buffers");

// NVS namespace and key. Both short: NVS keys are limited to 15 characters,
// and a truncated key that silently becomes a different key is a bad day.
static const char* NVS_NAMESPACE = "voice";
static const char* NVS_KEY       = "cfg";

static VoiceConfig live;
static ConfigLoadResult loadResult = CFG_LOAD_ABSENT;
static bool restartPending = false;

// Batch staging. Changes are validated into `staging` and only copied over
// `live` - and written to flash - if every one of them passed.
static VoiceConfig staging;
static bool     batching = false;
static uint32_t batchChanged = 0;
static bool     batchNeedsRestart = false;

// -----------------------------------------------------------------------------
// CRC-32 (IEEE, reflected), computed bitwise.
//
// No lookup table on purpose: the blob is forty-odd bytes and this runs a
// handful of times per boot, so 256 words of flash for a table that saves
// microseconds nobody is waiting for is a bad trade.
// -----------------------------------------------------------------------------
static uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return ~crc;
}

// The CRC covers everything up to but not including the crc field itself.
static uint32_t blobCrc(const VoiceConfig& c) {
  return crc32((const uint8_t*)&c, offsetof(VoiceConfig, crc));
}

// The same check for a blob of ANY version.
//
// This works without knowing that version's layout because `crc` is required
// to be the last field in every version - so it lives at (size - 4) whatever
// the rest of the struct looks like. That requirement is the only thing making
// a stored v1 blob verifiable by a build that has moved on to v2, which is why
// it is stated as a rule on the field itself in configstore.h.
static bool blobCrcOk(const void* p, size_t size) {
  if (size < 16 || size > CONFIG_MAX_BLOB) return false;
  uint32_t stored;
  memcpy(&stored, (const uint8_t*)p + size - 4, sizeof(stored));
  return crc32((const uint8_t*)p, size - 4) == stored;
}

// =============================================================================
// The field table.
//
// One row per setting: how to parse it, what it is allowed to be, and whether
// changing it does anything before the next restart. Everything the console
// does - list, get, set, validate, report - is driven from here, so adding a
// setting is one row plus a default, not five scattered edits.
// =============================================================================
// CFG_LEVEL is its own type rather than a bare number because "warn" is a far
// better thing to type - and to read back in `config` - than "2". The stored
// byte is LogLevel + 1; see the logLevel field in configstore.h for why the
// offset exists and why it must not be removed.
// CFG_LEVEL and CFG_ENUM both take words rather than numbers, because a number
// whose meaning depends on an enum the operator cannot see is a number they
// will eventually get backwards.
//
// CFG_ENUM is the general one: its legal values ARE the pipe-separated `range`
// string, and the stored byte is the index into it. One source of truth for
// what is allowed, what it is called, and what gets written.
//
// CFG_LEVEL exists separately only because its stored byte is the index PLUS
// ONE - the offset that lets zero mean "not set" and keeps v1 blobs readable.
// See the logLevel field in configstore.h.
enum CfgType { CFG_U8, CFG_U16, CFG_BOOL, CFG_KEY, CFG_LEVEL, CFG_ENUM };

// Copies the n'th pipe-separated name out of `list` into `out`. False if there
// are fewer than n+1 of them.
static bool enumName(const char* list, uint8_t n, char* out, size_t outLen) {
  const char* p = list;
  for (uint8_t i = 0; i < n; i++) {
    p = strchr(p, '|');
    if (!p) return false;
    p++;
  }
  const char* end = strchr(p, '|');
  const size_t len = end ? (size_t)(end - p) : strlen(p);
  if (len == 0 || len >= outLen) return false;
  memcpy(out, p, len);
  out[len] = 0;
  return true;
}

// The index of `value` in a pipe-separated list, or -1.
static int enumIndex(const char* list, const char* value) {
  char name[16];
  for (uint8_t i = 0; i < 16; i++) {
    if (!enumName(list, i, name, sizeof(name))) return -1;
    if (strcmp(name, value) == 0) return (int)i;
  }
  return -1;
}

static const char* const LEVEL_NAMES[] = { "debug", "info", "warn", "error" };

struct CfgField {
  const char* name;
  CfgType     type;
  uint16_t    offset;   // into VoiceConfig
  uint32_t    min;
  uint32_t    max;
  bool        live;     // false = only takes effect at the next boot
  const char* range;    // for display; the numbers above are the truth
  const char* help;
};

static const CfgField FIELDS[CFG_F_COUNT] = {
  { "key",     CFG_KEY,  offsetof(VoiceConfig, key),           0, 0,    false,
    "32 hex",  "AES-128 key; both handsets must match" },
  { "station", CFG_U8,   offsetof(VoiceConfig, stationId),     0, 254,  false,
    "0-254",   "station id in every packet; 0 = derive from MAC" },
  { "preset",  CFG_U8,   offsetof(VoiceConfig, preset),        0, VOICE_PRESET_COUNT - 1, false,
    "0-7",     "preset to come up on after a restart" },
  { "encrypt", CFG_BOOL, offsetof(VoiceConfig, encryptOnBoot), 0, 1,    false,
    "on/off",  "arm encryption at boot" },
  { "micgain", CFG_U8,   offsetof(VoiceConfig, micGainShift),  0, 6,    true,
    "0-6",     "microphone gain as a left shift; each step doubles it" },
  { "cues",    CFG_BOOL, offsetof(VoiceConfig, cueTones),      0, 1,    true,
    "on/off",  "cue tones at the edges of a transmission" },
  { "preroll", CFG_U16,  offsetof(VoiceConfig, prerollMs),     80, 1000, true,
    "80-1000", "receive buffer before playback starts, ms" },
  { "txmax",   CFG_U16,  offsetof(VoiceConfig, txMaxSeconds),  0, 300,  true,
    "0-300",   "hard limit on one transmission, s; 0 = no limit" },
  // Minimum 0, not 1: zero is the "not set" encoding that a blob written
  // before this field existed carries, and rejecting it at load would throw
  // away every other setting on those boards - including the key.
  { "loglevel", CFG_LEVEL, offsetof(VoiceConfig, logLevel),     0, 4,    true,
    "debug|info|warn|error", "minimum severity that reaches the event log" },
  // Added in config v2. Zero is "fixed", which is exactly how v1 behaved, so
  // the migration can leave it at its default and be correct by construction.
  { "presetmode", CFG_ENUM, offsetof(VoiceConfig, presetMode),  0, 1,    true,
    "fixed|last", "boot on `preset`, or on whichever preset was last in use" },
};

// -----------------------------------------------------------------------------
// Defaults come from config.h, so an unconfigured board behaves exactly like
// one built before any of this existed.
// -----------------------------------------------------------------------------
static void fillDefaults(VoiceConfig& c) {
  memset(&c, 0, sizeof(c));
  c.magic    = CONFIG_MAGIC;
  c.version  = CONFIG_VERSION;
  c.size     = sizeof(VoiceConfig);
  c.firmware = versionNumeric();

  // The build-time key, if secrets.ini supplied one. Parsed here rather than
  // in crypto.cpp so that there is exactly one path into the key bytes: the
  // config. crypto.cpp reads the config and nothing else.
  const char* hex = VOICE_KEY_HEX;
  uint8_t any = 0;
  if (strlen(hex) == 32) {
    for (int i = 0; i < 16; i++) {
      auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
      if (hi < 0 || lo < 0) { any = 0; break; }
      c.key[i] = (uint8_t)((hi << 4) | lo);
      any |= c.key[i];
    }
  }
  // All zero is the "not configured" sentinel from secrets.ini.example, and it
  // must not be mistaken for a deliberate key.
  c.keySet = any ? 1 : 0;

  c.stationId     = 0;                       // 0 = derive from MAC
  c.preset        = VOICE_DEFAULT_PRESET;
  c.encryptOnBoot =
#ifdef VOICE_ENCRYPT_ON_BOOT
      1;
#else
      0;
#endif
  c.micGainShift = VOICE_MIC_GAIN_SHIFT;
  c.cueTones     = (VOICE_CUE_TONES != 0) ? 1 : 0;
  c.prerollMs    = (uint16_t)VOICE_PREROLL_MS;
  c.txMaxSeconds = 0;
  c.logLevel     = LOG_LVL_INFO + 1;   // stored as level + 1; 0 means "not set"
  c.presetMode   = 0;                  // fixed: how every v1 board behaved

  c.crc = blobCrc(c);
}

// -----------------------------------------------------------------------------
// Validate every field against the table. Run on load as well as on set:
// flash rots, firmware gets downgraded, and a value that was legal under a
// previous build's table may not be legal under this one.
// -----------------------------------------------------------------------------
static bool validate(const VoiceConfig& c) {
  for (uint8_t i = 0; i < CFG_F_COUNT; i++) {
    const CfgField& f = FIELDS[i];
    uint32_t v = 0;
    switch (f.type) {
      case CFG_KEY: continue;   // any 16 bytes are a legal key
      case CFG_U8:
      case CFG_BOOL:
      case CFG_ENUM:
      case CFG_LEVEL: v = *((const uint8_t*)&c + f.offset); break;
      case CFG_U16: {
        uint16_t t;
        memcpy(&t, (const uint8_t*)&c + f.offset, sizeof(t));
        v = t;
        break;
      }
    }
    if (v < f.min || v > f.max) return false;
  }
  return true;
}

// =============================================================================
// MIGRATION
//
// A stored blob from an older version is read through ITS OWN frozen struct
// and carried forward field by field. Two rules make this safe, and both are
// easy to break by accident:
//
//   1. Each historical layout has its own frozen header (config_v1.h) with a
//      static_assert on its size. Reusing the CURRENT struct and guarding
//      fields with #ifdef defeats the entire exercise, because then the "old"
//      layout changes every time the new one does.
//
//   2. Fields are copied ONE AT A TIME, by name. Not memcpy of a common
//      prefix - that is exactly the shortcut that silently mis-assigns a field
//      the day somebody reorders the struct for alignment, and it would do so
//      without any compiler complaint.
//
// Migrations chain: v1 -> v2 -> v3 is v1 -> v2 followed by v2 -> v3, applied
// in sequence, working in a pair of buffers. There is no downgrade path.
//
// See docs/07-config-migration.md.
// =============================================================================
typedef bool (*MigrateFn)(const void* in, size_t inLen, void* out);

struct Migration {
  uint16_t  from;
  uint16_t  to;
  size_t    inSize;    // expected size of the input blob
  size_t    outSize;   // size this step produces
  MigrateFn fn;
};

static void fillDefaults(VoiceConfig& c);   // used by the migrations below

// -----------------------------------------------------------------------------
// v1 -> v2. Adds presetMode and a fresh reserve; 44 bytes becomes 48.
//
// Every v1 field is carried across explicitly. presetMode keeps its default of
// "fixed", which is not an arbitrary choice: v1 had no such concept and always
// booted on the stored preset, so "fixed" IS how a v1 board behaved. A
// migration that changed observable behaviour would be a worse outcome than
// one that lost the setting, because nobody would think to look for it.
// -----------------------------------------------------------------------------
static bool migrateV1toV2(const void* in, size_t inLen, void* out) {
  if (inLen != sizeof(VoiceConfigV1)) return false;

  const VoiceConfigV1* a = (const VoiceConfigV1*)in;
  VoiceConfig* b = (VoiceConfig*)out;

  // Start from current defaults, so anything v1 did not have is already sane.
  fillDefaults(*b);

  memcpy(b->key, a->key, sizeof(b->key));
  b->keySet        = a->keySet;
  b->stationId     = a->stationId;
  b->preset        = a->preset;
  b->encryptOnBoot = a->encryptOnBoot;
  b->micGainShift  = a->micGainShift;
  b->cueTones      = a->cueTones;
  b->logLevel      = a->logLevel;      // same encoding in both versions
  b->prerollMs     = a->prerollMs;
  b->txMaxSeconds  = a->txMaxSeconds;
  // b->presetMode stays at its default. See the note above.

  b->version  = 2;
  b->size     = sizeof(VoiceConfig);
  b->firmware = versionNumeric();
  b->crc      = blobCrc(*b);
  return true;
}

static const Migration MIGRATIONS[] = {
  { 1, 2, sizeof(VoiceConfigV1), sizeof(VoiceConfig), migrateV1toV2 },
};

static const Migration* findMigration(uint16_t from) {
  for (unsigned i = 0; i < sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]); i++) {
    if (MIGRATIONS[i].from == from) return &MIGRATIONS[i];
  }
  return nullptr;
}

// Walks the chain from `version` up to CONFIG_VERSION, in place in `buf`.
// False if any step is missing or refuses the input.
static bool migrateChain(uint8_t* buf, size_t* size, uint16_t version) {
  uint8_t scratch[CONFIG_MAX_BLOB];

  while (version < CONFIG_VERSION) {
    const Migration* m = findMigration(version);
    if (m == nullptr) {
      Serial.printf("[cfg] no migration from v%u - cannot upgrade\n",
                    (unsigned)version);
      logError(LOG_MOD_CONFIG, LOG_EV_CFG_MIGRATE_FAIL, (int32_t)version, 0);
      return false;
    }
    if (*size != m->inSize) {
      Serial.printf("[cfg] v%u blob is %u bytes, expected %u\n",
                    (unsigned)version, (unsigned)*size, (unsigned)m->inSize);
      logError(LOG_MOD_CONFIG, LOG_EV_CFG_MIGRATE_FAIL, (int32_t)version, -1);
      return false;
    }
    if (!m->fn(buf, *size, scratch)) {
      Serial.printf("[cfg] migration v%u -> v%u refused the input\n",
                    (unsigned)m->from, (unsigned)m->to);
      logError(LOG_MOD_CONFIG, LOG_EV_CFG_MIGRATE_FAIL, (int32_t)m->from,
               (int32_t)m->to);
      return false;
    }

    memcpy(buf, scratch, m->outSize);
    *size   = m->outSize;
    version = m->to;

    Serial.printf("[cfg] migrated v%u -> v%u\n", (unsigned)m->from,
                  (unsigned)m->to);
    logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_MIGRATE, (int32_t)m->from, (int32_t)m->to);
  }
  return true;
}

static bool persist() {
  live.magic    = CONFIG_MAGIC;
  live.version  = CONFIG_VERSION;
  live.size     = sizeof(VoiceConfig);
  live.firmware = versionNumeric();
  live.crc      = blobCrc(live);

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[cfg] NVS open failed - settings are RAM only this boot");
    return false;
  }
  const size_t written = prefs.putBytes(NVS_KEY, &live, sizeof(live));
  prefs.end();

  if (written != sizeof(live)) {
    Serial.printf("[cfg] NVS write short: %u of %u bytes\n",
                  (unsigned)written, (unsigned)sizeof(live));
    logError(LOG_MOD_CONFIG, LOG_EV_CFG_SAVE, (int32_t)written, -1);
    return false;
  }
  logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_SAVE, CONFIG_VERSION, (int32_t)written);
  return true;
}

// =============================================================================
// Migration self test.
//
// Synthesises a v1 blob with distinctive values, runs it through the real
// migration chain, and checks every field arrived.
//
// It runs ON THE DEVICE rather than on a host, and that is the point: what can
// go wrong here is a struct layout disagreeing with what the migration assumes,
// and the layout is whatever THIS compiler produced for THIS target. A host
// test with a different ABI would happily pass while the device silently
// mis-assigns a field.
//
// It is also how the migration can be exercised without owning a board that
// still has a v1 blob on it - which, a week after the version bump, is nobody.
// =============================================================================
bool configTestMigration(const char** detail) {
  static char why[64];
  if (detail) *detail = "";

  VoiceConfigV1 v1;
  memset(&v1, 0, sizeof(v1));
  v1.magic    = CONFIG_MAGIC;
  v1.version  = 1;
  v1.size     = sizeof(v1);
  v1.firmware = versionNumeric();

  // Distinctive values, all different from the defaults, so a field that is
  // silently taking its default instead of being carried across shows up.
  for (int i = 0; i < 16; i++) v1.key[i] = (uint8_t)(0xA0 + i);
  v1.keySet        = 1;
  v1.stationId     = 137;
  v1.preset        = 5;
  v1.encryptOnBoot = 1;
  v1.micGainShift  = 4;
  v1.cueTones      = 0;
  v1.logLevel      = LOG_LVL_WARN + 1;
  v1.prerollMs     = 480;
  v1.txMaxSeconds  = 90;
  v1.crc = crc32((const uint8_t*)&v1, sizeof(v1) - 4);

#define CFG_TEST_REQUIRE(cond, msg)                        \
  do { if (!(cond)) { snprintf(why, sizeof(why), msg);     \
                      if (detail) *detail = why;           \
                      return false; } } while (0)

  // The version-agnostic CRC check has to accept a v1 blob, or nothing else
  // below ever gets a chance to run on a real board.
  CFG_TEST_REQUIRE(blobCrcOk(&v1, sizeof(v1)), "v1 blob failed the CRC check");

  uint8_t buf[CONFIG_MAX_BLOB];
  memcpy(buf, &v1, sizeof(v1));
  size_t size = sizeof(v1);

  CFG_TEST_REQUIRE(migrateChain(buf, &size, 1), "migration chain refused v1");
  CFG_TEST_REQUIRE(size == sizeof(VoiceConfig), "migrated blob is the wrong size");

  VoiceConfig out;
  memcpy(&out, buf, sizeof(out));

  CFG_TEST_REQUIRE(out.version == CONFIG_VERSION, "version was not updated");
  CFG_TEST_REQUIRE(out.size == sizeof(VoiceConfig), "size field is wrong");
  CFG_TEST_REQUIRE(blobCrcOk(&out, sizeof(out)), "migrated blob CRC is wrong");

  CFG_TEST_REQUIRE(memcmp(out.key, v1.key, 16) == 0, "key was not carried across");
  CFG_TEST_REQUIRE(out.keySet == 1,          "keySet was lost");
  CFG_TEST_REQUIRE(out.stationId == 137,     "stationId was lost");
  CFG_TEST_REQUIRE(out.preset == 5,          "preset was lost");
  CFG_TEST_REQUIRE(out.encryptOnBoot == 1,   "encryptOnBoot was lost");
  CFG_TEST_REQUIRE(out.micGainShift == 4,    "micGainShift was lost");
  CFG_TEST_REQUIRE(out.cueTones == 0,        "cueTones was lost");
  CFG_TEST_REQUIRE(out.logLevel == LOG_LVL_WARN + 1, "logLevel was lost");
  CFG_TEST_REQUIRE(out.prerollMs == 480,     "prerollMs was lost");
  CFG_TEST_REQUIRE(out.txMaxSeconds == 90,   "txMaxSeconds was lost");

  // The new field must take its default, and that default must be the one that
  // reproduces v1 behaviour - "fixed", i.e. always boot on the stored preset.
  CFG_TEST_REQUIRE(out.presetMode == 0, "presetMode did not default to fixed");

  // And the result has to survive the same validation a real load applies.
  CFG_TEST_REQUIRE(validate(out), "migrated blob failed field validation");

#undef CFG_TEST_REQUIRE
  return true;
}

static uint16_t storedVersion = CONFIG_VERSION;

void configBegin() {
  Preferences prefs;
  uint8_t buf[CONFIG_MAX_BLOB];
  size_t  size = 0;
  bool    migrated = false;

  // Read-only open. If the namespace does not exist yet this still succeeds on
  // ESP32 for reading, and getBytesLength returns 0.
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    loadResult = CFG_LOAD_ABSENT;
  } else {
    size = prefs.getBytesLength(NVS_KEY);
    if (size == 0 || size > sizeof(buf)) {
      loadResult = (size == 0) ? CFG_LOAD_ABSENT : CFG_LOAD_BAD_SIZE;
      size = 0;
    } else {
      prefs.getBytes(NVS_KEY, buf, size);
    }
    prefs.end();
  }

  if (size > 0) {
    // Header fields are at the same offsets in every version - that is the one
    // thing a version scheme has to promise about itself, or there is no way
    // to find out which version you are holding.
    uint32_t magic;
    uint16_t ver, declaredSize;
    memcpy(&magic,        buf + 0, sizeof(magic));
    memcpy(&ver,          buf + 4, sizeof(ver));
    memcpy(&declaredSize, buf + 6, sizeof(declaredSize));

    if (magic != CONFIG_MAGIC) {
      loadResult = CFG_LOAD_BAD_MAGIC;
    } else if (declaredSize != size) {
      // The blob disagrees with NVS about its own length.
      loadResult = CFG_LOAD_BAD_SIZE;
    } else if (!blobCrcOk(buf, size)) {
      // Checked BEFORE migrating: there is no point carrying corrupt fields
      // forward into a shiny new layout.
      loadResult = CFG_LOAD_BAD_CRC;
    } else if (ver > CONFIG_VERSION) {
      // Written by a newer firmware. There is no downgrade path and there
      // should not be one - we would be guessing at fields we have never seen.
      loadResult = CFG_LOAD_VERSION;
    } else {
      storedVersion = ver;
      if (ver < CONFIG_VERSION) {
        migrated = migrateChain(buf, &size, ver);
        if (!migrated) {
          loadResult = CFG_LOAD_VERSION;
        }
      }

      if (ver == CONFIG_VERSION || migrated) {
        if (size != sizeof(VoiceConfig)) {
          loadResult = CFG_LOAD_BAD_SIZE;
        } else {
          VoiceConfig candidate;
          memcpy(&candidate, buf, sizeof(candidate));
          // Validated AFTER migration as well as before storage: a value that
          // was legal under the old version's table may not be legal under
          // this one.
          if (!validate(candidate)) {
            loadResult = CFG_LOAD_OUT_OF_RANGE;
          } else {
            live = candidate;
            loadResult = migrated ? CFG_LOAD_MIGRATED : CFG_LOAD_OK;
          }
        }
      }
    }
  }

  if (loadResult == CFG_LOAD_OK || loadResult == CFG_LOAD_MIGRATED) {
    Serial.printf("[cfg] loaded v%u (%u bytes), written by firmware %lu.%lu.%lu\n",
                  (unsigned)live.version, (unsigned)live.size,
                  (unsigned long)(live.firmware >> 16),
                  (unsigned long)((live.firmware >> 8) & 0xFF),
                  (unsigned long)(live.firmware & 0xFF));
    logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_LOAD, (int32_t)live.version,
            (int32_t)live.size);

    if (migrated) {
      // Write the migrated blob back immediately, so the migration happens
      // exactly once rather than on every boot for the rest of the board's
      // life - and so a later downgrade fails loudly rather than silently
      // reading a v2 blob as v1.
      if (persist()) {
        Serial.printf("[cfg] migrated settings saved as v%u\n",
                      (unsigned)CONFIG_VERSION);
      } else {
        Serial.println("[cfg] migrated settings could NOT be saved - will migrate again next boot");
      }
    }
  } else {
    // Any problem at all means build-time defaults. A handset with unreadable
    // settings still has to be a handset - refusing to boot over a bad
    // checksum would be strictly worse than starting from known values.
    fillDefaults(live);
    storedVersion = CONFIG_VERSION;
    Serial.printf("[cfg] using defaults: %s\n", configLoadResultName(loadResult));
    if (loadResult == CFG_LOAD_ABSENT) {
      logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_DEFAULTS, (int32_t)loadResult);
    } else {
      // Anything other than "absent" means something was there and was wrong,
      // which is worth a warning rather than an info.
      logWarn(LOG_MOD_CONFIG, LOG_EV_CFG_BAD, (int32_t)loadResult);
    }
  }

  // Push the log threshold now. The records written above were emitted before
  // this ran and are deliberately unaffected: a board should be able to say
  // why its configuration did not load, whatever that configuration says about
  // logging. Everything after this point obeys the setting.
  logSetMinLevel(configLogLevel());
}

uint16_t configStoredVersion() { return storedVersion; }

// Decoded from the stored byte, which is LogLevel + 1 with zero meaning "not
// set". Anything unexpected falls back to INFO rather than to silence: a
// corrupt byte should not be able to switch the log off.
LogLevel configLogLevel() {
  const uint8_t stored = live.logLevel;
  if (stored == 0 || stored > LOG_LVL_ERROR + 1) return LOG_LVL_INFO;
  return (LogLevel)(stored - 1);
}

ConfigLoadResult configLoadResult() { return loadResult; }

const char* configLoadResultName(ConfigLoadResult r) {
  switch (r) {
    case CFG_LOAD_OK:           return "loaded from NVS";
    case CFG_LOAD_MIGRATED:     return "migrated from an older version";
    case CFG_LOAD_ABSENT:       return "nothing stored yet (first boot)";
    case CFG_LOAD_BAD_MAGIC:    return "stored bytes are not ours";
    case CFG_LOAD_BAD_CRC:      return "stored bytes failed CRC - corrupt";
    case CFG_LOAD_BAD_SIZE:     return "stored size does not match this build";
    case CFG_LOAD_VERSION:      return "stored version cannot be read by this build";
    case CFG_LOAD_OUT_OF_RANGE: return "a stored field is outside its allowed range";
    default:                    return "?";
  }
}

const VoiceConfig& config()   { return live; }
bool configRestartPending()   { return restartPending; }
uint8_t configFieldCount()    { return CFG_F_COUNT; }

const char* configFieldName(uint8_t i)  { return i < CFG_F_COUNT ? FIELDS[i].name  : ""; }
const char* configFieldHelp(uint8_t i)  { return i < CFG_F_COUNT ? FIELDS[i].help  : ""; }
const char* configFieldRange(uint8_t i) { return i < CFG_F_COUNT ? FIELDS[i].range : ""; }
bool configFieldIsLive(uint8_t i)       { return i < CFG_F_COUNT ? FIELDS[i].live  : false; }

// -----------------------------------------------------------------------------
// Formatting a field for display.
//
// The key is NEVER printed. It is shown as "set" or "not set" and identified
// by the fingerprint on the SYS screen. Being able to write a key over a
// serial line is the point of storing it here; being able to read it back
// would make every terminal scrollback a copy of it, and that is a bad habit
// to build into a tool even when the tool is a lab toy.
// -----------------------------------------------------------------------------
static bool formatField(const VoiceConfig& c, uint8_t i, char* out, size_t n) {
  if (i >= CFG_F_COUNT) return false;
  const CfgField& f = FIELDS[i];
  const uint8_t* base = (const uint8_t*)&c;

  switch (f.type) {
    case CFG_KEY:
      snprintf(out, n, "%s", c.keySet ? "<set>" : "<not set>");
      return true;
    case CFG_BOOL:
      snprintf(out, n, "%s", base[f.offset] ? "on" : "off");
      return true;
    case CFG_U8:
      snprintf(out, n, "%u", (unsigned)base[f.offset]);
      return true;
    case CFG_ENUM:
      if (!enumName(f.range, base[f.offset], out, n)) {
        snprintf(out, n, "?%u", (unsigned)base[f.offset]);
      }
      return true;
    case CFG_LEVEL: {
      // Zero means the field predates this build, so report the default it is
      // actually behaving as rather than an index nobody can interpret.
      const uint8_t stored = base[f.offset];
      const uint8_t lvl = stored ? (uint8_t)(stored - 1) : (uint8_t)LOG_LVL_INFO;
      snprintf(out, n, "%s%s", LEVEL_NAMES[lvl & 3], stored ? "" : " (default)");
      return true;
    }
    case CFG_U16: {
      uint16_t v;
      memcpy(&v, base + f.offset, sizeof(v));
      snprintf(out, n, "%u", (unsigned)v);
      return true;
    }
  }
  return false;
}

bool configFieldValue(uint8_t i, char* out, size_t n) {
  return formatField(live, i, out, n);
}

bool configFieldIsDefault(uint8_t i) {
  if (i >= CFG_F_COUNT) return false;
  VoiceConfig d;
  fillDefaults(d);
  const CfgField& f = FIELDS[i];
  if (f.type == CFG_KEY) {
    return live.keySet == d.keySet &&
           memcmp(live.key, d.key, sizeof(d.key)) == 0;
  }
  const size_t width = (f.type == CFG_U16) ? 2 : 1;
  return memcmp((const uint8_t*)&live + f.offset,
                (const uint8_t*)&d + f.offset, width) == 0;
}

bool configGetByName(const char* name, char* out, size_t outLen) {
  for (uint8_t i = 0; i < CFG_F_COUNT; i++) {
    if (strcmp(name, FIELDS[i].name) == 0) return formatField(live, i, out, outLen);
  }
  return false;
}

// -----------------------------------------------------------------------------
// Parsing and validating a new value.
//
// Every rejection carries a reason. "invalid" on its own sends somebody to
// read the source; "must be 0-6" does not.
// -----------------------------------------------------------------------------
static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// -----------------------------------------------------------------------------
// Stage one field into `target`. Validates, but writes nothing to flash.
//
// This is the whole of the set logic; both the single and the batch entry
// points go through it, so there is exactly one place where a value is parsed
// and range-checked.
// -----------------------------------------------------------------------------
static bool stageField(VoiceConfig& target, const char* name, const char* value,
                       uint8_t* fieldIndex, const char** err) {
  static char reason[80];
  if (err) *err = "";

  for (uint8_t i = 0; i < CFG_F_COUNT; i++) {
    const CfgField& f = FIELDS[i];
    if (strcmp(name, f.name) != 0) continue;
    if (fieldIndex) *fieldIndex = i;

    uint8_t* base = (uint8_t*)&target;

    if (f.type == CFG_KEY) {
      if (strlen(value) != 32) {
        snprintf(reason, sizeof(reason), "key must be exactly 32 hex characters");
        if (err) *err = reason;
        return false;
      }
      uint8_t k[16];
      uint8_t any = 0;
      for (int b = 0; b < 16; b++) {
        const int hi = hexNibble(value[b * 2]);
        const int lo = hexNibble(value[b * 2 + 1]);
        if (hi < 0 || lo < 0) {
          snprintf(reason, sizeof(reason),
                   "key is not hex at character %d", b * 2);
          if (err) *err = reason;
          return false;
        }
        k[b] = (uint8_t)((hi << 4) | lo);
        any |= k[b];
      }
      if (!any) {
        // Refusing this is the same rule crypto.cpp applies: an all-zero AES
        // key is a published key, and storing one would let the handset show
        // ENC while protecting nothing.
        snprintf(reason, sizeof(reason),
                 "an all-zero key is not a key - use `openssl rand -hex 16`");
        if (err) *err = reason;
        return false;
      }
      memcpy(target.key, k, sizeof(k));
      target.keySet = 1;
      return true;
    }

    if (f.type == CFG_ENUM) {
      const int idx = enumIndex(f.range, value);
      if (idx < 0) {
        snprintf(reason, sizeof(reason), "%s must be one of %s", f.name, f.range);
        if (err) *err = reason;
        return false;
      }
      ((uint8_t*)&target)[f.offset] = (uint8_t)idx;
      return true;
    }

    if (f.type == CFG_LEVEL) {
      // Names only. A number here would be a number whose meaning depends on
      // an enum the operator cannot see, and getting it backwards - setting 0
      // expecting "silent" and getting "log everything" - is exactly the
      // mistake worth designing out.
      for (uint8_t l = 0; l < 4; l++) {
        if (strcmp(value, LEVEL_NAMES[l]) == 0) {
          ((uint8_t*)&target)[f.offset] = (uint8_t)(l + 1);
          return true;
        }
      }
      snprintf(reason, sizeof(reason), "loglevel must be one of %s", f.range);
      if (err) *err = reason;
      return false;
    }

    // Numeric, with on/off accepted for the booleans because typing "1" for a
    // thing the display calls "on" is a small daily irritation.
    uint32_t v;
    if (f.type == CFG_BOOL &&
        (strcmp(value, "on") == 0 || strcmp(value, "true") == 0)) {
      v = 1;
    } else if (f.type == CFG_BOOL &&
               (strcmp(value, "off") == 0 || strcmp(value, "false") == 0)) {
      v = 0;
    } else {
      char* end = nullptr;
      const long parsed = strtol(value, &end, 10);
      if (end == value || *end != '\0' || parsed < 0) {
        snprintf(reason, sizeof(reason), "'%s' is not a number", value);
        if (err) *err = reason;
        return false;
      }
      v = (uint32_t)parsed;
    }

    if (v < f.min || v > f.max) {
      snprintf(reason, sizeof(reason), "%s must be %s", f.name, f.range);
      if (err) *err = reason;
      return false;
    }

    if (f.type == CFG_U16) {
      const uint16_t t = (uint16_t)v;
      memcpy(base + f.offset, &t, sizeof(t));
    } else {
      base[f.offset] = (uint8_t)v;
    }
    return true;
  }

  snprintf(reason, sizeof(reason), "no setting called '%s' - try `config`", name);
  if (err) *err = reason;
  return false;
}

void configBatchBegin() {
  staging = live;
  batching = true;
  batchChanged = 0;
  batchNeedsRestart = false;
}

bool configBatchActive()     { return batching; }
uint32_t configBatchChanged(){ return batchChanged; }

void configBatchAbort() {
  batching = false;
  batchChanged = 0;
  batchNeedsRestart = false;
}

bool configBatchStage(const char* name, const char* value, const char** err) {
  if (!batching) configBatchBegin();

  uint8_t idx = 0;
  if (!stageField(staging, name, value, &idx, err)) return false;

  batchChanged |= (1UL << idx);
  if (!FIELDS[idx].live) batchNeedsRestart = true;
  return true;
}

bool configBatchCommit() {
  if (!batching) return false;

  // Belt and braces: every value was range-checked as it was staged, but run
  // the whole-blob validator too. It is the same function that guards a load,
  // and having exactly one definition of "valid" is worth the microseconds.
  if (!validate(staging)) {
    configBatchAbort();
    return false;
  }

  live = staging;
  batching = false;

  const bool ok = persist();
  if (ok) {
    // One log record per field actually changed. The key logs a zero rather
    // than its value: not printing it is undone if it goes in the log instead.
    for (uint8_t i = 0; i < CFG_F_COUNT; i++) {
      if (!(batchChanged & (1UL << i))) continue;
      const int32_t v = (FIELDS[i].type == CFG_KEY)
                      ? 0 : (int32_t)((const uint8_t*)&live)[FIELDS[i].offset];
      logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_SET, (int32_t)i, v);
    }
    if (batchNeedsRestart) restartPending = true;
  }
  batchChanged = 0;
  batchNeedsRestart = false;
  return ok;
}

// A batch of one. Kept so callers that only ever change a single setting do
// not have to spell out the three-call dance.
bool configSetByName(const char* name, const char* value, const char** err) {
  configBatchBegin();
  if (!configBatchStage(name, value, err)) {
    configBatchAbort();
    return false;
  }
  if (!configBatchCommit()) {
    static char reason[64];
    snprintf(reason, sizeof(reason), "value accepted but the NVS write failed");
    if (err) *err = reason;
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// "Remember the last preset used".
//
// The save is DEFERRED. Cycling through eight presets to reach the one you
// want would otherwise be eight flash writes in as many seconds, seven of them
// recording a preset nobody stopped on. Waiting for the choice to settle costs
// nothing and turns that into one write.
// -----------------------------------------------------------------------------
static uint8_t  pendingPreset = 0;
static uint32_t pendingSince = 0;
static bool     presetSavePending = false;

// How long a preset has to stay selected before it is worth a flash write.
#define CONFIG_PRESET_SETTLE_MS 5000UL

void configNotePreset(uint8_t preset) {
  if (live.presetMode == 0) return;          // fixed: the boot preset is manual
  if (preset >= VOICE_PRESET_COUNT) return;
  if (preset == live.preset && !presetSavePending) return;

  pendingPreset = preset;
  pendingSince = millis();
  presetSavePending = true;
}

void configService() {
  if (!presetSavePending) return;
  if ((millis() - pendingSince) < CONFIG_PRESET_SETTLE_MS) return;

  presetSavePending = false;
  if (pendingPreset == live.preset) return;  // settled back where it started

  live.preset = pendingPreset;
  if (persist()) {
    Serial.printf("[cfg] boot preset remembered as %u\n", (unsigned)pendingPreset);
    logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_SET, (int32_t)CFG_F_PRESET,
            (int32_t)pendingPreset);
  }
}

bool configReset() {
  fillDefaults(live);
  const bool ok = persist();
  logInfo(LOG_MOD_CONFIG, LOG_EV_CFG_RESET, ok ? 1 : 0);
  // Everything just changed, including the restart-only fields.
  restartPending = true;
  return ok;
}
