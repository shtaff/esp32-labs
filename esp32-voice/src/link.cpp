// =============================================================================
// link.cpp - the SX1276, and the only task allowed to touch it.
//
// linkTask sleeps on a task notification and does nothing else. Three things
// wake it:
//
//   NOTIFY_DIO0     the DIO0 interrupt. The chip has either finished sending
//                   or has a packet waiting; which one is decided by what we
//                   asked it to do, not by the pin.
//   NOTIFY_TX       voiceTask has queued a packet.
//   NOTIFY_PRESET   somebody pressed the mode button.
//
// Everything else in the firmware talks to the radio by queueing something and
// setting a bit. That is not ceremony: RadioLib keeps the chip's mode in its
// own state, so two tasks calling into it interleave a shared state machine
// across a shared SPI bus, and the failure mode is a radio that is quietly in
// the wrong mode rather than anything that looks like a bug.
// =============================================================================
#include "link.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <string.h>

#include "codec.h"
#include "config.h"
#include "crypto.h"
#include "ui.h"

// The FSK FIFO on this chip is 64 bytes and RadioLib will not split a packet
// across refills, so a packet that does not fit is simply refused at runtime -
// on the FSK presets only, which is a miserable way to find out. Catch it here
// instead. If this fires: lower VOICE_FRAMES_PER_PACKET, pick a codec mode
// with smaller frames, or delete the FSK rows from VOICE_PRESETS below.
static_assert(VOICE_PKT_MAX < RADIOLIB_SX127X_MAX_PACKET_LENGTH_FSK,
              "packet too large for the SX1276 FSK FIFO - see VOICE_PKT_MAX");

// -----------------------------------------------------------------------------
// The preset table.
//
// Ordered so the ones you want most are reachable in the fewest presses, and
// so the self test is last - you have to walk past everything to land on it,
// which is the right amount of friction for a mode that stops transmitting.
//
// The duty limits are EN 300 220's, per sub-band. The powers are what the
// sub-band permits, capped by what the SX1276 can do continuously: +20 dBm is
// rated by Semtech for 1 % duty and is therefore useless here, so the g3
// presets ask for +17 rather than the +27 the band would allow.
// -----------------------------------------------------------------------------
static const VoicePreset VOICE_PRESETS[VOICE_PRESET_COUNT] = {
  // label          note                       kind          modem       MHz      dBm  band            limit
  { "868.1 LoRa",  "default, best range",     PRESET_RADIO, MODEM_LORA, 868.100f, 14, VOICE_BAND_G1,    1.0f },
  { "868.3 LoRa",  "if 868.1 is busy",        PRESET_RADIO, MODEM_LORA, 868.300f, 14, VOICE_BAND_G1,    1.0f },
  { "868.5 LoRa",  "if 868.3 is busy",        PRESET_RADIO, MODEM_LORA, 868.500f, 14, VOICE_BAND_G1,    1.0f },
  { "867.1 LoRa",  "separate 1% budget",      PRESET_RADIO, MODEM_LORA, 867.100f, 14, VOICE_BAND_G,     1.0f },
  { "869.5 LoRa",  "10% band, full range",    PRESET_RADIO, MODEM_LORA, 869.525f, 17, VOICE_BAND_G3,   10.0f },
  { "869.5 FSK50", "compliant, short range",  PRESET_RADIO, MODEM_FSK,  869.525f, 17, VOICE_BAND_G3,   10.0f },
  { "869.9 FSK50", "no duty limit, 5mW",      PRESET_RADIO, MODEM_FSK,  869.850f,  7, VOICE_BAND_G4,  100.0f },
  { "SELF TEST",   "record and play locally", PRESET_SELFTEST, MODEM_LORA, 0.0f,   0, VOICE_BAND_G1,  100.0f },
};

const VoicePreset& linkPreset(uint8_t index) {
  return VOICE_PRESETS[index % VOICE_PRESET_COUNT];
}

// Module(cs, irq, rst, gpio). DIO0 is TxDone and RxDone both; DIO1 and DIO2
// are not used, so RADIOLIB_NC.
static SX1276 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, RADIOLIB_NC);

#define NOTIFY_DIO0   0x01
#define NOTIFY_TX     0x02
#define NOTIFY_PRESET 0x04

struct TxPacket {
  uint8_t len;
  uint8_t data[VOICE_PKT_MAX];
};

static TaskHandle_t  linkTaskHandle = nullptr;
static QueueHandle_t txQueue = nullptr;
static QueueHandle_t rxQueue = nullptr;

static LinkStats stats = {};
static uint32_t  airtimeMs = 0;
static uint8_t   stationId = 0;

static volatile uint8_t presetIndex = VOICE_DEFAULT_PRESET;
static volatile uint8_t requestedPreset = VOICE_DEFAULT_PRESET;
static volatile bool    encryptionOn = false;
static volatile bool    txInProgress = false;

// Transmit-side stream state, owned by whoever calls linkSendFrames -
// voiceTask, and nobody else.
static uint32_t txStreamId = 0;
static uint16_t txSeq = 0;

// Receive-side stream state, owned by linkTask.
static uint32_t rxStreamId = 0;
static bool     rxStreamKnown = false;
static uint16_t rxLastSeq = 0;
static volatile uint32_t rxLastMs = 0;
static volatile bool     rxEncrypted = false;
static volatile bool     rxEnded = false;

// =============================================================================
// Rolling-hour duty cycle, per sub-band.
//
// One bucket per minute per band. The current minute is millis()/60000, and
// its bucket is (minute % 60); advancing past a minute clears whatever that
// bucket held an hour ago, so the array is always exactly the last hour.
//
// Per band, because EN 300 220 assesses the limit per sub-band. Transmitting
// on 868.1 spends the g1 budget and leaves g3 untouched - and since g3 is the
// only place this application can be legal, sharing one counter between them
// would misreport the one number worth trusting.
// =============================================================================
static uint32_t dutyBucket[VOICE_BAND_COUNT][60] = {};
static uint32_t dutyLastMinute = 0;
static bool     dutyStarted = false;

// Zero out every bucket that has aged past an hour since we last looked.
static void dutyAdvance() {
  const uint32_t minute = millis() / 60000UL;
  if (!dutyStarted) {
    dutyLastMinute = minute;
    dutyStarted = true;
    return;
  }
  if (minute == dutyLastMinute) return;

  const uint32_t elapsed = minute - dutyLastMinute;
  if (elapsed >= 60) {
    // Idle for an hour or more: the whole window is stale.
    memset(dutyBucket, 0, sizeof(dutyBucket));
  } else {
    for (uint32_t m = dutyLastMinute + 1; m <= minute; m++) {
      for (int b = 0; b < VOICE_BAND_COUNT; b++) dutyBucket[b][m % 60] = 0;
    }
  }
  dutyLastMinute = minute;
}

static void dutyAdd(VoiceBand band, uint32_t ms) {
  dutyAdvance();
  dutyBucket[band][(millis() / 60000UL) % 60] += ms;
}

// Milliseconds of airtime spent in one band over the last rolling hour.
static uint32_t dutyUsedMs(VoiceBand band) {
  dutyAdvance();
  uint32_t total = 0;
  for (int i = 0; i < 60; i++) total += dutyBucket[band][i];
  return total;
}

float linkDutyPercent() {
  // 3600000 ms in an hour, so used/3600000 is the fraction and x100 the
  // percent.
  return (float)dutyUsedMs(linkCurrentPreset().band) * 100.0f / 3600000.0f;
}

float linkDutyLimit() { return linkCurrentPreset().dutyLimit; }

uint8_t linkKeyedDutyPercent() {
  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;
  if (audioMs == 0 || airtimeMs == 0) return 0;
  return (uint8_t)(airtimeMs * 100UL / audioMs);
}

uint32_t linkTalkSecondsLeft() {
  const VoicePreset& p = linkCurrentPreset();
  if (p.kind != PRESET_RADIO) return 0;

  const uint32_t budgetMs = (uint32_t)(p.dutyLimit * 36000.0f);  // limit% of 1h
  const uint32_t usedMs   = dutyUsedMs(p.band);
  if (usedMs >= budgetMs) return 0;
  const uint32_t leftMs = budgetMs - usedMs;

  // Airtime is only a fraction of wall-clock time while talking, so a given
  // airtime budget buys proportionally more seconds of speech: one second of
  // talking costs (airtime / audio) seconds of airtime.
  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;
  if (airtimeMs == 0) return 0;
  const uint32_t secs =
      (uint32_t)((uint64_t)leftMs * audioMs / airtimeMs / 1000ULL);

  // Clamp to an hour. On the 100 % band the arithmetic says you have a day of
  // talking left in the next hour, which is true but not useful - there are
  // only 3600 seconds in the window being measured. Clamping makes the display
  // read "the whole hour" instead of a number that looks like a bug.
  return secs > 3600 ? 3600 : secs;
}

// -----------------------------------------------------------------------------
// The interrupt. It must do nothing but wake the task: it runs from IRAM with
// the scheduler suspended, and anything that touches SPI - which is everything
// interesting about a radio - would deadlock there.
// -----------------------------------------------------------------------------
static ICACHE_RAM_ATTR void onDio0() {
  // The interrupt is attached before linkTask exists, and a spurious edge in
  // that window would be a null dereference inside an ISR - which on this
  // chip is a reset with no usable backtrace.
  if (linkTaskHandle == nullptr) return;
  BaseType_t woken = pdFALSE;
  xTaskNotifyFromISR(linkTaskHandle, NOTIFY_DIO0, eSetBits, &woken);
  if (woken) portYIELD_FROM_ISR();
}

static void notifyTask(uint32_t bits) {
  if (linkTaskHandle == nullptr) return;
  xTaskNotify(linkTaskHandle, bits, eSetBits);
}

// =============================================================================
// Applying a preset. Called at boot and on every preset change, always from
// linkTask, always with the radio quiet.
//
// This is a full re-initialisation, not a retune, because LoRa and FSK are
// different modems inside the chip and there is no path between them that does
// not go through begin(). Doing it the same way for both keeps one code path
// instead of two, and it costs a few milliseconds on a human-speed action.
// =============================================================================
static int16_t applyPreset(const VoicePreset& p) {
  if (p.kind == PRESET_SELFTEST) {
    // No radio settings to apply. Park the chip so it is not receiving
    // packets nothing is going to look at, and so the PA is definitely off.
    radio.standby();
    airtimeMs = 0;
    return RADIOLIB_ERR_NONE;
  }

  int16_t state;

  if (p.modem == MODEM_LORA) {
    state = radio.begin(p.freqMHz, VOICE_BW_KHZ, VOICE_SF, VOICE_CR,
                        VOICE_SYNC_WORD, p.powerDbm,
                        VOICE_PREAMBLE_SYMBOLS, 0);
    if (state != RADIOLIB_ERR_NONE) return state;

  } else {
    // beginFSK's preamble length is in BITS - RadioLib divides by 8 for the
    // register, which is in bytes.
    state = radio.beginFSK(p.freqMHz, VOICE_FSK_BR_KBPS, VOICE_FSK_FDEV_KHZ,
                           VOICE_FSK_RXBW_KHZ, p.powerDbm,
                           VOICE_FSK_PREAMBLE_BITS, false);
    if (state != RADIOLIB_ERR_NONE) return state;

    // FSK needs a sync word set explicitly - unlike LoRa, where it is one byte
    // passed to begin(). Two bytes is the conventional length; it is what the
    // receiver correlates against to find the start of a packet.
    uint8_t sync[2] = { VOICE_FSK_SYNC_0, VOICE_FSK_SYNC_1 };
    state = radio.setSyncWord(sync, sizeof(sync));
    if (state != RADIOLIB_ERR_NONE) return state;

    // Gaussian filtering, BT = 0.5. Unshaped FSK has hard symbol transitions,
    // and hard transitions have wide skirts - at 50 kbit/s that spills outside
    // the 250 kHz sub-band the g3 presets are trying to stay inside. This is
    // the difference between "fits in the band" and "technically does not".
    state = radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    if (state != RADIOLIB_ERR_NONE) return state;

    // Variable length: a length byte goes on the air ahead of the payload, so
    // the receiver does not have to be told the size in advance. Costs one
    // byte per packet and means the tail packet of a transmission - which is
    // usually short - does not have to be padded out.
    state = radio.variablePacketLengthMode(VOICE_PKT_MAX);
    if (state != RADIOLIB_ERR_NONE) return state;
  }

  // CRC on, both modems. A corrupted codec frame is not silently wrong audio
  // here - it is a burst of noise straight into somebody's ear - so it is
  // worth the two bytes to have the chip throw the packet away for us.
  state = radio.setCRC(true);
  if (state != RADIOLIB_ERR_NONE) return state;

  state = radio.setCurrentLimit(VOICE_CURRENT_LIMIT_MA);
  if (state != RADIOLIB_ERR_NONE) return state;

  // Measured, not computed. getTimeOnAir() reads the modem registers back and
  // accounts for the preamble, sync word, length byte and CRC that the chip
  // adds on its own - all of which differ between the two modems.
  airtimeMs = (uint32_t)(radio.getTimeOnAir(VOICE_PKT_MAX) / 1000ULL);

  // begin()/beginFSK() reset the DIO mapping, so the interrupt has to be
  // re-armed after every preset change. Forgetting this is a radio that
  // transmits once and then never reports anything again.
  radio.setDio0Action(onDio0, RISING);

  return RADIOLIB_ERR_NONE;
}

// -----------------------------------------------------------------------------
// One received packet, start to finish. Runs in linkTask.
// -----------------------------------------------------------------------------
static void handleRx() {
  uint8_t buf[VOICE_PKT_MAX];

  const size_t len = radio.getPacketLength();
  if (len == 0 || len > sizeof(buf)) {
    stats.rxErrors++;
    return;
  }

  const int16_t state = radio.readData(buf, len);
  if (state != RADIOLIB_ERR_NONE) {
    // Overwhelmingly RADIOLIB_ERR_CRC_MISMATCH: a packet arrived and was
    // damaged. On a shared public band that is normal traffic, not a fault.
    stats.rxErrors++;
    return;
  }

  if (len < VOICE_PKT_HEADER || buf[0] != VOICE_PKT_MAGIC) {
    stats.rxForeign++;
    return;
  }

  // Unpack the header. Little endian, byte at a time, so this does not depend
  // on the compiler's struct packing or the machine's alignment rules.
  const uint8_t  flags       = buf[1];
  const uint8_t  pktCodecId  = (flags & VOICE_FLAG_CODEC_MASK) >> VOICE_FLAG_CODEC_SHIFT;
  const uint32_t streamId    = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) |
                               ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
  const uint16_t seq         = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
  const uint8_t  station     = buf[8];

  if (pktCodecId != codecId()) {
    // The far end is built for a different bit rate. Decoding it would produce
    // noise, and noise that sounds like a broken radio rather than like a
    // configuration mistake - so say which it is.
    stats.rxCodecMismatch++;
    return;
  }

  const size_t payload = len - VOICE_PKT_HEADER;
  const int bytesPerFrame = codecBytesPerFrame();
  if (payload == 0 || (payload % bytesPerFrame) != 0) {
    stats.rxMalformed++;
    return;
  }

  if (flags & VOICE_FLAG_ENCRYPTED) {
    if (!cryptoKeyAvailable()) {
      // Encrypted traffic and no key. Perfectly well formed, and simply not
      // for us - so it gets its own counter rather than being lumped in with
      // corruption. This is what a handset looks like when the other end
      // double-tapped MODE and this one has no secrets.ini.
      stats.rxNoKey++;
      return;
    }
    // CTR is its own inverse, so the same call encrypts and decrypts. The
    // counter block is rebuilt from the three header fields the sender used.
    cryptoApply(buf + VOICE_PKT_HEADER, payload, streamId, seq, station);
  }

  // Stream bookkeeping. A new streamId means the far end pressed PTT again, so
  // anything still buffered belongs to a transmission that is over: play it
  // and you are playing the end of the last over on top of the start of this
  // one, several hundred milliseconds late.
  if (!rxStreamKnown || streamId != rxStreamId) {
    xQueueReset(rxQueue);
    rxStreamId = streamId;
    rxStreamKnown = true;
    rxEnded = false;
  } else {
    const uint16_t expected = (uint16_t)(rxLastSeq + 1);
    if (seq != expected) {
      // Unsigned arithmetic makes this the forward distance even across the
      // 16-bit wrap. A packet that arrives out of order shows up as a large
      // gap followed by nothing, which is close enough for a counter.
      const uint16_t gap = (uint16_t)(seq - expected);
      if (gap < 1000) stats.rxLost += gap;
    }
  }
  rxLastSeq = seq;

  // Split into codec frames and hand them to the jitter buffer.
  const uint8_t frames = (uint8_t)(payload / bytesPerFrame);
  for (uint8_t i = 0; i < frames; i++) {
    const uint8_t* frame = buf + VOICE_PKT_HEADER + (size_t)i * bytesPerFrame;
    if (xQueueSend(rxQueue, frame, 0) != pdTRUE) {
      // The jitter buffer is full, which means playback is not draining it -
      // either it has not started yet or the codec is too slow. Drop the
      // OLDEST frame: in a live conversation the newest audio is the useful
      // audio, and dropping from the front keeps latency from growing.
      uint8_t discard[VOICE_MAX_BYTES_PER_FRAME];
      xQueueReceive(rxQueue, discard, 0);
      xQueueSend(rxQueue, frame, 0);
    }
  }

  if (flags & VOICE_FLAG_END) rxEnded = true;

  stats.packetsRx++;
  stats.lastRssi = radio.getRSSI();
  // The SX1276 only estimates SNR in LoRa mode - it falls out of the spreading
  // correlation, and FSK has no equivalent. Reporting a stale LoRa number on
  // an FSK preset would be worse than reporting nothing.
  stats.lastSnr = (linkCurrentPreset().modem == MODEM_LORA) ? radio.getSNR() : 0.0f;
  stats.lastStation = station;
  rxEncrypted = (flags & VOICE_FLAG_ENCRYPTED) != 0;
  rxLastMs = millis();
  ledPacketFlash();
}

static void startListening() {
  // Nothing to listen to on the self test preset, and startReceive() would
  // undo the standby that applyPreset() just put the chip into.
  if (linkCurrentPreset().kind != PRESET_RADIO) return;

  const int16_t state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[link] startReceive failed: %d\n", state);
  }
}

static void linkTask(void* arg) {
  (void)arg;
  startListening();

  TxPacket pkt;
  uint32_t txStartedMs = 0;

  for (;;) {
    uint32_t bits = 0;
    // A one-second ceiling rather than portMAX_DELAY. Nothing depends on the
    // timeout, but it means the duty-cycle buckets keep advancing on a quiet
    // channel, and a lost DIO0 edge unsticks itself within a second instead of
    // wedging the radio until the next button press.
    xTaskNotifyWait(0, ULONG_MAX, &bits, pdMS_TO_TICKS(1000));

    // ---- preset change -----------------------------------------------------
    if (bits & NOTIFY_PRESET) {
      const uint8_t want = requestedPreset;
      if (want != presetIndex) {
        radio.standby();
        const VoicePreset& p = linkPreset(want);
        const int16_t state = applyPreset(p);
        if (state == RADIOLIB_ERR_NONE) {
          presetIndex = want;
          if (p.kind == PRESET_SELFTEST) {
            Serial.printf("[link] preset %u: %s - radio parked\n",
                          (unsigned)want, p.label);
          } else {
            Serial.printf("[link] preset %u: %s, %.3f MHz, %d dBm, "
                          "%lu ms air, %u%% keyed, limit %.0f%%\n",
                          (unsigned)want, p.label, (double)p.freqMHz,
                          (int)p.powerDbm, (unsigned long)airtimeMs,
                          (unsigned)linkKeyedDutyPercent(), (double)p.dutyLimit);
          }
        } else {
          Serial.printf("[link] preset %u failed to apply: %d\n",
                        (unsigned)want, state);
        }
        // Whatever was in flight on the old preset is gone, and anything
        // buffered from it is about to sound like a ghost. That goes for the
        // outbound queue too: packets encoded before the change belong to a
        // transmission the other end stopped being able to hear the moment we
        // retuned, and sending them on the new preset would put a fragment of
        // the last over on top of whatever is happening now.
        xQueueReset(rxQueue);
        xQueueReset(txQueue);
        rxStreamKnown = false;
        txInProgress = false;
        ledTransmitting(false);
        startListening();
      }
    }

    // ---- the radio finished something --------------------------------------
    bool justFinishedTx = false;
    if (bits & NOTIFY_DIO0) {
      if (txInProgress) {
        // TxDone. The interrupt tells us the chip is finished; it does not say
        // finished with what, so which of the two meanings DIO0 has is decided
        // by what we last asked the radio to do.
        radio.finishTransmit();
        txInProgress = false;
        justFinishedTx = true;
        const uint32_t onAir = millis() - txStartedMs;
        stats.txAirtimeMs += onAir;
        dutyAdd(linkCurrentPreset().band, onAir);
        stats.packetsTx++;
      } else {
        handleRx();
        // SX127x stays in continuous receive after a packet, but re-arming is
        // cheap and covers the case where readData() errored out and left the
        // chip in standby.
        startListening();
      }
    }

    // ---- anything to send? -------------------------------------------------
    // Sending always wins over listening: this is a half-duplex handset and
    // the operator has decided it is their turn.
    if (!txInProgress) {
      if (xQueueReceive(txQueue, &pkt, 0) == pdTRUE) {
        const int16_t state = radio.startTransmit(pkt.data, pkt.len);
        if (state == RADIOLIB_ERR_NONE) {
          txInProgress = true;
          txStartedMs = millis();
          ledTransmitting(true);
        } else {
          Serial.printf("[link] startTransmit failed: %d\n", state);
          ledTransmitting(false);
          startListening();
        }
      } else if (justFinishedTx) {
        // The queue has run dry, so that was the last packet of the over. Back
        // to listening immediately - the other station may already be keying
        // up, and every millisecond spent in standby is a word lost.
        ledTransmitting(false);
        startListening();
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Station identity.
//
// Derived from the factory MAC rather than configured, so two boards flashed
// from the same image come up distinguishable without anybody having to
// remember to change a build flag before the second upload.
//
// The two low bytes are XORed and folded into 1-254. Zero is reserved as "not
// set" and never produced. Collisions between two specific boards are possible
// but unlikely, and VOICE_STATION_ID pins it if you care.
// -----------------------------------------------------------------------------
static uint8_t deriveStationId() {
#ifdef VOICE_STATION_ID
  return (uint8_t)VOICE_STATION_ID;
#else
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  return (uint8_t)(1 + ((mac[4] ^ mac[5]) % 254));
#endif
}

uint8_t linkStationId() { return stationId; }

bool linkBegin() {
  stationId = deriveStationId();

  txQueue = xQueueCreate(VOICE_TX_QUEUE_PACKETS, sizeof(TxPacket));
  rxQueue = xQueueCreate(VOICE_RX_QUEUE_FRAMES, VOICE_MAX_BYTES_PER_FRAME);
  if (txQueue == nullptr || rxQueue == nullptr) {
    Serial.println("[link] out of memory creating queues");
    return false;
  }

  // The board does not use the default VSPI mapping, so pass the real pins.
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

  // Probe with a radio preset even if the default is the self test, otherwise
  // a board booted on preset 7 would never find out its radio is dead.
  const VoicePreset& boot = linkPreset(presetIndex);
  const VoicePreset& probe = (boot.kind == PRESET_RADIO) ? boot : linkPreset(0);

  int16_t state = applyPreset(probe);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[link] SX1276 init failed, RadioLib code %d\n", state);
    Serial.println("[link] check SPI wiring: SCK 5, MISO 19, MOSI 27, CS 18, RST 23");
    return false;
  }
  if (&probe != &boot) applyPreset(boot);

  Serial.printf("[link] SX1276 up, station id %u\n", (unsigned)stationId);
  Serial.println("[link] presets:");
  for (uint8_t i = 0; i < VOICE_PRESET_COUNT; i++) {
    const VoicePreset& p = VOICE_PRESETS[i];
    if (p.kind != PRESET_RADIO) {
      Serial.printf("[link]   %u  %-12s  %s\n", (unsigned)i, p.label, p.note);
    } else {
      Serial.printf("[link]   %u  %-12s  %.3f MHz  %2d dBm  limit %5.1f%%  %s\n",
                    (unsigned)i, p.label, (double)p.freqMHz, (int)p.powerDbm,
                    (double)p.dutyLimit, p.note);
    }
  }

  const uint32_t audioMs = codecFrameMs() * VOICE_FRAMES_PER_PACKET;
  Serial.printf("[link] active: %s, airtime %lu ms per %lu ms of audio = %u%% while keyed\n",
                boot.label, (unsigned long)airtimeMs, (unsigned long)audioMs,
                (unsigned)linkKeyedDutyPercent());

  if (boot.kind == PRESET_RADIO && linkKeyedDutyPercent() > 60) {
    // Not fatal, but it will not work well, and it will not be obvious why:
    // the symptom is the far end falling steadily further behind.
    Serial.println("[link] WARNING: over 60% duty while keyed - the transmitter cannot keep up");
    Serial.println("[link] lower the codec rate, or use an FSK preset; see config.h");
  }
  if (boot.kind == PRESET_RADIO && linkKeyedDutyPercent() > boot.dutyLimit) {
    Serial.printf("[link] NOTE: %u%% while keyed exceeds this band's %.0f%% limit; "
                  "about %lus of talking per hour\n",
                  (unsigned)linkKeyedDutyPercent(), (double)boot.dutyLimit,
                  (unsigned long)linkTalkSecondsLeft());
  }

  // Core 0. The audio task owns core 1, and the radio must not have to wait
  // behind a Codec2 frame to service a TxDone - that is dead air on a link
  // that is already running at a high duty cycle.
  const BaseType_t ok = xTaskCreatePinnedToCore(
      linkTask, "link", 4096, nullptr, 7, &linkTaskHandle, 0);
  if (ok != pdPASS) {
    Serial.println("[link] failed to start linkTask");
    return false;
  }
  return true;
}

void linkSetPreset(uint8_t index) {
  requestedPreset = index % VOICE_PRESET_COUNT;
  notifyTask(NOTIFY_PRESET);
}

uint8_t linkPresetIndex() { return presetIndex; }
const VoicePreset& linkCurrentPreset() { return linkPreset(presetIndex); }
bool linkSelfTestMode() { return linkCurrentPreset().kind == PRESET_SELFTEST; }

bool linkSetEncryption(bool on) {
  if (on && !cryptoKeyAvailable()) {
    Serial.printf("[link] cannot arm encryption: %s\n", cryptoKeyProblem());
    return false;
  }
  encryptionOn = on;
  Serial.printf("[link] encryption %s\n", on ? "ON" : "off");
  return true;
}

bool linkEncryption() { return encryptionOn; }

void linkStreamBegin() {
  // A fresh random stream id per press. This is what keeps the AES keystream
  // unique across transmissions - seq restarts at zero every time, so without
  // it the first packet of every over would use the same counter block and the
  // same key, and two of those XORed together give away both.
  txStreamId = esp_random();
  txSeq = 0;
}

bool linkSendFrames(const uint8_t* frames, uint8_t count, bool endOfStream) {
  if (count == 0 || count > VOICE_FRAMES_PER_PACKET) return false;

  // On the self test preset there is nowhere to send. voiceTask does not call
  // this there, but refusing here means a stray call cannot key a radio that
  // the operator believes is parked.
  if (linkSelfTestMode()) return false;

  TxPacket pkt;
  const size_t payload = (size_t)count * codecBytesPerFrame();
  pkt.len = (uint8_t)(VOICE_PKT_HEADER + payload);

  // Flags: codec id in the top nibble, end-of-stream and encrypted in the low
  // bits. All of it stays in clear - see the header note in link.h.
  uint8_t flags = (uint8_t)((codecId() << VOICE_FLAG_CODEC_SHIFT) & VOICE_FLAG_CODEC_MASK);
  if (endOfStream) flags |= VOICE_FLAG_END;

  const bool encrypt = encryptionOn && cryptoKeyAvailable();
  if (encrypt) flags |= VOICE_FLAG_ENCRYPTED;

  pkt.data[0] = VOICE_PKT_MAGIC;
  pkt.data[1] = flags;
  pkt.data[2] = (uint8_t)(txStreamId);
  pkt.data[3] = (uint8_t)(txStreamId >> 8);
  pkt.data[4] = (uint8_t)(txStreamId >> 16);
  pkt.data[5] = (uint8_t)(txStreamId >> 24);
  pkt.data[6] = (uint8_t)(txSeq);
  pkt.data[7] = (uint8_t)(txSeq >> 8);
  pkt.data[8] = stationId;
  memcpy(pkt.data + VOICE_PKT_HEADER, frames, payload);

  if (encrypt) {
    cryptoApply(pkt.data + VOICE_PKT_HEADER, payload, txStreamId, txSeq, stationId);
  }
  txSeq++;

  bool ok = true;
  if (xQueueSend(txQueue, &pkt, 0) != pdTRUE) {
    // Full. Drop the oldest rather than the newest: the queue only backs up
    // when the transmitter cannot keep up, and in that state the front of the
    // queue is the most stale audio in the system.
    TxPacket discard;
    xQueueReceive(txQueue, &discard, 0);
    xQueueSend(txQueue, &pkt, 0);
    stats.txDropped++;
    ok = false;
  }

  notifyTask(NOTIFY_TX);
  return ok;
}

bool linkTxBusy() {
  return txInProgress || uxQueueMessagesWaiting(txQueue) > 0;
}

void linkWaitTxIdle(uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while (linkTxBusy() && (int32_t)(millis() - deadline) < 0) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool linkTakeRxFrame(uint8_t* dst, uint32_t timeoutMs) {
  return xQueueReceive(rxQueue, dst, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint16_t linkRxFrameCount() {
  return (uint16_t)uxQueueMessagesWaiting(rxQueue);
}

void linkFlushRx() {
  xQueueReset(rxQueue);
  rxStreamKnown = false;
  rxEnded = false;
}

uint32_t linkLastRxMs()      { return rxLastMs; }
bool     linkRxWasEncrypted(){ return rxEncrypted; }
bool     linkRxStreamEnded() { return rxEnded; }
uint32_t linkAirtimeMs()     { return airtimeMs; }

const LinkStats& linkStats() { return stats; }
