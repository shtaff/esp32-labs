# 08 — Breaking the config on purpose

**Status: PLAN ONLY — not implemented.** This describes how to *attack* the
config store by cutting power during a write, and what to change if the attack
succeeds.

## The question

`config set` writes to NVS immediately. What happens if the board loses power,
crashes, or is reset **during** that write? Does it come back with the old
value, the new value, or something that is neither?

Any of those three is acceptable **as long as it is not the third**. Old or new
are both recoverable states. A corrupt blob that still passes validation is not.

## What is already in place

**Batched writes** (see [`03-config.md`](03-config.md)) came after this document
was first written and change its arithmetic. `config set a=1 b=2 c=3` is now a
single NVS write rather than three, so configuring a board from scratch exposes
**one** window instead of eight.

That reduces the exposure substantially and does not eliminate it — one write is
still one write, and it is still the write that this experiment is about.

| Defence | Catches |
| --- | --- |
| CRC-32 over the whole blob | a torn or partial write |
| `magic` word | a blob that is not ours at all |
| `size` field | a layout change |
| bounds check on every field at load | a value that survived the CRC but is nonsense |
| fall back to defaults, never refuse to boot | any of the above |

So the *predicted* worst case is: the blob fails CRC, POST reports
`config WARN: stored bytes failed CRC - corrupt`, and the board comes up on
build-time defaults having lost the operator's settings — including the key.

**That prediction is untested.** It also rests on an assumption about NVS that
this document exists to check.

## The assumption worth testing

ESP-IDF's NVS is a log-structured store with its own page-level CRCs and a
two-phase item write. It is *designed* to be power-fail safe: an interrupted
`nvs_set_blob` should leave the previous value intact, because the new item is
not marked valid until it is fully written.

If that holds, our CRC never fires and the settings survive. **The purpose of
the test is to find out whether it holds for a 44-byte blob written through the
Arduino `Preferences` wrapper, on this flash part, at this supply voltage** —
not to re-derive it from documentation.

The interesting failure is not "NVS is broken". It is the brownout case: flash
writes at a marginal supply do not fail cleanly, they write *wrong bits*, and a
page CRC written under the same brownout can be wrong in a way that agrees with
the wrong data.

## The test rig

### What to automate

Manual power-cycling gets a handful of samples. This needs hundreds, because the
window being hit is a few milliseconds wide and the interesting failures are
rare by construction.

```
   ┌────────────┐   USB     ┌──────────────┐
   │  host PC   │──────────▶│ handset      │
   │            │  serial   │ under test   │
   └─────┬──────┘◀──────────└──────┬───────┘
         │                         │ VBAT
         │ GPIO / relay            │
         └────────▶ ┌──────────────┴──┐
                    │ power switch    │
                    │ (MOSFET or SSR) │
                    └─────────────────┘
```

A second ESP32 or an FTDI with a spare pin driving a load switch is enough. A
mechanical relay is *not* — its contact bounce is milliseconds of undefined
supply, which is a different experiment (worth doing separately, see §Brownout).

### Procedure

1. Host sets a known, distinctive value: `config set station 137`.
2. Host verifies it: `config get station` → `137`.
3. Host sets a *different* distinctive value: `config set station 42`, and
   **cuts power `t` milliseconds after sending the newline**.
4. Restore power, wait for boot, read `config get station` and `post`.
5. Classify:

| Outcome | Verdict |
| --- | --- |
| `137` (old value) | **PASS** — write did not commit, previous intact |
| `42` (new value) | **PASS** — write committed before the cut |
| defaults, `CFG_LOAD_BAD_CRC` | **DETECTED** — corruption caught, settings lost |
| defaults, `CFG_LOAD_ABSENT` | **DETECTED** — NVS entry gone entirely |
| anything else | **FAIL** — this is the case that matters |

6. Sweep `t` from 0 to ~200 ms in 1 ms steps, ~20 repeats per step.

The sweep is the point. The vulnerable window is small and its position depends
on when NVS decides to erase a page, which is not every write — most writes
append, and only occasionally does one trigger a page rewrite. **That means the
dangerous case is rare and non-uniform, and only a sweep with repeats will find
it.**

### Making it observable

Add a build flag that emits a serial marker immediately before and after the
flash write:

```c
#ifdef VOICE_CONFIG_WRITE_MARKERS
  Serial.println("#CFGW-BEGIN"); Serial.flush();
#endif
  prefs.putBytes(...);
#ifdef VOICE_CONFIG_WRITE_MARKERS
  Serial.println("#CFGW-END"); Serial.flush();
#endif
```

The host times the cut relative to `#CFGW-BEGIN` rather than to the newline it
sent, which removes the console's own latency from the measurement and makes the
sweep hundreds of times more precise for the same number of runs.

### Brownout, separately

Cutting power cleanly is the easy case. The nastier one is a **slow supply
collapse** — a flat LiPo under transmit load — where the CPU keeps running at a
voltage the flash cannot reliably program at.

Approximate it by putting a series resistor and a small capacitor in the supply,
then keying up. The ESP32's brownout detector should reset before flash goes
marginal; the test is whether it actually does, and it is worth running with the
detector at its default threshold and again with it raised.

## Expected findings and what each would mean

| Finding | Meaning | Action |
| --- | --- | --- |
| No corruption in thousands of runs | NVS is doing its job | document the result, change nothing |
| CRC catches it, settings lost | works as designed, UX is poor | add the A/B slot below |
| Corruption not caught | CRC-32 is not enough, or the failure is above our layer | serious — investigate before anything else |
| Brownout corrupts, clean cut does not | it is a supply problem | raise the brownout threshold, refuse writes below a voltage |

## The fix, if one is needed

### A/B slots with a sequence number

Two NVS keys, `cfg_a` and `cfg_b`, and a monotonic `writeSeq` inside the blob.
Write alternately; on load, read both, discard any that fail CRC, and take the
valid one with the higher `writeSeq`.

```
load:  a valid? b valid?   ->  use
       yes      yes            higher writeSeq
       yes      no             a
       no       yes            b
       no       no             defaults (as today)
```

This makes a torn write structurally unable to destroy the last good
configuration, because the write never touches the slot holding it. Cost: one
extra blob in NVS (44 bytes) and about thirty lines.

This also composes with [`07-config-migration.md`](07-config-migration.md) — the
inactive slot is exactly the `cfg_prev` rollback copy that document wants.

### Refuse to write on a low battery

POST already measures the supply. If it is below a threshold — 3.4 V, say —
`config set` should refuse rather than attempt a flash write at a voltage where
programming is not guaranteed:

```
rejected: supply is 3.21 V, too low to write flash safely
```

Cheap, and it removes the most likely real-world path to the problem.

### What not to do

**Do not add a "write in progress" flag to NVS.** Setting it is itself a write
that can be interrupted, so it moves the window rather than closing it. The A/B
scheme has no such window because validity is inferred from data that is already
there, not from a marker that has to be maintained.

## Effort

| | |
| --- | --- |
| Write markers behind a build flag | trivial |
| Host-side sweep script | half a day |
| Power-switch hardware | a MOSFET and a spare GPIO |
| A/B slots, *if needed* | ~30 lines |
| Low-voltage write refusal, *if needed* | ~10 lines |

## Recommendation

**Run the experiment before writing the fix.** The A/B scheme is cheap enough to
be tempting to add pre-emptively, but adding it unmeasured means never learning
whether NVS was already handling this — and that is a fact worth having, because
it applies to the log ring, to any future filesystem, and to every other ESP32
project in this collection.

If the result is "NVS already handles it", the deliverable is a documented
result and a test that can be re-run after any IDF upgrade. That is a perfectly
good outcome and arguably the more useful one.
