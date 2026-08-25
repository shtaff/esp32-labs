// =============================================================================
// crypto.h - AES-128-CTR over the codec frames of a packet.
//
// Read the encryption section of config.h before relying on any of this. In
// one line: it hides the conversation from a passive listener and does nothing
// else. No authentication, no key exchange, no replay protection.
//
// The counter block is built from the packet header, which both ends already
// have in clear:
//
//     byte   0-3   streamId   random per PTT press
//     byte   4-5   seq        packet counter within the stream
//     byte   6     station    this board's identity
//     byte   7-15  zero, then incremented per 16-byte block by mbedtls
//
// Keystream is therefore unique as long as (streamId, seq, station) never
// repeats. Each of the three covers a different way that could happen:
//
//   seq       stops one packet reusing another packet's keystream inside a
//             single transmission. Reset per stream, and it cannot wrap inside
//             one - at 240 ms per packet a 16-bit seq covers four and a half
//             hours of continuous talking.
//   streamId  stops one transmission reusing the previous one's. It comes from
//             esp_random(), which is a true hardware RNG once the RF clock is
//             running, so every press of PTT starts somewhere new.
//   station   stops two DIFFERENT handsets colliding. Two boards keying up
//             independently draw streamIds with no knowledge of each other, so
//             about one time in four billion they pick the same one - and both
//             start at seq 0. Without this byte that hands an eavesdropper two
//             packets encrypted under identical keystream, which XOR together
//             to give both plaintexts. With it, boards that differ can never
//             collide at all.
// =============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// Parses VOICE_KEY_HEX. Returns false if it is not 32 hex characters, or if it
// is all zero - the "not configured" sentinel from secrets.ini.example. A
// false here is not fatal: the handset runs in clear and refuses to arm.
bool cryptoBegin();
bool cryptoKeyAvailable();

// Why the key was rejected, for the boot log and the display. "" when fine.
const char* cryptoKeyProblem();

// Four hex characters derived from the key, and from nothing else.
//
// Two handsets that show the same fingerprint hold the same key. That is the
// only way to tell, short of trying to talk, and a wrong key sounds exactly
// like a wrong channel or a dead radio - which is a miserable thing to debug.
// It leaks 16 bits about the key; against an attacker who can already read the
// flash, that is not the weak point.
const char* cryptoFingerprint();

// Encrypts or decrypts in place - CTR is its own inverse, so the sender and
// the receiver make the identical call. The three identifiers must be the ones
// from the packet header, or the counter block will not match and the result
// is noise.
//
// Safe to call with no key configured, in which case it does nothing and
// returns false, so a caller that forgot to check cannot silently transmit
// plaintext believing it was encrypted.
bool cryptoApply(uint8_t* data, size_t len,
                 uint32_t streamId, uint16_t seq, uint8_t station);
