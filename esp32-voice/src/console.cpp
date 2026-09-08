// =============================================================================
// console.cpp - the serial console, run from the Arduino loop task.
//
// Every button action is reachable from here. That is not a convenience: it is
// what lets the firmware be brought up, and the radio link proved, on a board
// that has nothing soldered to it yet - which is exactly the state a new lab
// board is in, and exactly when you most want to know whether the radio works.
//
// It runs at the lowest priority in the system and never touches the radio,
// the codec or the I2S peripheral directly. `ptt` injects a button event and
// lets the same state machine handle it; `ch` and `enc` go through the same
// entry points the buttons use.
// =============================================================================
#include "console.h"

#include <Arduino.h>
#include <string.h>

#include "app.h"
#include "audio.h"
#include "buttons.h"
#include "codec.h"
#include "config.h"
#include "post.h"
#include "configstore.h"
#include "crypto.h"
#include "link.h"
#include "log.h"
#include "version.h"
#include "ui.h"

static char line[64];
static uint8_t fill = 0;

// A scripted PTT press has to release itself. Holding the loop task in a
// delay() would work and would also stop the console answering for the
// duration, which is the one time you might want to type something.
static uint32_t pttReleaseAt = 0;

static void printHelp() {
  Serial.println();
  Serial.println("  help              this list");
  Serial.println("  stat              everything the display screens show");
  Serial.println("  version           firmware version and git provenance");
  Serial.println("  log               event log from RTC (survives a reset)");
  Serial.println("    log flash       from flash (survives power off)");
  Serial.println("    log raw|flash raw   hex, for offline decoding");
  Serial.println("    log flush       write pending records to flash now");
  Serial.println("    log clear|erase wipe the RTC ring | the flash history");
  Serial.println("  config            show settings; get/set/reset");
  Serial.println("                    set many at once, in ONE write:");
  Serial.println("                      config set station=7 preset=5 cues=off");
  Serial.println("    config test     verify the v1 -> v2 migration on this board");
  Serial.println("  post              re-run the power-on self test");
  Serial.println("  presets           list the preset table");
  Serial.println("  preset [0-7]      show or set the preset (alias: ch)");
  Serial.println("  enc [on|off]      show or set encryption");
  Serial.println("  screen [0-4]      show or set the display screen");
  Serial.println("  ptt [ms]          key up for ms (default 3000), then release");
  Serial.println("  tone [on|off]     force the synthetic test signal");
  Serial.println("  beep [on|off]     cue tones at the edges of a transmission");
  Serial.println("  reboot            restart");
  Serial.println();
  Serial.println("  On the self-test preset, `ptt` records and plays back locally");
  Serial.println("  instead of transmitting - the radio stays in standby.");
  Serial.println();
}

// -----------------------------------------------------------------------------
// The event log, decoded for a human.
//
// Oldest first, because you read a log forwards. The timestamp restarts at
// every boot, and because the ring survives a reset there can be several boots
// in one dump - the BOOT records are the boundaries, and their `b` field is
// the boot number.
// -----------------------------------------------------------------------------
static void printLog() {
  const uint16_t n = logCount();

  Serial.println();
  // The threshold first: a log that looks emptier than expected is nearly
  // always a threshold somebody raised and forgot, and that answer should be
  // on the same screen as the symptom.
  Serial.printf("log       minimum level %s (records below it were never stored)\n",
                logLevelName(logMinLevel()));
  Serial.printf("          %u records held, %lu written since clear, boot #%lu%s\n",
                (unsigned)n, (unsigned long)logTotalWritten(),
                (unsigned long)logBootCount(),
                logSurvivedReset() ? ", ring survived the last reset"
                                   : ", ring was initialised this boot");
  if (logTotalWritten() > n) {
    Serial.printf("          %lu older records were overwritten by the ring\n",
                  (unsigned long)(logTotalWritten() - n));
  }
  Serial.println();
  Serial.println("     time  L  module  event             a           b");
  Serial.println("  -------  -  ------  ----------------  ----------  ----------");

  LogRecord r;
  for (uint16_t i = 0; i < n; i++) {
    if (!logRead(i, &r)) break;

    // An event this build does not know about prints as its number rather
    // than being dropped - a dump from a newer firmware is still readable.
    const char* name = logEventName(r.code);
    char label[20];
    if (name[0] == '\0') snprintf(label, sizeof(label), "event %u", (unsigned)r.code);
    else                 snprintf(label, sizeof(label), "%s", name);

    Serial.printf("  %4lu.%03lu  %s  %-6s  %-16s  %10ld  %10ld\n",
                  (unsigned long)(r.ms / 1000UL), (unsigned long)(r.ms % 1000UL),
                  logLevelName(r.level), logModuleName(r.module), label,
                  (long)r.a, (long)r.b);
  }
  Serial.println();
}

// -----------------------------------------------------------------------------
// The same records as hex, for archiving or offline decoding.
//
// One line per record, 32 hex characters, little-endian exactly as stored -
// so a decoder can consume it with a struct unpack and nothing else. The
// header names the format version and the record count so the dump is
// self-describing: a file recovered from a terminal scrollback months later
// should not need to be matched up with the firmware that produced it.
//
// See docs/02-logging.md for a decoder.
// -----------------------------------------------------------------------------
static void printLogRaw() {
  const uint16_t n = logCount();

  Serial.println();
  Serial.printf("#VOICELOG v%u records=%u size=%u boots=%lu total=%lu\n",
                (unsigned)LOG_FORMAT_VERSION, (unsigned)n,
                (unsigned)sizeof(LogRecord), (unsigned long)logBootCount(),
                (unsigned long)logTotalWritten());

  LogRecord r;
  for (uint16_t i = 0; i < n; i++) {
    if (!logRead(i, &r)) break;
    const uint8_t* p = (const uint8_t*)&r;
    char line[2 * sizeof(LogRecord) + 1];
    for (unsigned b = 0; b < sizeof(LogRecord); b++) {
      snprintf(line + b * 2, 3, "%02X", p[b]);
    }
    Serial.println(line);
  }
  Serial.println("#END");
  Serial.println();
}

// -----------------------------------------------------------------------------
// The flash-backed history - the half that survives power being removed.
//
// Same records, same decoding, a different and much longer source. Records
// flagged `torn` are half-written ones left by a power cut mid-flush; there
// can be at most one per cut and it is always the newest, so seeing one is
// informative rather than alarming.
// -----------------------------------------------------------------------------
static void printLogFlash(bool raw) {
  if (!logFlashAvailable()) {
    Serial.println("no `eventlog` partition - nothing survives power off");
    return;
  }

  const uint32_t n = logFlashCount();
  Serial.println();
  Serial.printf("flash log %lu of %lu records, %lu still only in RTC\n",
                (unsigned long)n, (unsigned long)logFlashCapacity(),
                (unsigned long)logUnflushed());

  if (raw) {
    Serial.printf("#VOICELOG v%u records=%lu size=%u source=flash\n",
                  (unsigned)LOG_FORMAT_VERSION, (unsigned long)n,
                  (unsigned)sizeof(LogRecord));
  } else {
    Serial.println();
    Serial.println("     time  L  module  event             a           b");
    Serial.println("  -------  -  ------  ----------------  ----------  ----------");
  }

  LogRecord r;
  bool torn = false;
  for (uint32_t i = 0; i < n; i++) {
    if (!logFlashRead(i, &r, &torn)) break;

    if (raw) {
      const uint8_t* p = (const uint8_t*)&r;
      char line[2 * sizeof(LogRecord) + 1];
      for (unsigned b = 0; b < sizeof(LogRecord); b++) {
        snprintf(line + b * 2, 3, "%02X", p[b]);
      }
      Serial.println(line);
      continue;
    }

    if (torn) {
      Serial.println("  <torn record - power was lost part way through a write>");
      continue;
    }

    const char* name = logEventName(r.code);
    char label[20];
    if (name[0] == '\0') snprintf(label, sizeof(label), "event %u", (unsigned)r.code);
    else                 snprintf(label, sizeof(label), "%s", name);

    Serial.printf("  %4lu.%03lu  %s  %-6s  %-16s  %10ld  %10ld\n",
                  (unsigned long)(r.ms / 1000UL), (unsigned long)(r.ms % 1000UL),
                  logLevelName(r.level), logModuleName(r.module), label,
                  (long)r.a, (long)r.b);
  }
  if (raw) Serial.println("#END");
  Serial.println();
}

// -----------------------------------------------------------------------------
// Push the live settings at the modules that own them.
//
// The config store persists; it deliberately does not reach into other
// modules. Doing it here keeps the dependency pointing one way, and means the
// store can be tested without a radio or an I2S peripheral attached.
// -----------------------------------------------------------------------------
static void applyLiveSettings() {
  audioSetGainShift(config().micGainShift);
  audioSetCues(config().cueTones != 0);
  logSetMinLevel(configLogLevel());
  // Switching to "remember last used" should capture where the handset is
  // NOW, not wait until the next time somebody touches the button.
  configNotePreset(linkPresetIndex());
  // preroll and txmax are read from config() where they are used, every time,
  // so there is nothing to push for those two.
}

// -----------------------------------------------------------------------------
// `config set`, in two forms.
//
//     config set station 7                     one field, space separated
//     config set station=7 preset=5 cues=off   several fields, ONE flash write
//
// The second form is not just shorthand. Setting five fields one at a time is
// five NVS writes and five chances to lose power part way through; it is also
// five chances to end up half-configured, because a typo in the fourth value
// leaves the first three applied. Staging everything, validating everything,
// and then writing once collapses that into a single all-or-nothing operation.
//
// A rejection anywhere aborts the whole batch and the stored config is left
// exactly as it was - which is the property worth having, and the reason the
// error message names the field that failed.
// -----------------------------------------------------------------------------
static void configSet(const char* rest) {
  if (*rest == '\0') {
    Serial.println("usage: config set <name> <value>");
    Serial.println("       config set <name>=<value> [<name>=<value> ...]");
    return;
  }

  // Which form? A '=' anywhere means the batch form. Checking for it rather
  // than counting spaces keeps `config set key <hex>` working unchanged.
  const bool batch = strchr(rest, '=') != nullptr;

  configBatchBegin();
  uint8_t staged = 0;

  if (!batch) {
    char name[16] = "";
    const char* sp = strchr(rest, ' ');
    if (sp == nullptr || sp == rest) {
      Serial.println("usage: config set <name> <value>");
      configBatchAbort();
      return;
    }
    const size_t len = (size_t)(sp - rest);
    if (len >= sizeof(name)) {
      Serial.println("setting name too long");
      configBatchAbort();
      return;
    }
    memcpy(name, rest, len);
    name[len] = '\0';
    while (*sp == ' ') sp++;

    const char* err = nullptr;
    if (!configBatchStage(name, sp, &err)) {
      Serial.printf("rejected: %s\n", err ? err : "invalid");
      configBatchAbort();
      return;
    }
    staged = 1;

  } else {
    // Walk space-separated name=value tokens. Everything is staged before
    // anything is written, so the first bad token aborts with nothing changed.
    const char* p = rest;
    while (*p) {
      while (*p == ' ') p++;
      if (!*p) break;

      const char* tokEnd = p;
      while (*tokEnd && *tokEnd != ' ') tokEnd++;

      const char* eq = (const char*)memchr(p, '=', (size_t)(tokEnd - p));
      if (eq == nullptr || eq == p || eq + 1 == tokEnd) {
        Serial.printf("rejected: '%.*s' is not name=value - nothing was changed\n",
                      (int)(tokEnd - p), p);
        configBatchAbort();
        return;
      }

      char name[16];
      char value[40];
      const size_t nlen = (size_t)(eq - p);
      const size_t vlen = (size_t)(tokEnd - eq - 1);
      if (nlen >= sizeof(name) || vlen >= sizeof(value)) {
        Serial.println("rejected: name or value too long - nothing was changed");
        configBatchAbort();
        return;
      }
      memcpy(name, p, nlen);   name[nlen] = '\0';
      memcpy(value, eq + 1, vlen); value[vlen] = '\0';

      const char* err = nullptr;
      if (!configBatchStage(name, value, &err)) {
        Serial.printf("rejected: %s - nothing was changed\n", err ? err : "invalid");
        configBatchAbort();
        return;
      }
      staged++;
      p = tokEnd;
    }
  }

  if (staged == 0) {
    Serial.println("nothing to set");
    configBatchAbort();
    return;
  }

  const uint32_t changed = configBatchChanged();
  if (!configBatchCommit()) {
    Serial.println("values were accepted but the NVS write failed - nothing saved");
    return;
  }
  applyLiveSettings();

  // Report every field that moved, reading each back from the stored config
  // rather than echoing what was typed - so the line confirms what is now
  // true, not what was asked for.
  Serial.printf("%u setting%s saved in one write\n",
                (unsigned)staged, staged == 1 ? "" : "s");
  char value[24];
  for (uint8_t i = 0; i < configFieldCount(); i++) {
    if (!(changed & (1UL << i))) continue;
    configFieldValue(i, value, sizeof(value));
    Serial.printf("  %-7s = %-10s %s\n", configFieldName(i), value,
                  configFieldIsLive(i) ? "" : "(takes effect after a restart)");
  }
  if (configRestartPending()) Serial.println("  `reboot` to apply");
}

// -----------------------------------------------------------------------------
// The settings table.
//
// Shows where the running configuration came from, then every field with its
// value, its allowed range, whether a change takes effect now or at the next
// boot, and whether it still holds its build-time default. That last column is
// the one that answers "what has somebody changed on this board?", which is
// usually the real question.
// -----------------------------------------------------------------------------
static void printConfig() {
  Serial.println();
  Serial.printf("source    %s\n", configLoadResultName(configLoadResult()));
  Serial.printf("version   config v%u, %u bytes%s\n",
                (unsigned)config().version, (unsigned)config().size,
                configLoadResult() == CFG_LOAD_MIGRATED
                  ? "  (migrated on this boot)" : "");
  if (configLoadResult() == CFG_LOAD_MIGRATED) {
    Serial.printf("          written as v%u, upgraded in place and saved\n",
                  (unsigned)configStoredVersion());
  }
  Serial.println();
  Serial.println("  name     value       range     when      set?");
  Serial.println("  -------  ----------  --------  --------  ----");

  char value[24];
  for (uint8_t i = 0; i < configFieldCount(); i++) {
    configFieldValue(i, value, sizeof(value));
    Serial.printf("  %-7s  %-10s  %-8s  %-8s  %s\n",
                  configFieldName(i), value, configFieldRange(i),
                  configFieldIsLive(i) ? "now" : "restart",
                  configFieldIsDefault(i) ? "" : "changed");
  }

  Serial.println();
  for (uint8_t i = 0; i < configFieldCount(); i++) {
    Serial.printf("  %-7s  %s\n", configFieldName(i), configFieldHelp(i));
  }
  if (configRestartPending()) {
    Serial.println();
    Serial.println("  * a restart-only setting has changed - `reboot` to apply it");
  }
  Serial.println();
}

// The preset table, with the duty arithmetic worked out per row so you can see
// which ones are defensible without doing it in your head.
static void printPresets() {
  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;

  Serial.println();
  Serial.println("  #  preset        freq      pwr   limit   keyed   verdict");
  Serial.println("  -  ------------  --------  ----  ------  ------  ------------------");
  for (uint8_t i = 0; i < VOICE_PRESET_COUNT; i++) {
    const VoicePreset& p = linkPreset(i);
    const char* marker = (i == linkPresetIndex()) ? "*" : " ";

    if (p.kind != PRESET_RADIO) {
      Serial.printf(" %s%u  %-12s  %-8s  %-4s  %-6s  %-6s  %s\n",
                    marker, (unsigned)i, p.label, "-", "-", "-", "-", p.note);
      continue;
    }

    // "keyed" is what the duty cycle would be if you never let go of PTT. It
    // depends on the codec mode as well as the modem, so it is computed rather
    // than baked into the table.
    //
    // Only the ACTIVE row's airtime is measured - that is the one RadioLib was
    // asked about. The others are scaled from it by the roughly ten-to-one
    // ratio between LoRa SF7 and FSK 50k for this payload size. Getting exact
    // figures for every row would mean retuning the radio eight times just to
    // print a table.
    //
    // If the self-test preset is active there is no measured airtime at all,
    // so there is nothing to scale from and the row says so rather than
    // reporting a confident zero.
    const uint32_t active = linkAirtimeMs();
    if (active == 0) {
      Serial.printf(" %s%u  %-12s  %7.3f   %2d    %5.1f%%      ?   %s\n",
                    marker, (unsigned)i, p.label, (double)p.freqMHz,
                    (int)p.powerDbm, (double)p.dutyLimit, p.note);
      continue;
    }

    const uint32_t est = (p.modem == linkCurrentPreset().modem)
                       ? active
                       : (p.modem == MODEM_FSK ? active / 10 : active * 10);
    const uint32_t keyed = audioMs ? est * 100UL / audioMs : 0;

    const char* verdict = (keyed <= p.dutyLimit) ? "compliant"
                        : (keyed <= p.dutyLimit * 5) ? "over the limit"
                        : "far over the limit";

    Serial.printf(" %s%u  %-12s  %7.3f   %2d    %5.1f%%  %4lu%%   %s (%s)\n",
                  marker, (unsigned)i, p.label, (double)p.freqMHz,
                  (int)p.powerDbm, (double)p.dutyLimit,
                  (unsigned long)keyed, verdict, p.note);
  }
  Serial.println();
  Serial.println("  keyed = duty cycle while PTT is held; estimated for inactive rows.");
  Serial.println();
}

static void printStat() {
  const LinkStats& s = linkStats();
  const uint32_t up = millis() / 1000UL;
  const uint32_t budget = codecFrameMs() * 1000UL;
  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;

  const VoicePreset& p = linkCurrentPreset();

  Serial.println();
  Serial.printf("station   %u  (this board)\n", (unsigned)linkStationId());
  Serial.printf("state     %s for %lus\n", appStateName(),
                (unsigned long)((millis() - appStateSinceMs()) / 1000UL));
  if (p.kind != PRESET_RADIO) {
    Serial.printf("preset    %u = %s - %s; the radio is in standby\n",
                  (unsigned)linkPresetIndex(), p.label, p.note);
  } else if (p.modem == MODEM_LORA) {
    Serial.printf("preset    %u = %s, %.3f MHz, SF%d BW%.0f CR4/%d, %d dBm\n",
                  (unsigned)linkPresetIndex(), p.label, (double)p.freqMHz,
                  (int)VOICE_SF, (double)VOICE_BW_KHZ, (int)VOICE_CR,
                  (int)p.powerDbm);
  } else {
    Serial.printf("preset    %u = %s, %.3f MHz, FSK %.0f kbps dev %.0f kHz "
                  "rxbw %.1f kHz, %d dBm\n",
                  (unsigned)linkPresetIndex(), p.label, (double)p.freqMHz,
                  (double)VOICE_FSK_BR_KBPS, (double)VOICE_FSK_FDEV_KHZ,
                  (double)VOICE_FSK_RXBW_KHZ, (int)p.powerDbm);
  }
  Serial.printf("crypto    %s, key %s%s\n",
                linkEncryption() ? "ARMED" : "off",
                cryptoFingerprint(),
                cryptoKeyAvailable() ? "" : " (no key configured)");
  Serial.printf("codec     Codec2 %s, %d samples / %lums / %d bytes, %u per packet\n",
                codecModeName(), codecSamplesPerFrame(),
                (unsigned long)codecFrameMs(), codecBytesPerFrame(),
                (unsigned)VOICE_FRAMES_PER_PACKET);
  Serial.printf("cpu       encode %lu us (peak %lu) = %lu%% of the %lums frame\n",
                (unsigned long)codecEncodeUs(), (unsigned long)codecEncodePeakUs(),
                (unsigned long)(budget ? codecEncodeUs() * 100UL / budget : 0),
                (unsigned long)codecFrameMs());
  Serial.printf("          decode %lu us (peak %lu)\n",
                (unsigned long)codecDecodeUs(), (unsigned long)codecDecodePeakUs());
  Serial.printf("airtime   %lu ms per packet for %lu ms of audio = %u%% while keyed\n",
                (unsigned long)linkAirtimeMs(), (unsigned long)audioMs,
                (unsigned)linkKeyedDutyPercent());
  if (p.kind == PRESET_RADIO) {
    // The measured rolling hour, the limit for THIS sub-band, and what is left
    // expressed as talking time - which is the form you can actually use.
    Serial.printf("duty      %.3f%% of the last hour, limit %.0f%% for this "
                  "sub-band, %lus of talking left\n",
                  (double)linkDutyPercent(), (double)linkDutyLimit(),
                  (unsigned long)linkTalkSecondsLeft());
  }
  Serial.printf("packets   tx %lu  rx %lu  lost %lu  dropped %lu\n",
                (unsigned long)s.packetsTx, (unsigned long)s.packetsRx,
                (unsigned long)s.rxLost, (unsigned long)s.txDropped);
  Serial.printf("rejects   crc/read %lu  foreign %lu  codec-mismatch %lu  malformed %lu\n",
                (unsigned long)s.rxErrors, (unsigned long)s.rxForeign,
                (unsigned long)s.rxCodecMismatch, (unsigned long)s.rxMalformed);
  if (s.rxBadVersion > 0) {
    // Its own line because it is the one reject that means "upgrade a
    // handset" rather than "check your settings".
    Serial.printf("          %lu packets used a protocol version this build does "
                  "not speak (we send v%u)\n",
                  (unsigned long)s.rxBadVersion, (unsigned)VOICE_PROTO_VERSION);
  }
  if (s.rxNoKey > 0) {
    Serial.printf("          %lu encrypted packets dropped - the other handset "
                  "is armed and this one has no key\n", (unsigned long)s.rxNoKey);
  }
  if (s.packetsRx == 0) {
    Serial.println("signal    nothing received yet");
  } else if (p.modem == MODEM_LORA) {
    Serial.printf("signal    %.1f dBm, SNR %.1f dB, from station %u, "
                  "last packet %lus ago%s\n",
                  (double)s.lastRssi, (double)s.lastSnr,
                  (unsigned)s.lastStation,
                  (unsigned long)((millis() - linkLastRxMs()) / 1000UL),
                  linkRxWasEncrypted() ? ", encrypted" : "");
  } else {
    // No SNR estimator in FSK mode on this chip.
    Serial.printf("signal    %.1f dBm, SNR n/a (FSK), from station %u, "
                  "last packet %lus ago%s\n",
                  (double)s.lastRssi, (unsigned)s.lastStation,
                  (unsigned long)((millis() - linkLastRxMs()) / 1000UL),
                  linkRxWasEncrypted() ? ", encrypted" : "");
  }
  if (audioMicPresent()) {
    Serial.printf("mic       INMP441 in I2S slot %u, %u%% confidence%s\n",
                  (unsigned)audioMicSlot(), (unsigned)audioMicConfidence(),
                  audioTestSignalActive() ? ", OVERRIDDEN by test signal" : "");
    // The capture rate is not the codec rate, and that difference is the whole
    // reason speech comes back clean rather than muffled - see config.h.
    Serial.printf("          captured at %lu Hz (%ux oversampled), averaged "
                  "down to %d Hz for the codec\n",
                  (unsigned long)audioCaptureRate(),
                  (unsigned)VOICE_MIC_OVERSAMPLE, VOICE_SAMPLE_RATE);
  } else {
    Serial.println("mic       none - PTT sends the synthetic test signal");
  }
  Serial.printf("cues      %s - roger beep is generated here, never transmitted\n",
                audioCuesEnabled() ? "on" : "off");
  if (!audioAmpEnabled()) {
    Serial.println("amp       disabled at build time (-DNO_AMP)");
  } else if (!audioAmpSensed()) {
    Serial.println("amp       assumed present - not probed, see PIN_AMP_SD in config.h");
  } else {
    Serial.printf("amp       %s (%u/15 discharge trials, resting %u mV)\n",
                  audioAmpDetected() ? "detected" : "NOT DETECTED",
                  (unsigned)audioAmpVotes(), (unsigned)audioAmpSenseMv());
  }
  Serial.printf("audio     underruns %lu, capture overruns %lu, level %u%%\n",
                (unsigned long)appUnderruns(),
                (unsigned long)audioCaptureTimeouts(), (unsigned)audioLevel());
  // Clipping is the difference between a broken microphone and a loud one.
  Serial.printf("          %lu clipped samples%s\n",
                (unsigned long)audioClipCount(),
                audioClipCount()
                  ? "  <- lower VOICE_MIC_GAIN_SHIFT, or back off the mic"
                  : "");
  // Stack headroom for every task, worst case since boot.
  //
  // This is here because the first thing this firmware ever did on real
  // hardware was overflow a stack: codec2_create() puts 8 kB of FFT working
  // set in a single frame, and Arduino's loop task has 8 kB in total. The
  // failure mode is a reset with a corrupted backtrace, which tells you
  // nothing at all - whereas a margin that is shrinking tells you everything.
  //
  // uxTaskGetStackHighWaterMark() reports the MINIMUM free the task has ever
  // had, not a snapshot, so a healthy number here is a real guarantee. On
  // ESP-IDF it is in bytes, unlike vanilla FreeRTOS where it is words.
  Serial.print("stacks    ");
  static const char* const TASKS[] = { "loopTask", "voice", "link", "btn", "ui" };
  for (unsigned i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
    TaskHandle_t h = xTaskGetHandle(TASKS[i]);
    if (h) Serial.printf("%s %u  ", TASKS[i],
                         (unsigned)uxTaskGetStackHighWaterMark(h));
  }
  Serial.println("bytes never used");

  Serial.printf("system    up %luh%02lum%02lus, heap %lu bytes, display %s\n",
                (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
                (unsigned long)(up % 60), (unsigned long)ESP.getFreeHeap(),
                uiPresent() ? "present" : "absent");
  Serial.println();
}

// -----------------------------------------------------------------------------
// Command matching.
//
// True if `s` starts with `word` AND the next character is a space or the end
// of the string - so "enc" matches "enc" and "enc on" but not "encrypt". That
// second test is the whole reason this is not just strncmp: without it, every
// command would shadow every longer command that starts with the same letters.
//
// On a match, *rest is left pointing at the first argument character with the
// separating spaces skipped, or at the terminating NUL if there is no argument.
// Callers test *arg to tell "show me" from "set it to".
// -----------------------------------------------------------------------------
static bool matches(const char* s, const char* word, const char** rest) {
  const size_t n = strlen(word);
  if (strncmp(s, word, n) != 0) return false;
  if (s[n] != '\0' && s[n] != ' ') return false;
  const char* r = s + n;
  while (*r == ' ') r++;
  *rest = r;
  return true;
}

// -----------------------------------------------------------------------------
// Echo of the line just received, with secrets removed.
//
// The board does not echo typing, so a captured serial session is otherwise all
// answers and no questions - which is exactly the wrong half to keep when the
// session is going into a bug report or a lab notebook. The echo makes a
// transcript self-contained, and the `[cmd]` prefix makes it greppable
// alongside the other tagged output.
//
// The one thing that must not appear is the AES key. It reaches the console in
// two shapes:
//
//     config set key 000102030405060708090A0B0C0D0E0F
//     config set key=000102030405060708090A0B0C0D0E0F station=7
//
// Both are masked here rather than inside configSet(), because the echo happens
// before the line has been parsed and so cannot know which command it is
// looking at. Masking on the token `key` catches every route to that field,
// including a mistyped command that never reaches a handler at all.
//
// Nothing else is secret: `config` already renders the key as <set>, so no
// other command can put key material on the wire.
// -----------------------------------------------------------------------------
static void maskSecrets(const char* in, char* out, size_t cap) {
  size_t o = 0;
  bool nextIsSecret = false;    // set by a bare `key` token: the value follows

  const char* p = in;
  while (*p && o + 1 < cap) {
    // Separators are copied through, so the echo keeps the spacing that was
    // typed - which matters when the complaint is "it ignored my argument".
    while (*p == ' ' && o + 1 < cap) { out[o++] = ' '; p++; }
    if (!*p) break;

    const char* tok = p;
    while (*p && *p != ' ') p++;
    const size_t len = (size_t)(p - tok);

    // keep = how many leading characters of the token survive verbatim,
    // rep  = what is appended in place of the rest.
    size_t      keep = len;
    const char* rep  = nullptr;

    if (nextIsSecret) {
      keep = 0; rep = "<hidden>";
      nextIsSecret = false;
    } else if (len == 3 && strncmp(tok, "key", 3) == 0) {
      nextIsSecret = true;                 // `config set key <hex>`
    } else if (len > 4 && strncmp(tok, "key=", 4) == 0) {
      keep = 4; rep = "<hidden>";          // `key=<hex>` in the batch form
    }

    for (size_t i = 0; i < keep && o + 1 < cap; i++) out[o++] = tok[i];
    if (rep) for (const char* r = rep; *r && o + 1 < cap; r++) out[o++] = *r;
  }
  out[o] = '\0';   // a pathological line is truncated, never overrun
}

// -----------------------------------------------------------------------------
// Run one line. Nothing here touches the radio, the codec or the I2S
// peripheral directly - every command goes through the same entry point the
// buttons use, so the console cannot get the state machine into a position the
// buttons could not.
// -----------------------------------------------------------------------------
static void execute(char* cmd) {
  // Trim leading spaces, then trailing spaces and the carriage return that
  // terminals send ahead of the newline. Without the \r strip, "enc on\r"
  // would compare against "on\r" and never match "on".
  while (*cmd == ' ') cmd++;
  char* end = cmd + strlen(cmd);
  while (end > cmd && (end[-1] == ' ' || end[-1] == '\r')) *--end = '\0';
  if (*cmd == '\0') return;   // bare newline: no complaint, no output

  // Echoed before anything runs, so the command is on screen above its own
  // output even when the command reboots the board or takes a visible moment.
  //
  // Larger than `line` because masking can make text LONGER: `key=a` is five
  // characters and `key=<hidden>` is twelve. The worst case is a line made
  // entirely of six-character `key=a ` tokens, which is 2.2x, so 160 cannot be
  // reached from a 63-character input. maskSecrets() truncates rather than
  // overruns in any case.
  char shown[160];
  maskSecrets(cmd, shown, sizeof(shown));
  Serial.printf("[cmd] %s\n", shown);

  const char* arg = nullptr;

  if (matches(cmd, "help", &arg) || matches(cmd, "?", &arg)) {
    printHelp();

  } else if (matches(cmd, "stat", &arg)) {
    printStat();

  } else if (matches(cmd, "version", &arg) || matches(cmd, "ver", &arg)) {
    // Everything needed to identify this build, on one screenful. The dirty
    // flag is called out in words rather than a symbol because it is the part
    // people skip past, and it is the part that invalidates the hash.
    Serial.println();
    Serial.printf("firmware  %s\n", versionString());
    Serial.printf("semver    %s\n", versionSemver());
    Serial.printf("git       %s on %s%s\n", versionGitRev(), versionGitBranch(),
                  versionGitDirty()
                    ? "  (TREE WAS DIRTY - the hash does not identify the source)"
                    : "");
    Serial.printf("built     %s\n", versionBuildTimestamp());
    Serial.printf("protocol  v%u\n", (unsigned)VOICE_PROTO_VERSION);
    Serial.println();

  } else if (matches(cmd, "config", &arg) || matches(cmd, "cfg", &arg)) {
    const char* rest = nullptr;
    if (*arg == '\0' || matches(arg, "show", &rest)) {
      printConfig();

    } else if (matches(arg, "get", &rest)) {
      char value[24];
      if (*rest == '\0')                             Serial.println("usage: config get <name>");
      else if (configGetByName(rest, value, sizeof(value)))
        Serial.printf("%s = %s\n", rest, value);
      else Serial.printf("no setting called '%s' - try `config`\n", rest);

    } else if (matches(arg, "set", &rest)) {
      configSet(rest);

    } else if (matches(arg, "test", &rest)) {
      const char* why = nullptr;
      if (configTestMigration(&why)) {
        Serial.println("migration self test PASSED - a v1 blob survives intact");
      } else {
        Serial.printf("migration self test FAILED: %s\n", why ? why : "?");
      }

    } else if (matches(arg, "reset", &rest)) {
      if (configReset()) {
        applyLiveSettings();
        Serial.println("config reset to build defaults and saved - reboot to apply fully");
      } else {
        Serial.println("config reset in RAM but the NVS write failed");
      }

    } else {
      Serial.println("usage: config [show | get <n> | set <n> <v> | reset | test]");
    }

  } else if (matches(cmd, "post", &arg)) {
    // Re-runnable on demand. Most items are cheap re-reads; the battery is
    // measured fresh, which is the main reason to ask for it again - a
    // reading taken at boot says nothing about the cell twenty minutes of
    // transmitting later.
    postRun();

  } else if (matches(cmd, "log", &arg)) {
    if (strcmp(arg, "raw") == 0) {
      printLogRaw();
    } else if (strcmp(arg, "flash") == 0) {
      printLogFlash(false);
    } else if (strcmp(arg, "flash raw") == 0 || strcmp(arg, "flashraw") == 0) {
      printLogFlash(true);
    } else if (strcmp(arg, "flush") == 0) {
      // Force it now, regardless of the idle rule. Doing this by hand is what
      // you want before pulling the battery.
      const uint32_t n = logFlush();
      Serial.printf("flushed %lu records to flash\n", (unsigned long)n);
    } else if (strcmp(arg, "clear") == 0) {
      logClear();
      Serial.println("RTC ring cleared - the flash history is untouched");
      Serial.println("use `log erase` to wipe that too");
    } else if (strcmp(arg, "erase") == 0) {
      logEraseFlash();
      Serial.println("flash log erased");
    } else {
      printLog();
    }

  } else if (matches(cmd, "presets", &arg)) {
    printPresets();

  // `ch` kept as an alias: it is what the sibling lab and every radio in the
  // world calls this, even though a preset here is more than a frequency.
  } else if (matches(cmd, "preset", &arg) || matches(cmd, "ch", &arg)) {
    if (*arg) {
      const int n = atoi(arg);
      if (n < 0 || n >= VOICE_PRESET_COUNT) {
        Serial.printf("preset must be 0..%d (try `presets`)\n",
                      VOICE_PRESET_COUNT - 1);
      } else {
        linkSetPreset((uint8_t)n);
        // linkSetPreset() is asynchronous - it hands the change to linkTask.
        // Give it a moment so the readback below reports the new preset
        // rather than the old one.
        delay(50);
      }
    }
    {
      const VoicePreset& cur = linkCurrentPreset();
      if (cur.kind == PRESET_RADIO) {
        Serial.printf("preset %u = %s, %.3f MHz, %d dBm, limit %.0f%% (%s)\n",
                      (unsigned)linkPresetIndex(), cur.label,
                      (double)cur.freqMHz, (int)cur.powerDbm,
                      (double)cur.dutyLimit, cur.note);
      } else {
        Serial.printf("preset %u = %s - %s\n", (unsigned)linkPresetIndex(),
                      cur.label, cur.note);
      }
    }

  } else if (matches(cmd, "enc", &arg)) {
    if (*arg) {
      const bool on = (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
      if (!linkSetEncryption(on) && on) {
        Serial.printf("cannot arm: %s\n", cryptoKeyProblem());
      }
    }
    Serial.printf("encryption %s, key %s\n", linkEncryption() ? "ON" : "off",
                  cryptoFingerprint());

  } else if (matches(cmd, "screen", &arg)) {
    if (*arg) uiSetScreen((UiScreen)atoi(arg));
    else      uiNextScreen();
    Serial.printf("screen %d\n", (int)uiScreen());

  } else if (matches(cmd, "ptt", &arg)) {
    uint32_t ms = *arg ? (uint32_t)atol(arg) : 3000UL;
    if (ms > 30000UL) ms = 30000UL;   // this thing transmits; cap it
    buttonsInject(BTN_PTT_DOWN);
    pttReleaseAt = millis() + ms;
    if (linkSelfTestMode()) {
      Serial.printf("recording for %lu ms, then playing it back\n",
                    (unsigned long)ms);
    } else {
      Serial.printf("keyed for %lu ms\n", (unsigned long)ms);
    }

  } else if (matches(cmd, "tone", &arg)) {
    if (*arg) audioForceTestSignal(strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
    Serial.printf("test signal %s%s\n",
                  audioTestSignalActive() ? "active" : "off",
                  audioMicPresent() ? "" : " (no microphone fitted)");

  } else if (matches(cmd, "beep", &arg)) {
    if (*arg) audioSetCues(strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0);
    Serial.printf("cue tones %s\n", audioCuesEnabled() ? "on" : "off");
    if (audioCuesEnabled()) {
      // Play all four so you can learn what they mean by ear rather than from
      // the table in config.h. They are local: nothing goes on the air here,
      // on any preset, so this is safe to run as often as you like.
      //
      // Requested rather than played, because voiceTask owns the I2S
      // peripheral and this is the loop task. It happens as soon as the
      // handset is idle - which, if a transmission is arriving right now, is
      // when that transmission finishes.
      Serial.println("  keyup, incoming, roger, lost");
      appRequestCueDemo();
    }

  } else if (matches(cmd, "reboot", &arg)) {
    // Flush first. A reset preserves the RTC ring so nothing would be lost
    // either way, but a deliberate restart is a free moment to make the flash
    // history current - and if the next thing after the reboot is somebody
    // pulling the battery, it is the only moment.
    const uint32_t n = logFlush();
    if (n) Serial.printf("flushed %lu log records\n", (unsigned long)n);
    Serial.println("rebooting");
    Serial.flush();
    ESP.restart();

  } else {
    // The masked copy, not the raw line: a key typed after a mistyped command
    // is still a key.
    Serial.printf("unknown command: %s (try `help`)\n", shown);
  }
}

void consoleBegin() {
  Serial.println("[console] type `help` for commands");
}

void consolePoll() {
  // A scripted PTT press releases itself here rather than in a delay(), so the
  // console keeps answering while the radio is keyed - which is the one time
  // you might want to type `stat`.
  if (pttReleaseAt && (int32_t)(millis() - pttReleaseAt) >= 0) {
    pttReleaseAt = 0;
    buttonsInject(BTN_PTT_UP);
    Serial.println("released");
  }

  // Accumulate characters until a line terminator. Serial.available() returns
  // immediately when the buffer is empty, so this costs nothing on the idle
  // path and never blocks the loop task.
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      // Terminals send \r\n, so the second terminator arrives with fill == 0
      // and is ignored rather than running an empty command.
      if (fill > 0) {
        line[fill] = '\0';
        fill = 0;
        execute(line);
      }
    } else if (fill < sizeof(line) - 1) {
      line[fill++] = c;
    }
    // Overlong lines drop their tail rather than overflowing the buffer. The
    // command still runs on the prefix, which for a typo is the right amount
    // of unhelpful.
  }
}
