#include "post.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "codec.h"
#include "config.h"
#include "configstore.h"
#include "crypto.h"
#include "link.h"
#include "log.h"
#include "ui.h"

static PostResult results[POST_ITEM_COUNT];
static char       details[POST_ITEM_COUNT][48];
static uint8_t    failures = 0;
static uint8_t    warnings = 0;
static float      batteryVolts = 0.0f;
static char       summary[24] = "POST not run";

static void set(PostItem item, PostResult r, const char* fmt, ...) {
  results[item] = r;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(details[item], sizeof(details[item]), fmt, ap);
  va_end(ap);

  if (r == POST_FAIL) failures++;
  if (r == POST_WARN) warnings++;
}

// =============================================================================
// Battery voltage.
//
// GPIO35 carries a 1:2 divider from the battery on the T3_V1.6.1, so the pin
// sees half the pack voltage and the reading is doubled back.
//
// analogReadMilliVolts() is used rather than analogRead() because the raw ADC
// on this chip is markedly non-linear and varies part to part; the milliVolts
// call applies the per-chip calibration burned into eFuse at the factory.
// Without it a "measurement" here would be a number with the shape of a
// voltage and none of the accuracy, which is exactly the kind of manufactured
// confidence this file is supposed to avoid.
//
// Averaging sixteen reads costs nothing at boot and takes the edge off the
// noise, which on a board with a radio on it is not negligible.
//
// The result is reported honestly: with no cell fitted and the board running
// from USB, this pin reads whatever the divider leaks to, and that is NOT a
// battery voltage. Anything below a plausible floor is reported as "no cell"
// rather than as a low battery, because sending somebody to charge a battery
// that is not there is worse than saying nothing.
// =============================================================================
static void checkPower() {
#if defined(PIN_VBAT_SENSE)
  const int pin = PIN_VBAT_SENSE;
#else
  const int pin = 35;
#endif

  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(pin);
  mv /= 16;

  batteryVolts = (float)mv * 2.0f / 1000.0f;   // undo the 1:2 divider

  // A single LiPo runs 3.0 V flat to 4.2 V full. Below 2.5 V there is no cell
  // on the divider at all; above 5.5 V the divider or the pin is not what we
  // think it is.
  if (batteryVolts < 2.5f) {
    set(POST_POWER, POST_SKIP, "no cell on the divider (USB power?)");
    batteryVolts = 0.0f;
  } else if (batteryVolts > 5.5f) {
    set(POST_POWER, POST_WARN, "%.2f V implausible - check the divider",
        (double)batteryVolts);
  } else if (batteryVolts < 3.3f) {
    // 3.3 V under no load is most of the way down a LiPo discharge curve, and
    // transmitting pulls it further. This is a warning rather than a failure
    // because the handset still works - right up until it does not.
    set(POST_POWER, POST_WARN, "%.2f V - low, recharge soon",
        (double)batteryVolts);
  } else {
    set(POST_POWER, POST_PASS, "%.2f V", (double)batteryVolts);
  }
}

void postRun() {
  memset(results, 0, sizeof(results));
  memset(details, 0, sizeof(details));
  failures = warnings = 0;

  // ---- settings ------------------------------------------------------------
  // OBSERVED: configBegin() already ran and told us how it went.
  {
    const ConfigLoadResult r = configLoadResult();
    if (r == CFG_LOAD_OK) {
      set(POST_CONFIG, POST_PASS, "v%u loaded, CRC ok", (unsigned)config().version);
    } else if (r == CFG_LOAD_ABSENT) {
      set(POST_CONFIG, POST_PASS, "defaults (nothing stored yet)");
    } else {
      // Something was stored and was wrong. The handset runs on defaults, so
      // it works - but somebody's settings are gone and they should know.
      set(POST_CONFIG, POST_WARN, "%s", configLoadResultName(r));
    }
  }

  // ---- event log -----------------------------------------------------------
  {
    if (logSurvivedReset()) {
      set(POST_LOG, POST_PASS, "%u records survived boot #%lu",
          (unsigned)logCount(), (unsigned long)logBootCount());
    } else {
      set(POST_LOG, POST_PASS, "initialised (cold start)");
    }
  }

  // ---- codec ---------------------------------------------------------------
  // MEASURED. "Did codec2_create() succeed" is necessary but not sufficient:
  // a codec that runs slower than real time compiles, links and initialises
  // perfectly and then produces broken audio. So encode one frame of silence
  // and time it against the frame budget.
  if (!codecReady()) {
    set(POST_CODEC, POST_FAIL, "codec2_create failed");
  } else if (!codecCallerOk()) {
    // Two tasks have touched the codec. That corrupts its state and overflows
    // whichever caller was not sized for it, so it is a failure even though
    // everything may look fine for a while.
    set(POST_CODEC, POST_FAIL, "codec used from more than one task");
  } else {
    // MEASURED, but measured ELSEWHERE. The bench runs on voiceTask, because
    // Codec2's decode path alone needs more stack than this task has - trying
    // to time it here is what crashed the first version of this check.
    //
    // "Did codec2_create() succeed" is necessary but nowhere near sufficient:
    // a codec that runs slower than real time initialises perfectly and then
    // produces broken audio.
    // The WORSE of encode and decode, each against a whole frame period.
    // Half duplex means only one of them ever runs at a time, so charging one
    // frame for the sum of both is the wrong budget by a factor of two - which
    // is exactly the mistake the first version of this check made.
    const uint32_t us     = codecBenchUs();
    const uint32_t budget = codecFrameMs() * 1000UL;
    const uint32_t pct    = budget ? (us * 100UL / budget) : 0;
    const uint32_t encPct = budget ? (codecBenchEncodeUs() * 100UL / budget) : 0;
    const uint32_t decPct = budget ? (codecBenchDecodeUs() * 100UL / budget) : 0;

    if (us == 0) {
      set(POST_CODEC, POST_WARN, "%s: not benched yet", codecModeName());
    } else if (pct >= 85) {
      set(POST_CODEC, POST_FAIL, "%s: enc %lu%% dec %lu%% of frame",
          codecModeName(), (unsigned long)encPct, (unsigned long)decPct);
    } else if (pct >= 60) {
      set(POST_CODEC, POST_WARN, "%s: enc %lu%% dec %lu%% of frame",
          codecModeName(), (unsigned long)encPct, (unsigned long)decPct);
    } else {
      set(POST_CODEC, POST_PASS, "%s: enc %lu%% dec %lu%% of frame",
          codecModeName(), (unsigned long)encPct, (unsigned long)decPct);
    }
    if (pct >= 60) logWarn(LOG_MOD_CODEC, LOG_EV_CODEC_SLOW, (int32_t)us,
                           (int32_t)budget);
  }

  // ---- radio ---------------------------------------------------------------
  // OBSERVED plus MEASURED: linkBegin() would have refused to start the task
  // if the chip had not answered, and the airtime figure is read back from the
  // modem registers rather than computed here.
  {
    const VoicePreset& p = linkCurrentPreset();
    if (p.kind != PRESET_RADIO) {
      set(POST_RADIO, POST_SKIP, "self-test preset - radio parked");
    } else if (linkAirtimeMs() == 0) {
      set(POST_RADIO, POST_FAIL, "no airtime reading - radio not configured");
    } else {
      set(POST_RADIO, POST_PASS, "%.3f MHz, %lums airtime, %u%% keyed",
          (double)p.freqMHz, (unsigned long)linkAirtimeMs(),
          (unsigned)linkKeyedDutyPercent());
    }
  }

  // ---- microphone ----------------------------------------------------------
  // MEASURED at boot by the I2S signature probe - not a level threshold. See
  // audioProbeMic() in audio.cpp.
  if (audioMicPresent()) {
    set(POST_MIC, POST_PASS, "INMP441 slot %u, %u%% confidence",
        (unsigned)audioMicSlot(), (unsigned)audioMicConfidence());
  } else {
    // Not a failure. A board with no microphone is a perfectly good receiver
    // and this rig is often deliberately run that way.
    set(POST_MIC, POST_WARN, "none found - PTT sends the test signal");
  }

  // ---- amplifier -----------------------------------------------------------
  // ASSERTED, and it says so. The MAX98357A has no readback path of any kind:
  // no status pin, no I2C, nothing. What we can honestly claim is that the
  // ESP32 side of the link was configured, which is the half of it we control.
  if (!audioAmpEnabled()) {
    set(POST_AMP, POST_SKIP, "disabled at build time (-DNO_AMP)");
  } else if (!audioAmpSensed()) {
    // ASSERTED, and it says so. Without the SD_MODE sense wire the MAX98357A
    // has no readback path of any kind, and what we can honestly claim is that
    // the ESP32 side was configured - the half we control. Wire PIN_AMP_SD and
    // this item becomes a measurement; see config.h.
    set(POST_AMP, POST_PASS, "I2S TX ready (not probed - see PIN_AMP_SD)");
  } else if (audioAmpDetected()) {
    // MEASURED. The amplifier's internal 100k pulldown discharges the node
    // after we charge and release it - see ampProbeOnce() in audio.cpp.
    set(POST_AMP, POST_PASS, "detected, %u/15 trials", (unsigned)audioAmpVotes());
  } else {
    // A warning, not a failure: a board with no amplifier is a perfectly good
    // transmitter, and this rig is deliberately run that way.
    set(POST_AMP, POST_WARN, "none found, %u/15 trials - nothing will be heard",
        (unsigned)audioAmpVotes());
  }

  // ---- power ---------------------------------------------------------------
  checkPower();

  // ---- memory --------------------------------------------------------------
  // MEASURED. Heap alone is not enough: this firmware has already been taken
  // down once by a stack overflow that left plenty of heap free, so the
  // tightest task stack is checked too.
  {
    const uint32_t heap = ESP.getFreeHeap();
    static const char* const TASKS[] = { "loopTask", "voice", "link", "btn", "ui" };
    uint32_t worst = UINT32_MAX;
    const char* worstName = "?";
    for (unsigned i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
      TaskHandle_t h = xTaskGetHandle(TASKS[i]);
      if (!h) continue;
      const uint32_t free = uxTaskGetStackHighWaterMark(h);
      if (free < worst) { worst = free; worstName = TASKS[i]; }
    }
    if (worst == UINT32_MAX) worst = 0;

    if (heap < 20000 || worst < 512) {
      set(POST_MEMORY, POST_FAIL, "heap %luk, %s stack %lu B",
          (unsigned long)(heap / 1024), worstName, (unsigned long)worst);
    } else if (heap < 40000 || worst < 1024) {
      set(POST_MEMORY, POST_WARN, "heap %luk, %s stack %lu B",
          (unsigned long)(heap / 1024), worstName, (unsigned long)worst);
    } else {
      set(POST_MEMORY, POST_PASS, "heap %luk, %s stack %lu B",
          (unsigned long)(heap / 1024), worstName, (unsigned long)worst);
    }
    if (worst < 1024) {
      logWarn(LOG_MOD_BOOT, LOG_EV_STACK_LOW, 0, (int32_t)worst);
    }
  }

  // ---- report --------------------------------------------------------------
  // A failure is the headline if there is one, then a warning, then the pass
  // count - the display has room for one line and this is the priority order
  // somebody holding the board cares about.
  if (failures) {
    snprintf(summary, sizeof(summary), "POST %u FAIL", (unsigned)failures);
  } else if (warnings) {
    snprintf(summary, sizeof(summary), "POST %u warn", (unsigned)warnings);
  } else {
    snprintf(summary, sizeof(summary), "POST %u pass", (unsigned)POST_ITEM_COUNT);
  }

  Serial.println();
  Serial.println("[post] power-on self test");
  for (uint8_t i = 0; i < POST_ITEM_COUNT; i++) {
    Serial.printf("[post]   %-8s %-5s %s\n", postItemName(i),
                  postResultName(results[i]), details[i]);
    logWrite(results[i] == POST_FAIL ? LOG_LVL_ERROR
             : results[i] == POST_WARN ? LOG_LVL_WARN : LOG_LVL_INFO,
             LOG_MOD_POST, LOG_EV_POST_ITEM, (int32_t)i, (int32_t)results[i]);
  }
  Serial.printf("[post] %s\n", summary);
  logWrite(failures ? LOG_LVL_ERROR : warnings ? LOG_LVL_WARN : LOG_LVL_INFO,
           LOG_MOD_POST, LOG_EV_POST_SUMMARY, (int32_t)failures,
           (int32_t)warnings);

  // A failure is put in front of whoever is holding the board, not just in a
  // log they have to know to read.
  if (failures) {
    for (uint8_t i = 0; i < POST_ITEM_COUNT; i++) {
      if (results[i] == POST_FAIL) {
        uiFlash("POST FAIL", postItemName(i), 4000);
        break;
      }
    }
  }
}

bool     postFailed()    { return failures > 0; }
uint8_t  postFailures()  { return failures; }
uint8_t  postWarnings()  { return warnings; }
float    postBatteryVolts() { return batteryVolts; }
const char* postSummary()   { return summary; }

PostResult postItemResult(uint8_t item) {
  return item < POST_ITEM_COUNT ? results[item] : POST_SKIP;
}

const char* postItemDetail(uint8_t item) {
  return item < POST_ITEM_COUNT ? details[item] : "";
}

const char* postItemName(uint8_t item) {
  static const char* const NAMES[POST_ITEM_COUNT] = {
    "config", "log", "codec", "radio", "mic", "amp", "power", "memory",
  };
  return item < POST_ITEM_COUNT ? NAMES[item] : "?";
}

const char* postResultName(PostResult r) {
  switch (r) {
    case POST_PASS: return "pass";
    case POST_WARN: return "WARN";
    case POST_FAIL: return "FAIL";
    default:        return "skip";
  }
}
