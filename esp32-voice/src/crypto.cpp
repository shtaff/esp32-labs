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

static mbedtls_aes_context aes;
static bool  keyOk = false;
static char  problem[48] = "";
static char  fingerprint[8] = "----";

// One hex character to its value, or -1 if it is not hex. Used instead of
// strtol so that a malformed key is reported at the character that broke it
// rather than silently parsing as far as it can and zero-filling the rest.
static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool cryptoBegin() {
  const char* hex = VOICE_KEY_HEX;
  uint8_t key[VOICE_KEY_BYTES];

  if (strlen(hex) != VOICE_KEY_BYTES * 2) {
    snprintf(problem, sizeof(problem), "key is %u hex chars, want %u",
             (unsigned)strlen(hex), (unsigned)(VOICE_KEY_BYTES * 2));
    Serial.printf("[crypto] %s\n", problem);
    return false;
  }

  // Parse 32 hex characters into 16 bytes, ORing everything together as we go
  // so the all-zero test below costs nothing extra.
  uint8_t orAll = 0;
  for (int i = 0; i < VOICE_KEY_BYTES; i++) {
    int hi = hexNibble(hex[i * 2]);
    int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      snprintf(problem, sizeof(problem), "key is not hex at char %d", i * 2);
      Serial.printf("[crypto] %s\n", problem);
      return false;
    }
    key[i] = (uint8_t)((hi << 4) | lo);
    orAll |= key[i];
  }

  // All zero is the sentinel from platformio.ini, meaning nobody has created a
  // secrets.ini yet. Refusing it is the point: an all-zero AES key is a
  // published key, and a handset that showed ENC while using one would be
  // lying about the only thing encryption is for.
  if (orAll == 0) {
    snprintf(problem, sizeof(problem), "no key in secrets.ini");
    Serial.println("[crypto] key is all zero - see secrets.ini.example");
    Serial.println("[crypto] encryption cannot be armed; the link runs in clear");
    return false;
  }

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
