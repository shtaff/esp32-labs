// =============================================================================
// crypto.cpp - AES-128-CTR, keyed from a build-time constant.
//
// mbedtls ships with the ESP-IDF, and on the ESP32 its AES calls land on the
// hardware accelerator - the symbol that actually gets linked here is
// esp_aes_crypt_ctr, not a software implementation. A 48-byte packet is three
// AES blocks and costs a few microseconds, which is why encryption can be
// toggled mid-transmission without anything noticing.
//
// Read the encryption section of config.h before trusting any of this. The
// short version: it hides the conversation from a passive listener and does
// nothing else at all.
// =============================================================================
#include "crypto.h"

#include <Arduino.h>
#include <string.h>
#include <mbedtls/aes.h>

#include "config.h"
#include "configstore.h"
#include "log.h"

static mbedtls_aes_context aes;
static bool  keyOk = false;
static char  problem[48] = "";
static char  fingerprint[8] = "----";

// Hex parsing lives in configstore.cpp now, along with the range checking for
// every other setting. This file only ever sees 16 raw bytes.

bool cryptoBegin() {
  // The key comes from the config store and nowhere else.
  //
  // That is the single path on purpose: configBegin() parses VOICE_KEY_HEX
  // from secrets.ini into the defaults, and anything set later with
  // `config set key` replaces it there. Reading secrets.ini again here would
  // give two sources of truth that disagree the moment somebody changes the
  // key over the serial line - and the disagreement would present as "the
  // other handset cannot hear me", which is a long way from its cause.
  //
  // The all-zero rejection now lives in two places, and both are load-bearing:
  // configstore.cpp refuses to STORE one, and the keySet flag below refuses to
  // USE one. An all-zero AES key is a published key, and a handset that showed
  // ENC while using one would be lying about the only thing encryption is for.
  const VoiceConfig& cfg = config();

  if (!cfg.keySet) {
    snprintf(problem, sizeof(problem), "no key configured");
    Serial.println("[crypto] no key: put one in secrets.ini, or run");
    Serial.println("[crypto]   config set key <32 hex characters>");
    Serial.println("[crypto] encryption cannot be armed; the link runs in clear");
    logWarn(LOG_MOD_CRYPTO, LOG_EV_CRYPTO_KEY, 0, 0);
    return false;
  }

  uint8_t key[VOICE_KEY_BYTES];
  memcpy(key, cfg.key, sizeof(key));

  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, VOICE_KEY_BYTES * 8) != 0) {
    snprintf(problem, sizeof(problem), "mbedtls rejected the key");
    Serial.printf("[crypto] %s\n", problem);
    mbedtls_aes_free(&aes);
    return false;
  }

  // Fingerprint: AES of a fixed all-zero block under this key, first two
  // bytes. Deterministic, needs no extra primitive, and reveals nothing an
  // attacker who can encrypt a chosen block could not work out anyway.
  uint8_t zero[16] = {0};
  uint8_t out[16];
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, zero, out);
  snprintf(fingerprint, sizeof(fingerprint), "%02X%02X", out[0], out[1]);

  keyOk = true;
  problem[0] = '\0';
  Serial.printf("[crypto] AES-128 key loaded, fingerprint %s\n", fingerprint);
  Serial.println("[crypto] both handsets must show the same fingerprint");
  // The fingerprint goes in the log as a number so a dump can prove which
  // key a board was carrying without recording the key itself.
  logInfo(LOG_MOD_CRYPTO, LOG_EV_CRYPTO_KEY, 1,
          (int32_t)((out[0] << 8) | out[1]));
  return true;
}

bool        cryptoKeyAvailable() { return keyOk; }
const char* cryptoKeyProblem()   { return problem; }
const char* cryptoFingerprint()  { return keyOk ? fingerprint : "----"; }

bool cryptoApply(uint8_t* data, size_t len,
                 uint32_t streamId, uint16_t seq, uint8_t station) {
  if (!keyOk || len == 0) return false;

  // The counter block. All three identifiers come straight out of the packet
  // header, so the receiver builds exactly the same block from what it can
  // see. The remaining nine bytes start at zero and mbedtls increments the
  // whole block once per 16 bytes of payload - a packet carries at most 48
  // bytes, so it consumes three counter values and cannot run into the space
  // any other packet's block would occupy.
  uint8_t nonce[16] = {0};
  nonce[0] = (uint8_t)(streamId);
  nonce[1] = (uint8_t)(streamId >> 8);
  nonce[2] = (uint8_t)(streamId >> 16);
  nonce[3] = (uint8_t)(streamId >> 24);
  nonce[4] = (uint8_t)(seq);
  nonce[5] = (uint8_t)(seq >> 8);
  nonce[6] = station;

  // mbedtls_aes_crypt_ctr keeps a partial-block offset across calls; we always
  // start a fresh stream per packet, so it is zero every time and the stream
  // block buffer is scratch.
  size_t   ncOff = 0;
  uint8_t  streamBlock[16];

  int rc = mbedtls_aes_crypt_ctr(&aes, len, &ncOff, nonce, streamBlock,
                                 data, data);
  return rc == 0;
}
