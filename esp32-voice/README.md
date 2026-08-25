# esp32-voice — digital voice over LoRa, 868 MHz

Two LilyGO TTGO LoRa32 T3\_V1.6.1 boards, a push-to-talk button each, and a
conversation carried by Codec2 over an SX1276.

One binary goes on both boards. There is no transmitter build and no receiver
build — every unit is a half-duplex handset that listens until you hold PTT.

```
TX   INMP441 ──I2S──▶ Codec2 encode ──▶ AES-128-CTR ──▶ SX1276 ──▶ 868 MHz
RX   868 MHz ──▶ SX1276 ──▶ jitter buffer ──▶ AES-128-CTR ──▶ Codec2 decode ──I2S──▶ MAX98357A
```

Never both at once — that is not a shortcut, it is what lets the microphone and
the amplifier share four pins and one I2S peripheral. See [Tasks](#how-its-put-together).

Eight presets step from **LoRa at 868 MHz** (best range, well over the legal duty
cycle) through **FSK in the 10 % sub-band** (compliant, shorter range) to a
**local self test** that needs no radio at all.

Sibling lab: [`lora-radio-rxtx`](../lora-radio-rxtx) sweeps LoRa and FSK
profiles on the same hardware and logs RSSI against GPS position — which is the
rig to reach for if you want to *measure* the range trade-off this one only
quotes from the datasheet.

---

## Quick start

```bash
cp secrets.ini.example secrets.ini      # then put a real key in it
openssl rand -hex 16                    # ...like this one
```

Both handsets need the **same** key, so build once and flash both. There is no
`upload_port` in `platformio.ini` — with two boards plugged in, name the port:

```bash
pio run                                        # build
pio run -t upload --upload-port COM5           # board A
pio run -t upload --upload-port COM10          # board B
pio device monitor --port COM5                 # watch one of them
```

### First light

1. **Qualify each board on its own.** Press MODE seven times to reach
   `SELF TEST`. Hold PTT, say something, let go — you should hear yourself
   played back. That proves the microphone, the codec and the amplifier without
   the radio being involved at all.
2. **Press MODE once more** on both boards. That wraps back to preset 0,
   `868.1 LoRa`.
3. **Hold PTT on one and talk.** The other should show `RX`, play it, and then
   beep — a falling two-tone if the transmission ended cleanly, a low double
   blip if it dropped out.

If step 1 works and step 3 doesn't, the fault is in the radio. If step 1
doesn't work, nothing about the radio matters yet.

---

## Controls

| Button | Action | Effect |
| --- | --- | --- |
| **PTT** (GPIO4) | hold | transmit while held |
| **MODE** (GPIO2) | press | next preset (0–7) |
| **MODE** | double press | encryption on / off |
| **MODE** | hold ~1 s | next display screen |

PTT acts on the edge; MODE waits out a 350 ms double-press window before it
commits to a single press. That asymmetry is deliberate — a push-to-talk button
that spends a third of a second deciding what you meant clips the first word off
every transmission.

Everything the buttons do is also on the serial console, so the firmware can be
brought up and a link proved on a board with nothing soldered to it yet:

| Command | |
| --- | --- |
| `help` | the list |
| `stat` | everything the four screens show, at once |
| `presets` | the preset table, with the duty arithmetic per row |
| `preset [0-7]` | show or set the preset (`ch` is an alias) |
| `enc [on\|off]` | show or set encryption |
| `screen [0-3]` | show or set the display screen |
| `ptt [ms]` | key up for ms (default 3000), then release |
| `tone [on\|off]` | force the synthetic test signal |
| `beep [on\|off]` | cue tones, and play all four so you learn them |
| `reboot` | restart |

---

## Wiring

Six pins, and none of them is a free choice — see the long comment in
[config.h](include/config.h) for how the board's routing and the ESP32's
strapping pins narrow it to exactly this.

### Microphone — INMP441

| INMP441 | ESP32 | Note |
| --- | --- | --- |
| VDD | 3V3 | |
| GND | GND | |
| SCK | **GPIO14** | shared with the amplifier |
| WS | **GPIO15** | shared with the amplifier |
| SD | **GPIO34** | input-only pin, which is exactly right for a data input |
| L/R | GND *or* 3V3 | **either works** — the firmware finds it, see below |

### Amplifier — MAX98357A

| MAX98357A | ESP32 | Note |
| --- | --- | --- |
| VIN | 5V (or 3V3) | 5 V is louder |
| GND | GND | |
| BCLK | **GPIO14** | shared with the microphone |
| LRC | **GPIO15** | shared with the microphone |
| DIN | **GPIO13** | |
| GAIN | leave floating | 9 dB |
| SD | leave floating | plays (L+R)/2 |

### Buttons

Both to ground, no external resistors — the internal pull-ups do it.

| Button | ESP32 |
| --- | --- |
| PTT | **GPIO4** → GND |
| MODE | **GPIO2** → GND |

GPIO25 stays as the onboard status LED: lit while transmitting, a short flash on
each received packet.

### ⚠ Do not fit a microSD card

The three I2S pins (14, 15, 13) and the MODE button (2) are the card slot's four
pins. The slot is unused here, which is what makes them available — but a card
in it will drive GPIO2 and fight the I2S bus.

### ⚠ Do not touch GPIO16 or GPIO17

Not even `pinMode()`. On this module they carry the flash bus, and re-muxing
either one kills the chip on the next cache miss — `rst:0x8 (TG1WDT_SYS_RESET)`
about 900 ms into the boot, no panic output, no backtrace. The
`ttgo-lora32-v21` variant header nominates GPIO16 as `OLED_RST` and
Adafruit\_SSD1306 drives it by default, so the obvious way to bring the display
up bricks the board on every boot. `PIN_OLED_RST` is `-1` here for that reason.

### How the microphone is found

Not by looking for signal, and not from the L/R strap. GPIO34 is input-only with
no internal pull-up, so with nothing fitted it floats, picks up whatever is on
the board, and delivers a stream of convincing garbage — a level test calls that
a microphone every time.

What separates the two cases is the *shape* of the words. The INMP441 sends 24
bits of audio into a 32-bit slot and drives the remaining 8 bits low, so **every
sample from a real part ends in a zero byte**; a floating pin manages that one
time in 256. The boot probe reads both stereo slots, counts zero low bytes in
each, and takes the slot that scores over 90 % and is not stuck at a constant.

Since that also answers *which* slot, the L/R strap on the breakout is a
don't-care. The AUDIO screen shows the slot and the confidence.

---

## Presets

A preset is a whole operating point — frequency, modem, power, and the
duty-cycle limit of the sub-band it lands in — not just a frequency. The MODE
button steps through them, and both handsets stay in step because both operators
press the button the same number of times.

That matters more than it sounds. Two handsets that disagree about the
**modulation** can't hear each other at all — not even enough to notice the
disagreement and complain about it. There's no negotiation channel here and no
way to build one, so making the modem part of the thing the button already steps
through is what stops it becoming a new way to get lost.

| # | Preset | Freq | Power | Limit | Keyed duty | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| **0** | **868.1 LoRa** | 868.100 | 14 dBm | 1 % | 45 % | default, best range |
| 1 | 868.3 LoRa | 868.300 | 14 dBm | 1 % | 45 % | if 868.1 is busy |
| 2 | 868.5 LoRa | 868.500 | 14 dBm | 1 % | 45 % | if 868.3 is busy |
| 3 | 867.1 LoRa | 867.100 | 14 dBm | 1 % | 45 % | separate 1 % budget |
| 4 | 869.5 LoRa | 869.525 | 17 dBm | 10 % | 45 % | 10 % band, full range |
| **5** | **869.5 FSK50** | 869.525 | 17 dBm | 10 % | **4.3 %** | **compliant** |
| 6 | 869.9 FSK50 | 869.850 | 7 dBm | 100 % | 4.3 % | no duty limit, 5 mW |
| 7 | SELF TEST | — | — | — | — | record and play locally |

"Keyed duty" is what the duty cycle would be if you never let go of PTT.

Preset 3 is on a different ETSI sub-band from 0–2, so it has its own 1 % budget
— useful when you've spent the g1 allowance and want to keep going.

**Preset 5 is the one to reach for if you want this to be defensible**: legal
duty cycle, legal power, and a few hundred metres of range instead of a few
kilometres. Preset 6 trades nearly all the remaining range for a band with no
duty limit at all — 5 mW ERP is across a room.

### Self test (preset 7)

Not a radio channel. The radio sits in standby; PTT **records** while held (up
to 10 s) and **plays it back** through the amplifier when you let go. Press PTT
again during playback to abandon it and record afresh.

It exercises the microphone, I2S capture, the Codec2 encoder, the decoder, I2S
playback, and the direction switch between them — which on this rig is a full
driver reinstall and so the most fragile part. It's the tool you want on a board
you've just soldered, because it splits the problem in half: if speech comes back
recognisable here and the far end still hears nothing, the fault is in the radio.
If it comes back as noise, it isn't.

It works with no microphone (records the synthetic test signal, which makes it a
pure amplifier test) and with no amplifier (silent, but the VU bar still moves).

Recording-then-playback works *because* the rig is half duplex: with capture and
playback separated in time there's no conflict over the single I2S peripheral.

---

## Cue tones

Short beeps marking the edges of a transmission. On a half-duplex link the audio
just *stops* when the other station lets go — and "finished talking" and "fell
off the air" sound identical, which on a marginal link is the difference between
replying and repeating yourself.

| Cue | Where it plays | Means |
| --- | --- | --- |
| **KEYUP** | your own handset, on PTT | you're keyed — go ahead |
| **INCOMING** | the listening handset | someone has started transmitting |
| **ROGER** | the listening handset | they finished cleanly — your turn |
| **LOST** | the listening handset | they dropped out — you missed the end |

`beep off` silences them; `beep on` plays all four so you can learn them by ear.
`-DVOICE_CUE_TONES=0` builds them out.

### None of these is transmitted

The obvious implementation is to append a tone to the outgoing audio. Every
reason not to points the same way:

1. **A transmitted beep can't tell you the one thing worth knowing.** If the link
   fails, the beep fails with it — so its absence means either "they're still
   talking" or "they're gone", which is exactly the ambiguity it was supposed to
   resolve. Deciding at the *receiver* lets a missing end flag produce a
   **different sound** rather than no sound. That's why ROGER and LOST exist as a
   pair.
2. **Codec2 models a vocal tract.** Pure tones come out of it warbling and
   unpredictable. A tone synthesised at the point of playback is clean every
   time, because it never goes near the codec.
3. **It would cost airtime** on a link already over its duty budget.

Nothing extra goes on the air for any of this — `VOICE_FLAG_END` was already in
the packet header and already meant what the roger beep needs it to.

Two of them are free in the strict sense: KEYUP plays during the microphone
settling delay, INCOMING plays inside the pre-roll wait. Both are dead time that
already existed. KEYUP in particular earns its place — without it you start
talking into the settle delay and lose the first syllable, which is precisely
the mistake a real handset's key-up beep exists to prevent.

On the **self test** preset, KEYUP proves the amplifier is alive before you've
committed to recording anything: hear the beep but no playback, and the fault is
the microphone, not the speaker.

---

## Encryption

AES-128 in counter mode over the codec frames. The key lives in `secrets.ini`,
which is gitignored:

```ini
[voice]
key = 000102030405060708090a0b0c0d0e0f   ; openssl rand -hex 16
```

Both handsets need the same key. The SYS screen shows a four-character
**fingerprint** derived from it — two boards showing the same fingerprint hold
the same key, and that is the only way to tell without trying to talk, because a
wrong key sounds exactly like a wrong preset or a dead radio.

With no `secrets.ini` the key is all zero, which is the "not configured"
sentinel: the handset runs in clear and *refuses* to arm, rather than showing
ENC while protecting nothing. Double-pressing MODE then puts the reason on the
screen.

**What this is not.** No authentication — CTR is malleable, so anyone can flip
bits in a packet and the far end will decode the result and play it. No key
exchange, no forward secrecy: one static key compiled into both images, so
whoever reads the flash has it forever. No replay protection. It stops a passive
listener with an SDR from hearing the conversation. That is the entire claim.

---

## Station ID

Every packet carries a one-byte station id, so the receiver can say who's
talking. It's derived from the board's factory MAC, so two boards flashed from
the same image come up distinguishable without anyone having to remember to
change a build flag between uploads. `-DVOICE_STATION_ID=7` pins it to a number
you've written on the case.

It's also folded into the AES counter block, alongside the stream id and the
sequence number. Two handsets keying up independently draw random stream ids with
no knowledge of each other, so roughly one time in four billion they'd pick the
same one — and both start at sequence 0. Without the station byte that hands an
eavesdropper two packets encrypted under identical keystream, which XOR together
to give both plaintexts. With it, boards that differ can never collide at all.
It costs one byte of a nonce that was full of zeros.

---

## Latency, airtime, and duty cycle

Default configuration: Codec2 **1600 bps**, 6 frames per packet, LoRa
**SF7 / BW125 / CR4:5**.

| | |
| --- | --- |
| Codec2 frame | 40 ms → 8 bytes |
| Packet | 9-byte header + 6 frames = 57 bytes = 240 ms of audio |
| Airtime (LoRa SF7) | ≈ 108 ms → **45 % duty while keyed** |
| Airtime (FSK 50k) | ≈ 10 ms → **4.3 % duty while keyed** |
| Mouth-to-ear | 240 ms packing + 108 ms air + 320 ms pre-roll ≈ **670 ms** |
| PTT to open microphone | ≈ 125 ms — key-up beep, then the INMP441 settle |

The firmware measures its own airtime with `getTimeOnAir()` on every preset
change — not once at boot, because changing modem changes it by an order of
magnitude — and warns if the duty goes over 60 %, because past that the encoder
outruns the transmitter, the outbound queue backs up, and the far end falls
steadily further behind.

### What FSK buys, and what it costs

FSK is about ten times more airtime-efficient than LoRa SF7 for the same
payload. It isn't free: it has no processing gain, so the receiver is far less
sensitive.

| Modem | Sensitivity | vs LoRa SF7 | Airtime |
| --- | --- | --- | --- |
| LoRa SF7 / BW125 | −123 dBm | — | 108 ms |
| FSK 25 kbit/s | ≈ −110 dBm | −13 dB | 21 ms |
| **FSK 50 kbit/s** | ≈ −106 dBm | −17 dB | **10 ms** |
| FSK 100 kbit/s | ≈ −104 dBm | −19 dB | 5 ms |

17 dB is roughly a factor of seven in free-space range, and worse with terrain in
the way. **So FSK doesn't make this rig better — it converts a duty-cycle problem
into a range problem.** Which one you'd rather have is the experiment.

### The part that is not compliant, and the preset that is

**ETSI EN 300 220 allows 1 % duty cycle in 868.0–868.6 MHz, assessed over any one
hour.** A handset at 45 % while keyed blows through that after about 80 seconds
of talking per hour. Voice is not a 1 % application and no amount of tuning makes
it one.

The way out is the **sub-band**, not the modem. 869.4–869.65 MHz allows 10 % at
500 mW, and costs nothing in sensitivity — it's the same radio on a different
frequency. Putting FSK there as well is what actually closes the gap: preset 5 is
4.3 % against a 10 % limit.

One trap on power: the SX1276's **+20 dBm PA\_BOOST mode is itself rated by
Semtech for 1 % duty**, so it's unusable here even though 869.4–869.65 legally
permits far more. +17 dBm is the practical ceiling for continuous operation, and
that's what the g3 presets ask for.

The firmware doesn't enforce any of this. It measures the rolling hour **per
sub-band** — time spent on 868.1 doesn't count against 869.525, which is how the
regulation assesses it too — shows it against the active preset's limit, and
works out how many seconds of talking are left. The MAIN screen shows that as
`39s left/h`, because "0.42 % of 1 %" needs arithmetic and "39 s" doesn't.

### Other codec modes

Set `-DVOICE_CODEC_MODE=` in `platformio.ini` **and** enable the matching
`CODEC2_MODE_*_EN` flag, or `codec2_create()` returns `NULL` and the firmware
halts at boot saying so.

| Mode | Frame | Payload | LoRa SF7 | FSK 50k | Audio | Duty (LoRa / FSK) |
| --- | --- | --- | --- | --- | --- | --- |
| `CODEC2_MODE_700C` | 40 ms → 4 B | 33 B | 72 ms | 6 ms | 240 ms | 30 % / 2.6 % |
| `CODEC2_MODE_1600` | 40 ms → 8 B | 57 B | 108 ms | 10 ms | 240 ms | **45 % / 4.3 %** |
| `CODEC2_MODE_3200` | 20 ms → 8 B | 57 B | 108 ms | 10 ms | 120 ms | 90 % / 8.7 % |

On LoRa, 3200 needs a wider channel (`-DVOICE_BW_KHZ=250.0f`) to fit. On FSK it
fits as it is — so the FSK presets let you have *better* audio at a *lower* duty
cycle, and you pay for both in range.

The AUDIO screen shows encode time as a percentage of the frame period. If that
approaches 100 %, the codec is not keeping up with the microphone and the mode
is not usable on this hardware — drop to 700C.

⚠ Packets must stay **under 64 bytes** or the FSK presets break: the SX1276's FSK
FIFO is 64 bytes and RadioLib won't split a packet across refills. There's a
`static_assert` on it in [link.cpp](src/link.cpp) so you find out at compile
time rather than as a runtime failure that only appears on presets 5 and 6.

---

## Screens

Long-press MODE to cycle, or `screen N`. No auto-cycle: on a handset, the screen
you chose should still be there when you look down at it.

- **MAIN** — state, preset, `ENC`/`clear`, signal, VU bar, seconds keyed, and
  talk-time remaining this hour
- **LINK** — tx/rx/lost/dropped, CRC and foreign-packet rejects, duty cycle
  against the active preset's limit
- **AUDIO** — codec mode, encode cost against the frame budget, microphone probe
  result, underruns
- **SYS** — uptime, station id, heap, key fingerprint, modem settings, airtime

---

## How it's put together

Nothing runs in a mega-loop. `loop()` does the serial console and nothing else.

| Task | Core | Prio | Stack | Job | Wakes on |
| --- | --- | --- | --- | --- | --- |
| `link` | 0 | 7 | 4 K | owns the SX1276 | DIO0 interrupt, TX queued, preset change |
| `voice` | 1 | 5 | 28 K | state machine, Codec2, I2S | I2S DMA (that *is* the clock) |
| `btn` | 0 | 4 | 3 K | debounce, classify | GPIO interrupt |
| `ui` | 0 | 2 | 4 K | OLED at 4 Hz, LED at 20 Hz | its own tick |
| `loopTask` | 1 | 1 | — | serial console | — |

Three things follow from the design rather than from convenience:

**Exactly one task touches the radio.** RadioLib holds the chip's mode in its
own state, so two tasks calling into it interleave a shared state machine across
a shared SPI bus, and the failure mode is a radio quietly in the wrong mode
rather than anything that looks like a bug. Everything else queues a packet and
sets a notification bit.

**Exactly one task touches the codec and the I2S peripheral.** A Codec2 instance
carries mutable analysis state and is not thread safe, and there is one I2S
peripheral that can only point one way at a time. Half duplex is not a
limitation being worked around — it is why capture and playback can share a
task, and why the microphone and the amplifier can share the bit clock and word
select lines and fit in four pins instead of six. It is also why the console's
`beep` command *requests* the cue demo rather than playing it: the console runs
in the loop task, and a debug convenience is not a reason to put a hole in the
one-owner rule.

**There is no timer in the audio path.** Capture is paced by `i2s_read()`
blocking on the microphone DMA; playback is paced by `i2s_write()` blocking on a
full DMA ring. Both directions therefore run at exactly the rate the hardware
does and cannot drift away from it.

### Where things live

| File | |
| --- | --- |
| [include/config.h](include/config.h) | every tunable, the pin-budget reasoning, the duty-cycle arithmetic |
| [include/link.h](include/link.h) | packet format, preset table type |
| [src/main.cpp](src/main.cpp) | boot order, and which failures are fatal |
| [src/app.cpp](src/app.cpp) | the state machine: IDLE / TX / RX / TEST\_REC / TEST\_PLAY |
| [src/link.cpp](src/link.cpp) | SX1276, preset table, LoRa+FSK setup, per-band duty accounting |
| [src/audio.cpp](src/audio.cpp) | I2S, microphone probe, synthetic test signal, cue tones |
| [src/codec.cpp](src/codec.cpp) | Codec2 wrapper and its timing measurements |
| [src/crypto.cpp](src/crypto.cpp) | AES-128-CTR and the key fingerprint |
| [src/buttons.cpp](src/buttons.cpp) | interrupt → debounce → single/double/long |
| [src/ui.cpp](src/ui.cpp) | OLED screens and the status LED |
| [src/console.cpp](src/console.cpp) | serial commands |

---

## Running with audio hardware missing

Neither the microphone nor the amplifier is required, which is what lets a link
be brought up and proved with one set of parts between two boards.

**No microphone.** The boot probe notices, and PTT transmits a **synthetic test
signal** instead — an impulse train at speech pitch driving two formant
resonators, which is to say a crude sustained "ah". It is not a tone: Codec2
models a vocal tract, and feeding it a sine wave produces something you can't
tell apart from a broken link. This gets encoded, transmitted and decoded like
speech, it is obviously artificial, and it gates on and off every 900 ms so you
can hear exactly where the link is dropping frames. `tone on` forces it even
with a microphone fitted, which is how you send a repeatable stimulus while you
measure the far end.

**No amplifier.** Nothing to configure. Received audio is still decoded, still
metered, and the **VU bar on the MAIN screen still moves** — which on a board
with no speaker is how you see that the other end is talking. `-DNO_AMP` skips
the I2S write entirely if you want the pins back.

So with one microphone and one amplifier: put both on board A and nothing on
board B, hold PTT on B to send the test buzz to A's speaker, then hold PTT on A
and watch B's VU bar and packet counters move. Everything except B's absent
amplifier is verified.

---

## Troubleshooting

**Start with preset 7.** If the self test plays your voice back cleanly, the
whole audio chain is good and the fault is in the radio. If it doesn't, the radio
is irrelevant until you've fixed the audio.

| Symptom | Look at |
| --- | --- |
| Nothing received | Same **preset number** on both? Different modems can't hear each other at all. `stat` on each. |
| Received but noise | Key fingerprints match on the SYS screen? |
| `codec-mismatch` climbing | The two boards are built for different Codec2 modes. |
| `rxNoKey` in `stat` | One handset is armed and the other has no `secrets.ini`. |
| Audio stutters | AUDIO screen: encode % near 100, or underruns climbing. Drop to 700C, or raise `VOICE_PREROLL_MS`. |
| Hearing LOST, not ROGER | The far end's tail packet isn't arriving — you're at the edge of range. |
| Beeps but no speech | Amp is fine, microphone or codec isn't. Try preset 7. |
| Far end falls behind | Duty over 60 % — see the airtime table. |
| Range collapsed | On an FSK preset? That's the 17 dB, working as designed. |
| `SNR n/a` | FSK preset — the SX1276 has no SNR estimator outside LoRa. |
| Silence, mic fitted | AUDIO screen says `mic none`? Check GPIO34 and 3V3. Then try preset 7. |
| Hiss from the speaker | DIN not on GPIO13, or a microSD card is fitted. |
| Board resets ~900 ms in | Something touched GPIO16/17. |
| `packet too long` at compile time | `VOICE_FRAMES_PER_PACKET` pushed the packet past the 64-byte FSK FIFO. |

---

## Parked

Decisions taken and deliberately not implemented, with the reasoning, so they
don't have to be rediscovered.

**Analog amplifier (HXJ8002 and friends).** Would work: the only usable DAC is
GPIO25, because DAC2 is GPIO26 and that's the LoRa IRQ. It slots into the
existing half-duplex design cleanly, since the I2S peripheral has a built-in DAC
mode and keeps the DMA pacing. Costs the status LED — and worse, the LED clamps
the DAC waveform above its forward voltage, so you'd have to lift its resistor or
accept ~5 bits of range. Expect more noise too: an analog amp sitting 2 cm from a
transmitting PA picks up hash that a digital I2S amp is immune to by
construction. Worth doing as a *comparison*, not as the main path.

**Sidetone** (hearing your own voice while transmitting). This needs simultaneous
capture and playback — two I2S peripherals, six pins. It does fit, but only by
spending GPIO25's LED on an I2S data line, or by adding one 10 kΩ pull-up to put
a button on GPIO39. Don't: an open microphone and a live speaker 5 cm apart is an
acoustic feedback loop, and half duplex is currently what prevents it by
construction. Real handsets only get away with sidetone through an earpiece.

**Full-duplex voice** (both stations talking at once). Impossible on this
hardware regardless of pins: one SX1276 cannot transmit and receive at the same
time.

**Duty-cycle enforcement.** The firmware measures and displays; it does not
block. LoRaWAN-style "wait 99× your airtime" is unusable for push-to-talk — it
would give you 108 ms of speech followed by 10.7 seconds of silence.
