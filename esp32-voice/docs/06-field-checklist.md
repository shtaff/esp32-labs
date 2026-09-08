# 06 — Field checklist

Two handsets. Ordered so that **each stage adds exactly one thing** to what is
already proved — so the first stage that fails contains the fault. Don't skip
ahead.

Station id A: `____`  B: `____`  ·  Date: `________`

---

## A · Bench

| # | Check | Pass | A | B |
| --- | --- | --- | --- | --- |
| 1 | Powers on, display lit | within ~1 s | ☐ | ☐ |
| 2 | `version` — **same on both** | no `-dirty`, or you know why | ☐ | ☐ |
| 3 | `log` | one `BOOT`, not a loop | ☐ | ☐ |
| 4 | `post` | `POST 8 pass`, or warnings you understand | ☐ | ☐ |
| 5 | Battery | ≥ 3.6 V, or `no cell` on USB | ☐ | ☐ |
| 6 | PTT | `TX` on press, `idle` on release | ☐ | ☐ |
| 7 | MODE ×1 / ×2 / hold | preset / `ENC` / screen | ☐ | ☐ |

> **2 matters more than it looks.** The most reliable way to lose an afternoon
> is comparing two boards running different code.
>
> `mic WARN` is fine on a board with no microphone — that's a supported setup.

## B · Self test — preset 7

Proves the **whole audio chain with the radio in standby**. The single most
valuable check here.

| # | Check | Pass | A | B |
| --- | --- | --- | --- | --- |
| 8 | Key-up beep on PTT | short blip | ☐ | ☐ |
| 9 | Recording | counter climbs, VU moves | ☐ | ☐ |
| 10 | Playback | your voice, recognisable | ☐ | ☐ |
| 11 | `stat` → clipped | `0` at normal volume | ☐ | ☐ |

> **8 without 10** → amp good, microphone or codec bad.
> **10 muffled** → check `micgain` and the clip count.
> **Nothing at all** → amp, wiring, or a `NO_AMP` build.

## C · Pair

Four things must match. Three of them fail **silently**.

| # | Check | Where | A | B |
| --- | --- | --- | --- | --- |
| 12 | Preset **number** | MAIN, top-left | ☐ | ☐ |
| 13 | Codec mode | `stat` → `codec` | ☐ | ☐ |
| 14 | Protocol version | `version` | ☐ | ☐ |
| 15 | Key fingerprint | SYS — if using encryption | ☐ | ☐ |

> A preset mismatch **across modems** (LoRa vs FSK) gives no reception and no
> error at all, because nothing ever demodulates. Most common cause of "it
> doesn't work".

## D · Link, close range

A few metres apart, not touching.

| # | Check | Pass | |
| --- | --- | --- | --- |
| 16 | A→B and B→A speech | `RX` shown, clear audio | ☐ |
| 17 | Roger beep on release | falling two-tone, not the low double blip | ☐ |
| 18 | Station id on RX | shows the *other* board | ☐ |
| 19 | RSSI | −30 to −60 dBm at a few metres | ☐ |
| 20 | `stat` | `lost 0`, `underruns 0` | ☐ |
| 21 | PTT interrupts RX | cuts over immediately | ☐ |

> **LOST instead of ROGER at three metres** is a real fault, not marginal range.

## E · Field

| # | Check | |
| --- | --- | --- |
| 22 | **Antennas fitted before any transmission** | ☐ |
| 23 | Both on the intended preset, limit known | ☐ |
| 24 | `post` re-run — battery reads differently under load | ☐ |
| 25 | MAIN shows talk-time remaining; you stop at 0 | ☐ |

> **22 is not a formality.** Transmitting into an unterminated PA reflects power
> back into it.
>
> The firmware **measures** duty cycle; it does not enforce. Presets 0–4 exceed
> their legal limit within a couple of minutes of conversation.

### Range walk

| Distance | RSSI | Lost | Intelligible? |
| --- | --- | --- | --- |
| | | | |
| | | | |
| | | | |

> Degradation order: `lost` rises → words drop → LOST instead of ROGER →
> nothing. **Straight from perfect to nothing means preset mismatch, not range.**

## F · After

| # | Action | |
| --- | --- | --- |
| 26 | `log raw` from both, saved with the version string | ☐ |
| 27 | `stat` from both, for the final counters | ☐ |

> The RTC ring survives a reset but not a power cut. The flash log survives
> both — `log flush` before pulling a battery, or just `reboot`, which flushes.

---

## Fault index

| Symptom | Likely | Check |
| --- | --- | --- |
| Nothing received, no errors | preset mismatch | 12 |
| Received but noise | key mismatch | 15 |
| `codec-mismatch` climbing | different Codec2 modes | 13 |
| `rxBadVersion` climbing | different firmware | 2, 14 |
| Voice muffled | mic capture — see README | 10, 11 |
| Beeps but no voice | mic or codec; amp is fine | 8 vs 10 |
| LOST not ROGER | tail packet lost — range | 17 |
| Audio stutters | codec too slow, or preroll | 4, 20 |
| Range collapsed | on an FSK preset? that's the 17 dB | 12 |
| Board resets | `log` → `BOOT` reset reason | 3 |
| Nothing at all | POST | 4 |
