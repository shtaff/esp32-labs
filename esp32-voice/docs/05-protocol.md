# 05 — Over-the-air protocol

**Protocol version 1** · [`include/link.h`](../include/link.h), [`src/link.cpp`](../src/link.cpp)

Normative. Two handsets interoperate if and only if they agree on everything
here. All multi-byte integers are **little-endian**; bit 0 is least significant.

---

## 1. Transport and frame structure

| | |
| --- | --- |
| Carrier | SX1276, 868 MHz, LoRa or FSK — a property of the *preset*, not the protocol |
| Framing | supplied by the modem: preamble, sync word, length byte (FSK), CRC-16 |
| Mapping | **one protocol packet = one physical packet.** No fragmentation, no reassembly |
| Direction | half duplex. A station never transmits while receiving |

The modem's CRC is the only integrity check; the protocol adds none of its own.
A packet that fails it is discarded by the chip and never reaches this layer.

```
        ┌──────────── 10-byte header, always in clear ────────────┐
byte    0        1        2        3 4 5 6      7 8      9        10 … N
      ┌────────┬────────┬────────┬──────────┬────────┬────────┬──────────┐
      │ magic  │version │ flags  │ streamId │  seq   │station │  frames  │
      └────────┴────────┴────────┴──────────┴────────┴────────┴──────────┘
                                                              └ encrypted ┘
```

`length = 10 + (N × bytes_per_codec_frame)`. Default configuration
(Codec2 1600, 6 frames): **58 bytes**.

The header is never encrypted. The receiver needs `streamId`, `seq` and
`station` to build the AES counter block before it can decrypt anything, and
`magic`, `version` and the codec id must be checkable on a packet that turns
out not to be ours.

---

## 2. Field table

| Offset | Size | Type | Byte order | Field | Notes |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 1 | `uint8` | — | `magic` | `0x56` (`'V'`). Constant |
| 1 | 1 | `uint8` | — | `version` | `VOICE_PROTO_VERSION`, currently 1 |
| 2 | 1 | bitfield | — | `flags` | see below |
| 3 | 4 | `uint32` | little-endian | `streamId` | random per PTT press |
| 7 | 2 | `uint16` | little-endian | `seq` | packet counter, from 0 |
| 9 | 1 | `uint8` | — | `station` | 1–254; 0 reserved |
| 10 | N×F | opaque | — | `frames` | N Codec2 frames of F bytes |

### `flags` bits

| Bits | Name | Meaning |
| --- | --- | --- |
| 0 | `ENCRYPTED` | payload is AES-128-CTR encrypted |
| 1 | `END` | last packet of this transmission |
| 2–3 | reserved | **transmit as 0, ignore on receive** — two spare bits that can be used later without a version bump |
| 4–7 | `CODEC` | 0 = Codec2 3200, 1 = 1600, 2 = 700C, 15 = unknown |

### Constraints

- `magic` MUST be `0x56`, else discard as foreign traffic.
- `version` MUST match, else discard (§4).
- `CODEC` MUST match the receiver's own, else discard — decoding another
  bitrate produces noise that sounds like a broken radio rather than a
  configuration mistake.
- `station` MUST NOT be 0.
- `seq` MUST NOT wrap within one transmission. At 240 ms/packet a 16-bit
  counter covers 4.4 hours — longer than the battery and far longer than
  `txmax` permits.
- `(length − 10)` MUST be a positive whole multiple of the receiver's codec
  frame size, else discard as malformed.
- `N` is **not transmitted** because it is derivable, and a field that can be
  derived is a field that can disagree with reality.

---

## 3. Message types and payload

**There is one message type.** No control frames, no handshake, no discovery —
adding any would need a back-channel that a half-duplex PTT link does not have.

Everything that would otherwise be a control message is carried as a flag on a
voice packet:

| Meaning | Encoding |
| --- | --- |
| start of transmission | a new `streamId` |
| continuation | same `streamId`, `seq` + 1 |
| end of transmission | `END` set |
| who is talking | `station` |
| audio format | `CODEC` in `flags` |

### Payload

`N` back-to-back Codec2 frames, oldest first, no padding and no per-frame
header. Frame size follows from `CODEC`:

| `CODEC` | Mode | Frame duration | Bytes/frame | Default N | Audio/packet |
| --- | --- | --- | --- | --- | --- |
| 0 | 3200 | 20 ms | 8 | 6 | 120 ms |
| 1 | **1600** | 40 ms | 8 | **6** | **240 ms** |
| 2 | 700C | 40 ms | 4 | 6 | 240 ms |

When `ENCRYPTED` is set the whole payload region is encrypted as one run,
AES-128-CTR, counter block:

```
byte   0  1  2  3 │ 4  5 │ 6  │ 7 ........ 15
      └ streamId ┘└ seq ┘└stn┘└ zero (9)  ┘
```

Keystream uniqueness rests on `(streamId, seq, station)` never repeating —
`seq` prevents reuse within a transmission, `streamId` between transmissions,
`station` between two handsets that happen to draw the same random `streamId`
and both start at `seq` 0.

**No authentication, no replay protection, no key exchange.** CTR is malleable:
an attacker can flip payload bits and the receiver will play the result. The
claim is exactly one thing — a passive listener hears noise.

---

## 4. Versioning

```c
#define VOICE_PROTO_VERSION 1
```

Changes **only when the bytes on the air change**. Independent of the firmware
version and the config version.

| Change | Bump? |
| --- | --- |
| Add, remove or resize a header field | **yes** |
| Change the meaning of a field | **yes** |
| Change the counter-block construction | **yes** |
| Use one of the reserved flag bits | no |
| Change modem, codec mode, frames per packet | no — preset and build settings, not protocol |

A receiver MUST reject any packet whose version it does not implement, count it
separately, and MUST NOT interpret the remaining fields — a future layout would
be misread, and misreading it means decoding noise into somebody's ear.

**There is no negotiation and there cannot be one.** Mismatch is reported, not
resolved:

```
14 packets used a protocol version this build does not speak (we send v1)
```

---

## 5. Errors, ACK, retry and timeouts

### There are no acknowledgements and no retries

Deliberate, and worth being explicit about because their absence looks like an
omission:

- An ACK needs a back-channel. The link is half duplex; the far end is
  physically unable to answer while you are talking.
- A retry needs time. At 45 % duty while keyed there is none, and audio that
  arrives late is worthless — by the time a retransmission landed, its 240 ms
  of speech would be several hundred milliseconds stale.

**Loss is concealed, not corrected.** A gap in `seq` is counted; the receiver
inserts silence and continues.

### Rejection order

Checks are applied cheapest-and-most-fundamental first, so the counter that
increments names the *most specific* thing that was wrong:

| # | Check | Counter | Means |
| --- | --- | --- | --- |
| 1 | modem CRC | `rxErrors` | corruption — normal at range |
| 2 | `magic` | `rxForeign` | somebody else's traffic |
| 3 | `version` | `rxBadVersion` | upgrade a handset |
| 4 | `CODEC` | `rxCodecMismatch` | different build |
| 5 | length modulo frame size | `rxMalformed` | should not happen |
| 6 | `ENCRYPTED` with no key | `rxNoKey` | one end armed, one not |

### Timeouts

| Timeout | Value | Effect |
| --- | --- | --- |
| Stream end | 1200 ms with no accepted packet | close the stream, play the LOST cue |
| Pre-roll fill | 3 × preroll | start playback with what there is |
| Jitter buffer | 64 frames (2.5 s) | oldest frames dropped when full |
| Transmit cap | `txmax`, 0–300 s | stop transmitting (stuck-PTT guard) |

The stream timeout is longer than one packet interval on purpose, so a single
lost packet does not close the squelch.

### End of transmission

The last packet MUST set `END`. If the PTT release lands exactly on a packet
boundary, the transmitter MUST emit one more packet containing a single frame
of encoded silence, carrying `END`.

That one extra packet is what makes these two distinguishable:

| Receiver sees | Means | Cue |
| --- | --- | --- |
| packet with `END` | signed off | ROGER (falling two-tone) |
| silence past 1200 ms, no `END` | **vanished** | LOST (low double blip) |

A tone transmitted at the end could not do this — a tone that fails to arrive is
indistinguishable from a station still talking.

---

## 6. Worked example

Station 42, stream `0xDEADBEEF`, packet 3, Codec2 1600, clear, not the last:

```
56 01 10 EF BE AD DE 03 00 2A  1F 8C 43 A0 22 7E 91 04 …
```

| Offset | Bytes | Value | Meaning |
| ---: | --- | --- | --- |
| 0 | `56` | 0x56 | magic `'V'` — ours |
| 1 | `01` | 1 | protocol v1 — we speak it |
| 2 | `10` | `0001_0000` | bit0=0 clear, bit1=0 not last, bits4-7 = 1 → Codec2 1600 |
| 3 | `EF BE AD DE` | 0xDEADBEEF | streamId (LE) — same as last packet, so a continuation |
| 7 | `03 00` | 3 | seq (LE) — expected 3, so nothing lost |
| 9 | `2A` | 42 | station 42 |
| 10 | 48 bytes | — | 6 Codec2 frames × 8 bytes = 240 ms of audio |

Total **58 bytes**. With `ENCRYPTED` set, byte 2 would be `11` and bytes 10–57
would be ciphertext; the header would be byte-for-byte identical.

---

## 7. Limitations

### MTU

**63 bytes, hard.** The SX1276's FSK FIFO is 64 bytes and RadioLib will not
split a packet across refills, so anything larger is refused at runtime *on the
FSK presets only* — a miserable way to find out. A `static_assert` in
`link.cpp` catches it at compile time instead.

At 58 bytes the default leaves 5 bytes of headroom. Raising
`VOICE_FRAMES_PER_PACKET` to 7 would exceed it.

### Rate and latency

| | |
| --- | --- |
| Packet rate | one per 240 ms while keyed |
| Airtime | 108 ms (LoRa SF7/BW125) · 10 ms (FSK 50k) |
| Mouth-to-ear | ≈ 670 ms (240 packing + 108 air + 320 pre-roll) |
| PTT to open mic | ≈ 125 ms (key-up cue, then the INMP441 settle) |

### Duty cycle

| Preset | Keyed duty | ETSI limit | |
| --- | --- | --- | --- |
| 868.1–868.5 LoRa | 45 % | 1 % | **45× over** |
| 869.5 LoRa | 45 % | 10 % | 4.5× over |
| **869.5 FSK50** | **4.3 %** | **10 %** | **compliant** |
| 869.9 FSK50 | 4.3 % | 100 % | always legal, 5 mW |

Voice is not a 1 % application. The firmware **measures and displays** the
rolling hour per sub-band and shows the talk-time remaining; it does not
enforce. See the README.

### Other

- **No addressing.** `station` says who sent a packet; it does not select a
  recipient. Every handset on a preset hears every other.
- **No relaying**, no mesh, no store-and-forward. One hop.
- **Metadata is in clear** even when encrypted: who, when, how long, how often.
- **No time source.** `seq` orders packets within a stream; there is no
  absolute timestamp anywhere in the protocol.
- **Both handsets must agree** on preset, codec mode, protocol version and key.
  Three of those four fail *silently* — see the field checklist.
