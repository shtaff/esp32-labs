# 04 — Power-on self test

**Status:** implemented · **Files:** [`include/post.h`](../include/post.h), [`src/post.cpp`](../src/post.cpp)

## What it is

Eight checks, run once at the end of boot, reported to the serial port, the
event log and the display. Re-runnable at any time with `post`.

```
[post] power-on self test
[post]   config   pass  v1 loaded, CRC ok
[post]   log      pass  47 records survived boot #12
[post]   codec    pass  1600: enc 54% dec 52% of frame
[post]   radio    pass  868.100 MHz, 108ms airtime, 45% keyed
[post]   mic      pass  INMP441 slot 1, 98% confidence
[post]   amp      pass  detected, 15/15 trials
[post]   power    pass  3.91 V
[post]   memory   pass  heap 243k, voice stack 19488 B
[post] POST 8 pass
```

## The rule it follows

**A self test that reports what it configured rather than what it measured is
worse than no self test, because it manufactures confidence.**

Every item is labelled in the source as one of:

| Kind | Meaning | Items |
| --- | --- | --- |
| **MEASURED** | a real reading from hardware | codec timing, microphone signature, battery volts, heap and stacks |
| **OBSERVED** | the outcome of something that already ran and could have failed | config CRC, log ring, radio init |
| **ASSERTED** | known only because we were told | the amplifier — and only the amplifier |

The amplifier is the only ASSERTED item, and its detail line says so out loud:
`I2S TX ready (not probed - see PIN_AMP_SD)`. Claiming to have tested it would
be the single most misleading thing this file could do.

**It can be promoted to MEASURED with one wire** — see below.

## PASS / WARN / FAIL

| | |
| --- | --- |
| **FAIL** | the handset cannot do its job — radio dead, codec absent or too slow |
| **WARN** | it will work, but not as intended or not for long |
| **SKIP** | not applicable to this build or preset |
| **PASS** | measured and in range |

A WARN is never promoted to a FAIL just because it is inconvenient. **A board
with no microphone is a perfectly good receiver**, and this rig is often
deliberately run that way.

## The checks worth explaining

### Codec — timing, not just creation

"Did `codec2_create()` succeed" is necessary but nowhere near sufficient: a
codec that runs *slower than real time* compiles, links and initialises
perfectly, and then produces broken audio. So an encode+decode round trip on a
frame of silence is timed against the frame budget.

**That measurement is taken on `voiceTask`, not here.** POST reads the cached
result. Two reasons, and the first one is not theoretical — it crashed the first
version of this check:

1. **Stack.** Codec2's *decode* path alone puts more than 8 kB of FFT working
   set in a single frame. `voiceTask` is sized for that at 28 kB; the Arduino
   loop task that POST runs on is not, and the symptom was a stack canary trip
   deep inside `lpc_post_filter` with a backtrace naming a DSP routine rather
   than the caller that had no business being there.
2. **Ownership.** A Codec2 instance carries mutable state. `post` is
   re-runnable from the console at any time, so calling the codec from there
   would corrupt a live transmission — a race that would never show up at boot,
   only in the field.

`voiceTask` benches the codec once, before entering its state machine;
`appBegin()` waits for that to complete so everything after it can rely on the
figure being there.

`codec.cpp` now records which task first used the codec and reports any other
caller. POST reports that as a **FAIL** — `codec used from more than one task` —
because it corrupts state and overflows a stack even when everything looks fine
for a while. The rule has been broken twice; a guard that names the mistake in
one line is worth a pointer comparison per frame.

Encode and decode are timed **separately**, and each is compared against a
whole frame period. That matters: this is a half-duplex handset, so it encodes
while transmitting and decodes while receiving and never does both. The first
version charged **one** frame period for the **sum** of the two, and reported
`108% of frame budget` on hardware that was actually running at about 54 % each
— a self test manufacturing an alarm rather than measuring one, which is the
same sin as manufacturing confidence.

The worse of the pair is the one that has to fit:

- ≥ 85 % of a frame → **FAIL**
- ≥ 60 % → **WARN**, and a `CODEC_SLOW` record in the log

Silence is not representative of speech — the pitch estimator has nothing to
lock onto — so the bench understates the real cost. It is a **floor**, and a
floor that already exceeds the frame budget is decisive on its own. Once real
audio has been through, `stat` reports the rolling averages, which are the
honest numbers.

This is the check that would catch someone switching to `CODEC2_MODE_3200` on
hardware that cannot sustain it — before they spend an afternoon blaming the
radio.

### Amplifier — ASSERTED by default, MEASURED with one wire

The MAX98357A has no status pin, no I2C and no readback. But it does have one
electrically distinctive property: **an internal 100 kΩ pulldown on SD_MODE**,
against the breakout's 1 MΩ to VIN. That is what puts a bare board in (L+R)/2
mode, and it is a signature nothing else on the bench has.

Wire `SD` to GPIO12. The probe **charges the node and watches it discharge**:

1. drive the pin hard to 3V3
2. release to a true high-Z input — *no* internal pull, because a pull would
   define the very thing being measured
3. wait 200 µs, then read the pin as a plain digital input

| | Result |
| --- | --- |
| amplifier attached | discharges through its 100 kΩ toward ~0.3 V, τ ≈ 3 µs → **LOW** |
| nothing attached | pin leakage is tens of nA; 35 pF barely moves → **HIGH** |

Full-rail separation, no ADC, no external components. Fifteen trials, majority
vote — a clean answer is unanimous, and anything in between means a marginal
joint, which is worth seeing rather than rounding away.

On its first real use this caught a wiring mistake — SD landed on the wrong
GPIO and the probe reported 0/15 — which is exactly what a presence check is
for. A test that can only ever say "yes" is not a test.

### Why not measure the voltage

The first attempt enabled the internal pull-up and read the **voltage**,
expecting ≈2.3 V attached and ≈3.3 V bare. First hardware run gave 1995 mV
"attached" and 2067 mV "bare" — 72 mV apart, useless.

**That turned out to be a miswire**: SD was on the wrong pin, so GPIO12 was
floating in *both* cases and the two numbers were two samples of the same
condition. The voltage method was never actually tested. Worth stating plainly,
because the obvious conclusion — "the ADC cannot see it" — was not what the
data showed.

What the data *did* show is still useful, and is why the discharge test was
kept once the wiring was fixed:

- A true 3.3 V read back as **~2.07 V**. The ESP32 ADC at 11 dB attenuation is
  genuinely compressed above ~2 V, and the ~45 kΩ internal pull-up is far above
  the ~10 kΩ source impedance the ADC wants, so the sample-and-hold never fully
  charges either.
- Two samples of an identical condition differed by **72 mV**, so that is the
  noise floor of this measurement.

The voltage method would therefore have had roughly 170 mV of signal against
70 mV of noise — about 2.4:1. Workable, probably. The discharge test gives the
whole rail and reads 15/15 versus 0/15 on the same hardware, because a digital
input has a threshold rather than a transfer curve.

Both are recorded here rather than just the winner: the reasoning that picked
the discharge test is only sound if the numbers behind it are the real ones.

**Why GPIO12 is safe here** when it is unusable as a button: GPIO12 is the
flash-voltage strapping pin and must read LOW at boot. A button with a pull-up
reads HIGH and sets the flash rail to 1.8 V, and the board never boots. The
amplifier's network does the opposite — 100 kΩ down against 1 MΩ up holds the
node near 0.1 V through the ROM bootloader, which is the value that is wanted.

**The same wire also buys a real shutdown.** SD_MODE *is* the amplifier's
shutdown control, so the firmware now powers the output stage down between
transmissions instead of feeding it zeros through DIN. That saves its quiescent
current and is a strictly better mute.

One caveat: GPIO12 is on ADC2, which cannot be read while WiFi is active. This
firmware never brings WiFi up, so it does not matter — but it is the first thing
to check if this moves into a project that does.

Sources: [MAX98357A datasheet](https://cdn-shop.adafruit.com/product-files/3006/MAX98357A-MAX98357B.pdf),
[Adafruit MAX98357 pinouts](https://learn.adafruit.com/adafruit-max98357-i2s-class-d-mono-amp/pinouts)

### Power — a real measurement, honestly bounded

GPIO35 carries a 1:2 divider from the battery on the T3\_V1.6.1.

`analogReadMilliVolts()` is used rather than `analogRead()` because the raw ADC
on this chip is markedly non-linear and varies part to part; the milliVolts call
applies the per-chip calibration burned into eFuse at the factory. Without it,
this "measurement" would be a number with the shape of a voltage and none of the
accuracy — exactly the manufactured confidence the rule above forbids. Sixteen
reads are averaged, which costs nothing at boot and takes the edge off the noise
from having a radio on the same board.

The result is bounded honestly:

| Reading | Verdict |
| --- | --- |
| < 2.5 V | **SKIP** — "no cell on the divider (USB power?)" |
| 2.5–3.3 V | **WARN** — low, recharge soon |
| 3.3–5.5 V | **PASS** |
| > 5.5 V | **WARN** — implausible, check the divider |

The low bound matters: with no cell fitted and the board on USB, this pin reads
whatever the divider leaks to, and that is *not* a battery voltage. Sending
somebody to charge a battery that is not there is worse than saying nothing.

### Memory — stacks, not just heap

Heap alone is not enough. **This firmware has already been taken down once by a
stack overflow that left plenty of heap free** (see
[`01-version.md`](01-version.md) history and `SET_LOOP_TASK_STACK_SIZE` in
`main.cpp`). So POST walks all five tasks and reports the tightest one.

- heap < 20 k **or** any stack < 512 B → FAIL
- heap < 40 k **or** any stack < 1 kB → WARN

A stack under 1 kB also writes a `STACK_LOW` record, so the trend is visible in
the log even if nobody was watching the console.

### Microphone — signature, not level

Reuses the boot probe described in the README: it counts samples whose low byte
is zero, which is the INMP441's 24-bit-in-32-bit signature, rather than looking
for signal. A floating pin produces convincing garbage that any level test would
call a microphone.

## Where it shows up

| | |
| --- | --- |
| Serial | full table at boot, and on `post` |
| Log | one `POST_ITEM` record per check, plus a `POST_SUMMARY` |
| Display | one line on the VERSION screen: `POST 8 pass` / `POST 1 FAIL` |
| Display | a 4-second `POST FAIL <item>` flash on any failure |

A failure is put in front of whoever is holding the board, not just into a log
they have to know to read.

The log records use `LOG_LVL_ERROR` for failures and `LOG_LVL_WARN` for
warnings, so a later `log` dump shows POST problems at the right severity
without needing to decode the result codes by eye.

## Re-running it

`post` re-runs everything. Most items are cheap re-reads, but the battery is
measured fresh — which is the main reason to ask again. **A reading taken at
boot says nothing about the cell twenty minutes of transmitting later**, and
transmitting is exactly what pulls it down.

## Adding a check

Add to the `PostItem` enum, add a name in `postItemName()`, and add a block in
`postRun()` that calls `set(item, result, "detail %d", ...)`. Label it MEASURED,
OBSERVED or ASSERTED in a comment, and if it is ASSERTED, say so in the detail
string where the operator will see it.
