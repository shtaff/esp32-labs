# 05 — Over-the-air protocol specification

**Version:** 1 · **Status:** implemented · **Files:** [`include/link.h`](../include/link.h), [`src/link.cpp`](../src/link.cpp)

This is the normative description of what goes on the air. Two handsets
interoperate if and only if they agree on everything in this document.

---

## 1. Scope

Defines the framing, field layout, sequencing and cryptographic construction of
the voice packet carried between two `esp32-voice` handsets.

Out of scope: the modem parameters (a property of the *preset*, see the README),
the audio codec's own bitstream (defined by Codec2), and the physical layer's
preamble, sync word and CRC (supplied by the SX1276 and configured by the
preset).

### 1.1 Conventions

- All multi-byte integers are **little-endian**.
- Bit 0 is the least significant bit.
- "MUST", "MUST NOT" and "MAY" are used in the usual sense..

---

## 2. Versioning

```c
#define VOICE_PROTO_VERSION 1
```

This number changes **only when the bytes on the air change**. It moves
independently of the firmware version and the config version — see
[`01-version.md`](01-version.md).

| Change | Action |
| --- | --- |
| Add/remove/resize a header field | increment |
| Change the meaning of an existing field | increment |
| Change the counter-block construction | increment |
| Add a flag bit from the reserved pool | **no** increment (see §4.1) |
| Change modem parameters, codec mode, frames per packet | **no** increment — those are preset and build settings, not protocol |

**A receiver MUST reject any packet whose version it does not implement**, and
MUST count the rejection separately from other rejects. It MUST NOT attempt to
interpret the remaining fields: a packet from a future protocol has fields that
would be misread, and misreading them means decoding noise into somebody's ear
rather than declining politely.

There is no version negotiation and there cannot be one — a half-duplex PTT link
has no back-channel. Mismatch is reported, not resolved:

```
rejects   crc/read 0  foreign 0  codec-mismatch 0  malformed 0
          14 packets used a protocol version this build does not speak (we send v1)
```

---

## 3. Frame

One protocol packet is carried in exactly one physical-layer packet. There is no
fragmentation and no reassembly.

```
        ┌────────────────── 10-byte header, always in clear ──────────────────┐
byte:   0        1        2        3  4  5  6      7  8      9      10 ... N
      ┌────────┬────────┬────────┬──────────────┬────────┬────────┬──────────┐
      │ magic  │version │ flags  │   streamId   │  seq   │station │  frames  │
      │  0x56  │   1    │        │   uint32     │ uint16 │ uint8  │          │
      └────────┴────────┴────────┴──────────────┴────────┴────────┴──────────┘
                                                                  └ encrypted ┘
                                                                    when armed
```

Total length = `10 + (frames × bytes_per_codec_frame)`.

At the default configuration (Codec2 1600, 6 frames/packet) that is
**10 + 48 = 58 bytes**.

### 3.1 Size constraint

Total packet length **MUST** be ≤ 63 bytes. The SX1276's FSK FIFO is 64 bytes
and RadioLib will not split a packet across refills, so a longer packet is
refused at runtime *on the FSK presets only* — a miserable way to discover it.
`link.cpp` carries a `static_assert` to catch it at compile time instead.

---

## 4. Header fields

### 4.0 `magic` — offset 0, `uint8`, value `0x56` (`'V'`)

A one-byte filter applied before anything else is trusted. 868 MHz is a shared
public band and a CRC-valid packet is not necessarily *ours*.

A receiver MUST discard any packet whose first byte is not `0x56` and count it
as foreign traffic.

### 4.1 `version` — offset 1, `uint8`

`VOICE_PROTO_VERSION`. Placed second so that it is the second thing checked and
every later field is interpreted knowing which layout it is in.

### 4.2 `flags` — offset 2, `uint8`

| Bits | Name | Meaning |
| --- | --- | --- |
| 0 | `ENCRYPTED` | payload is AES-128-CTR encrypted (§6) |
| 1 | `END` | last packet of this transmission (§5.3) |
| 2–3 | — | **reserved, MUST be transmitted as 0, MUST be ignored on receive** |
| 4–7 | `CODEC` | codec identity (§4.2.1) |

Bits 2–3 are the forward-compatibility budget: a receiver that ignores unknown
bits allows two of them to be added later without a version bump.

#### 4.2.1 Codec identity

| Value | Codec2 mode | Frame | Bytes/frame |
| --- | --- | --- | --- |
| 0 | 3200 | 20 ms | 8 |
| 1 | 1600 | 40 ms | 8 |
| 2 | 700C | 40 ms | 4 |
| 15 | unknown | — | — |

A receiver MUST discard a packet whose codec id differs from its own, and count
it separately. Decoding it would produce noise — and noise that sounds like a
broken radio rather than like a configuration mistake, which is why it gets its
own counter.

This is *not* part of the version: two handsets on different codec modes both
speak protocol v1 correctly, they simply have nothing to say to each other.

### 4.3 `streamId` — offset 3, `uint32`

A random value drawn per PTT press, from `esp_random()`.

It serves three purposes:

1. Identifies one transmission, so the receiver can tell a continuing stream
   from a new one.
2. A change in value MUST cause the receiver to flush its jitter buffer —
   anything still buffered belongs to a transmission that is over, and playing
   it would put the end of the last over on top of the start of this one.
3. Contributes to the AES counter block (§6).

### 4.4 `seq` — offset 7, `uint16`

Packet counter within the stream, starting at 0 and incrementing by 1.

A receiver detects loss as the forward distance from the expected value:

```c
gap = (uint16_t)(received_seq - expected_seq);   // correct across the wrap
```

`seq` MUST NOT wrap within a single transmission. At 240 ms per packet a 16-bit
counter covers 4.4 hours of continuous talking, which is longer than the
battery lasts and far longer than `txmax` allows.

### 4.5 `station` — offset 9, `uint8`

Who is talking. Range 1–254; 0 is reserved and MUST NOT be transmitted.

Derived from the board's factory MAC unless configured (`config set station`).
Also contributes to the counter block (§6.2).

### 4.6 `frames` — offset 10, variable

`N` back-to-back Codec2 frames, where `N = (packet_length - 10) /
bytes_per_codec_frame`.

A receiver MUST discard the packet if `(packet_length - 10)` is not a positive
whole multiple of its own codec's frame size, and count it as malformed.

`N` is not transmitted because it is derivable, and a field that can be derived
is a field that can disagree with reality.

---

## 5. Behaviour

### 5.1 Transmission

On PTT press the transmitter draws a new `streamId`, resets `seq` to 0, and
emits packets of exactly `VOICE_FRAMES_PER_PACKET` frames as the encoder fills
them.

### 5.2 Half duplex

A station MUST NOT transmit while receiving. PTT takes priority over an incoming
stream — the operator has decided it is their turn, exactly as on every other
walkie-talkie.

### 5.3 End of transmission

The final packet of a transmission MUST set `END`. If the release lands exactly
on a packet boundary, the transmitter MUST emit one further packet containing a
single frame of encoded silence, carrying `END`.

This costs one packet and is what lets a receiver distinguish:

| Receiver sees | Means |
| --- | --- |
| packet with `END` | the far end signed off — play out the buffer and idle |
| silence past the timeout, no `END` | the far end **vanished** — the link failed |

The receiver signals that difference audibly (ROGER vs LOST cue). It cannot be
signalled by a transmitted tone, because a tone that fails to arrive is
indistinguishable from a station still talking. See
[`README`](../README.md#cue-tones).

### 5.4 Stream timeout

A receiver MUST treat a stream as ended after
`VOICE_RX_STREAM_TIMEOUT_MS` (1200 ms) with no accepted packet. This is
comfortably longer than one packet interval, so a single lost packet does not
close the squelch.

### 5.5 Receiver rejection order

Checks MUST be applied in this order, each with its own counter:

1. Physical-layer CRC (by the modem) → `rxErrors`
2. `magic` → `rxForeign`
3. `version` → `rxBadVersion`
4. codec id → `rxCodecMismatch`
5. payload length modulo frame size → `rxMalformed`
6. `ENCRYPTED` set but no key held → `rxNoKey`

The order matters: it is cheapest-and-most-fundamental first, and it means the
counter that increments identifies the *most specific* thing that was wrong.

---

## 6. Cryptography

### 6.1 Cipher

AES-128 in counter (CTR) mode, applied to the `frames` region only. The header
is never encrypted.

The header stays in clear because the receiver needs `streamId`, `seq` and
`station` to construct the counter block before it can decrypt anything, and
because `magic`, `version` and the codec id must be checkable on a packet that
is not for us.

This leaks who is talking to whom, for how long, and how often. On a band where
the preamble itself is a detectable event, that was never hidden anyway.

### 6.2 Counter block

```
byte:  0   1   2   3   4   5   6   7 ............ 15
     ┌───────────────┬───────┬───┬─────────────────┐
     │   streamId    │  seq  │st │  zero (9 bytes) │
     └───────────────┴───────┴───┴─────────────────┘
```

The block is incremented by the cipher once per 16 bytes of payload. A packet
carries at most 48 bytes, consuming three counter values, so it cannot run into
the space any other packet's block would occupy.

Keystream uniqueness rests on `(streamId, seq, station)` never repeating, and
each of the three covers a distinct failure:

| Field | Prevents |
| --- | --- |
| `seq` | one packet reusing another's keystream **within a transmission** |
| `streamId` | one transmission reusing the previous one's |
| `station` | **two different handsets** colliding |

The third is not theoretical. Two stations key up independently and draw
`streamId` with no knowledge of each other, so roughly one time in 2³² they
choose the same value — and both start at `seq` 0. Without the station byte that
hands an eavesdropper two packets under identical keystream, which XOR together
to reveal both plaintexts. With it, stations that differ can never collide.

### 6.3 What this does not provide

- **No authentication.** CTR is malleable. An attacker can flip bits in the
  payload and the receiver will decode the result and play it. The physical CRC
  catches accidental corruption, not deliberate corruption.
- **No key exchange, no forward secrecy.** One static key, shared out of band.
- **No replay protection.** A recorded packet replays.
- **No header protection.** All metadata is in clear.

The claim is exactly one thing: a passive listener with an SDR and a matching
receiver hears noise instead of the conversation.

### 6.4 Key

128 bits, shared out of band, identical on both handsets. An all-zero key MUST
be refused — it is a published key. Both the config store and the crypto layer
enforce this independently.

Handsets are matched by a **fingerprint**: `AES-128-ECB(key, 0^128)` truncated to
its first two bytes, rendered as four hex characters, shown on the SYS screen.
Two boards showing the same fingerprint hold the same key. It leaks 16 bits
about the key, which against an attacker who can already read the flash is not
the weak point.

---

## 7. Worked example

Codec2 1600, 6 frames, encryption off, station 42, stream `0xDEADBEEF`,
packet 3 of a transmission, not the last:

```
56 01 10 EF BE AD DE 03 00 2A | <48 bytes of Codec2>
│  │  │  └──streamId───┘ └seq┘ └station
│  │  └ flags: codec 1 (<<4) = 0x10, no END, not encrypted
│  └ protocol version 1
└ magic 'V'
```

Total 58 bytes.

---

## 8. Version history

| Version | Change |
| --- | --- |
| 1 | Initial. 10-byte header with explicit version, station id in the header and in the counter block. |

### Predecessor (unnumbered, pre-release)

A 9-byte header with no `version` field existed during development. It is not
assigned a protocol version and is not interoperable — it will be rejected as
`rxForeign` or `rxBadVersion` depending on what its second byte happened to be.
Reflash both handsets.
