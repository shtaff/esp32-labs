# 06 — Field checklist

**Status:** implemented (procedure) · Print this, or keep it on a phone.

A pair of handsets, checked in an order that **splits the problem in half at
every step**. Each stage adds exactly one thing to what has already been proved,
so the first stage that fails contains the fault. Do not skip ahead: a stage
that passes for the wrong reason costs more time than it saves.

Two columns because these are done per handset. Write the station id at the top
— you will want it when reading logs later.

---

## A · Bench, before leaving

### A1 — Power on

| # | Check | Pass looks like | A | B |
| --- | --- | --- | --- | --- |
| 1 | Board powers up | display lights within ~1 s | ☐ | ☐ |
| 2 | Boot banner | `=== LoRa digital voice handset ===` | ☐ | ☐ |
| 3 | Firmware version | `version` — **same on both boards** | ☐ | ☐ |
| 4 | Dirty flag | no `-dirty`, or you know why | ☐ | ☐ |
| 5 | No boot loop | `log` shows one `BOOT`, not several | ☐ | ☐ |

> **3 and 4 are not bureaucracy.** The most reliable way to lose an afternoon is
> comparing two boards that turn out to be running different code. `-dirty`
> means the git hash does not identify the source.

### A2 — POST

| # | Check | Pass looks like | A | B |
| --- | --- | --- | --- | --- |
| 6 | `post` runs | 8 items reported | ☐ | ☐ |
| 7 | No FAIL | `POST 8 pass`, or warnings you understand | ☐ | ☐ |
| 8 | Codec headroom | under ~40 % of frame budget | ☐ | ☐ |
| 9 | Battery | ≥ 3.6 V, or `no cell` if on USB | ☐ | ☐ |
| 10 | Memory | heap > 200 k, tightest stack > 4 kB | ☐ | ☐ |

> Expect `mic WARN` on a board with no microphone fitted — that is a supported
> configuration, not a fault. `amp pass` only means the ESP32 side is
> configured; the MAX98357A cannot be probed at all.

### A3 — Buttons

| # | Check | Pass looks like | A | B |
| --- | --- | --- | --- | --- |
| 11 | PTT press | display shows `TX` immediately | ☐ | ☐ |
| 12 | PTT release | returns to `idle`, LED off | ☐ | ☐ |
| 13 | MODE short | preset advances, flash message shows it | ☐ | ☐ |
| 14 | MODE double | `ENCRYPTED` / `CLEAR` flash | ☐ | ☐ |
| 15 | MODE long | screen changes | ☐ | ☐ |
| 16 | No stuck button | `log` has no unexpected `TX_START` | ☐ | ☐ |

> If 14 says `NO KEY`, that is the button working and the key missing. Go to B2.

### A4 — Self test (preset 7)

Press MODE until `SELF TEST`. This proves the **entire audio chain with the
radio in standby** — the single most valuable check in this document.

| # | Check | Pass looks like | A | B |
| --- | --- | --- | --- | --- |
| 17 | Key-up cue | short beep on PTT press | ☐ | ☐ |
| 18 | Recording | display counts up, VU bar moves | ☐ | ☐ |
| 19 | Playback | your voice, recognisable | ☐ | ☐ |
| 20 | End cue | falling two-tone at the end | ☐ | ☐ |
| 21 | No clipping | `stat` → `0 clipped samples` at normal volume | ☐ | ☐ |

> **17 without 19** = amplifier good, microphone or codec bad.
> **19 muffled/distorted** = check `config get micgain` and the clip count; see
> the mic section of the README.
> **Nothing at all** = amplifier, wiring, or `NO_AMP` build.

---

## B · Pair, before leaving

### B1 — Agreement

Both handsets must match on all four. Any mismatch and they cannot hear each
other — in three of the four cases, *silently*.

| # | Check | Where | A | B |
| --- | --- | --- | --- | --- |
| 22 | Preset **number** | MAIN screen, top-left | ☐ | ☐ |
| 23 | Codec mode | `stat` → `codec` | ☐ | ☐ |
| 24 | Protocol version | `version` → `protocol` | ☐ | ☐ |
| 25 | Key fingerprint | SYS screen — **only if using encryption** | ☐ | ☐ |

> A preset mismatch across *modems* (LoRa vs FSK) produces no reception at all
> and no error, because the receiver never demodulates anything. This is the
> single most common cause of "it doesn't work".

### B2 — Encryption

| # | Check | Pass looks like | A | B |
| --- | --- | --- | --- | --- |
| 26 | Key present | SYS shows a fingerprint, not `----` | ☐ | ☐ |
| 27 | Fingerprints match | identical four characters | ☐ | ☐ |
| 28 | Arms | double-press MODE → `ENC` on MAIN | ☐ | ☐ |
| 29 | Clear speech works | disarm both, talk | ☐ | ☐ |
| 30 | Encrypted speech works | arm both, talk | ☐ | ☐ |

> Set a key with `config set key <32 hex>` — it persists across reflashing.
> Generate one with `openssl rand -hex 16`.

### B3 — Link, close range

Keep the handsets a few metres apart, not touching.

| # | Check | Pass looks like | A | B |
| --- | --- | --- | --- | --- |
| 31 | A→B speech | B shows `RX`, plays A clearly | ☐ | ☐ |
| 32 | B→A speech | A shows `RX`, plays B clearly | ☐ | ☐ |
| 33 | Roger beep | falling two-tone when the talker releases | ☐ | ☐ |
| 34 | Station id | RX line shows the *other* board's id | ☐ | ☐ |
| 35 | RSSI sane | roughly −30 to −60 dBm at a few metres | ☐ | ☐ |
| 36 | No loss | `stat` → `lost 0` | ☐ | ☐ |
| 37 | No underruns | `stat` → `underruns 0` | ☐ | ☐ |
| 38 | Interrupt works | press PTT while receiving — it cuts over | ☐ | ☐ |

> **Hearing LOST instead of ROGER** means the tail packet is not arriving. At
> three metres that is a real fault, not marginal range.

---

## C · In the field

### C1 — Before the first over

| # | Check | | |
| --- | --- | --- | --- |
| 39 | Both on the intended preset, and you know its duty limit | ☐ | ☐ |
| 40 | `post` re-run after moving — battery reads differently under load | ☐ | ☐ |
| 41 | Antennas fitted **before** any transmission | ☐ | ☐ |

> **41 is not a formality.** Transmitting into an unterminated PA reflects power
> back into it. Do not key up an antenna-less board.

### C2 — Range walk

Walk out in steps, keying up at each. Record where it degrades.

| Distance | RSSI | Loss | Intelligible? | Notes |
| --- | --- | --- | --- | --- |
| | | | | |
| | | | | |
| | | | | |

Read from `stat` on the *listening* board:

- `signal` → RSSI, SNR (LoRa only — FSK has no SNR estimator)
- `packets` → `lost` climbing means you are at the edge
- `rejects` → `crc/read` climbing means marginal, not absent

> **Degradation order on this link:** loss counter rises → words drop → LOST
> instead of ROGER → nothing. If it goes straight from perfect to nothing,
> suspect a preset mismatch rather than range.

### C3 — Duty cycle

| # | Check | |
| --- | --- | --- |
| 42 | MAIN screen shows talk-time remaining this hour | ☐ |
| 43 | You know the active preset's limit (1 %, 10 % or 100 %) | ☐ |
| 44 | If it reaches 0, you stop | ☐ |

> The firmware **measures and displays; it does not enforce.** Presets 0–4 will
> exceed their legal duty cycle within a couple of minutes of conversation. See
> the README.

---

## D · After

| # | Action | |
| --- | --- | --- |
| 45 | `log raw` from both boards, saved to a file | ☐ |
| 46 | Note the version string alongside each log | ☐ |
| 47 | Note the station ids | ☐ |
| 48 | `stat` from both, for the final counters | ☐ |

> The log survives a reset but **not a power cycle**. Pull it before
> disconnecting the battery, or it is gone.

---

## Quick fault index

| Symptom | Most likely | Check |
| --- | --- | --- |
| Nothing received, no errors | preset mismatch | 22 |
| Received but noise | key mismatch | 25, 27 |
| `codec-mismatch` climbing | different Codec2 modes | 23 |
| `rxBadVersion` climbing | different firmware | 3, 24 |
| Voice muffled | mic capture — see README | 19, 21 |
| Beeps but no voice | mic or codec, amp fine | 17 vs 19 |
| LOST instead of ROGER | tail packet lost — range | 33 |
| Audio stutters | codec too slow, or preroll | 8, 37 |
| Board resets | `log` → `BOOT` reset reason | 5 |
| Nothing at all | POST | 6 |
