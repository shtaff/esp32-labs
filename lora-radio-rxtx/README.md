# LoRa / FSK propagation research rig

Two TTGO LoRa32 boards, one codebase. One transmits a fixed 64-byte packet on
six modem profiles; the other receives them and writes one CSV row per packet —
**including the packets that never arrived**, which is what makes the log a loss
measurement rather than a reception diary.

- **Board:** LilyGO TTGO LoRa32 `T3_V1.6.1` (PlatformIO id `ttgo-lora32-v21`)
- **Radio:** SX1276 @ 868 MHz via RadioLib 7.7 · **GPS:** NEO-6M (receiver only)

```bash
pio run -e tx                        # transmitter
pio run -e rx -t upload -t monitor   # receiver
```

Mode is a compile-time define, and `build_src_filter` drops the other mode's
source entirely — the transmitter binary contains no receiver code and never
links TinyGPSPlus.

---

## How the two boards stay in step

One SX1276 cannot listen to LoRa and FSK at once, nor to two spreading factors.
So the receiver has to be time-multiplexed — and a receiver switching profiles
by hand would almost never be tuned to whatever is on the air, leaving it unable
to tell "packet lost" from "wasn't listening".

Neither board negotiates anything. **Both derive UTC independently and run the
same slot arithmetic** (`src/schedule.cpp`): the transmitter from NTP over WiFi,
the receiver from GPS.

A round begins whenever `epoch_seconds % 120 == 0`, and profile *i* owns slot *i*:

```
  round = 120 s, 6 slots of 20 s
  +--------+--------+--------+--------+--------+--------+
  | slot 0 | slot 1 | slot 2 | slot 3 | slot 4 | slot 5 |
  | SF6    | SF6    | SF12   | SF12   | FSK    | FSK    |
  | BW125  | BW500  | BW125  | BW500  | 15.2k  | 100k   |
  +--------+--------+--------+--------+--------+--------+

  inside every slot - each repeat gets an equal share:
  0 ms          2000 ms                      11000 ms          20000 ms
  |              |<-- repeat 0 -->|           |<-- repeat 1 -->|     |
  reconfigure    |<------ REPEAT_STRIDE_MS ----->|              idle
```

**The offsets are identical for all six profiles** and do not depend on airtime
at all — repeat *r* goes at `SLOT_GUARD_MS + r × REPEAT_STRIDE_MS`, where the
stride is simply the usable slot divided by the repeat count. Both boards
compute the same thing from the same constants, with no measurement involved:

```
[sched] slot 20000ms, 2 repeats, stride 9000ms, guard 2000ms
[sched] transmit offsets within every slot (ms): 2000 11000
```

The two boards must print identical lines. A profile whose airtime no longer
fits its stride raises a `[sched] WARNING` rather than quietly mis-measuring.

This replaced a configurable gap measured from the end of the previous
transmission, which made offsets airtime-dependent and therefore different in
every slot. Two things fell out of the change:

- The fast profiles used to space their repeats only ~2 s apart, and repeat
  windows must not overlap — `stride > airtime + 2 × ARRIVAL_TOLERANCE_MS` —
  which capped tolerance below 1000 ms when it was already 750. A 9000 ms
  stride lifts that ceiling to ~3.2 s.
- One less thing to configure, and one less way for the two boards to disagree.

### Skew tolerance is symmetric

`SLOT_GUARD_MS` and `ARRIVAL_TOLERANCE_MS` protect **opposite directions** of
clock skew, and are deliberately equal at **2 s each**:

| | Covers | Bound |
|---|---|---|
| `SLOT_GUARD_MS` | transmitter **early** — it transmits before the receiver has retuned | skew < guard − ~20 ms retune |
| `ARRIVAL_TOLERANCE_MS` | transmitter **late** — a packet still in flight when the window closes | skew < tolerance |

Together that absorbs roughly **a day of transmitter drift in either direction**
(~72 ms/hour) if it loses its NTP server. The last transmission still ends at
13466 ms of the 20 s slot, so the extra guard costs nothing.

The trade is repeat count, not time: a generous tolerance eats stride, and at
2 s a 20 s slot fits exactly two repeats. Four would need a longer `SLOT_MS` or
a smaller tolerance — the boot check will say which way you have gone.

The other cost, stated plainly: with 2 repeats they are 9 s apart, so a moving
receiver logs them from noticeably different places — 125 m apart at 50 km/h.
No information is lost, because every row carries its own GPS position and
distance, but the pair is no longer two looks at the same spot.

**Clock accuracy is not a concern.** NTP and GPS are each good to ~±50 ms and
the 2 s guard swallows both many times over. Wire GPS 1PPS to GPIO36 and define
`GPS_PPS_ENABLED` if you want `arrival_offset_ms` to be genuinely precise
rather than merely correct.

With no GPS fix the receiver parks on SF12/BW125 and listens; one received
packet carries the transmitter's UTC and bootstraps the schedule.

---

## Wiring

### Already on the board

```
                    TTGO LoRa32 T3_V1.6.1
   SX1276 (VSPI)              SSD1306 OLED (I2C)     microSD (HSPI)
     SCK   GPIO5                SDA  GPIO21            SCK   GPIO14
     MISO  GPIO19               SCL  GPIO22            MISO  GPIO2
     MOSI  GPIO27               addr 0x3C              MOSI  GPIO15
     CS    GPIO18                                      CS    GPIO13
     RST   GPIO23
     DIO0  GPIO26  <- RxDone / TxDone interrupt
```

Radio on VSPI, SD on HSPI — logging never contends with reception.

### ⚠ The GPIO16/17 trap

> **Never call `pinMode()` on GPIO16 or GPIO17. Not even to read them.**

Those pins carry the flash/PSRAM bus. Re-muxing one re-points a pin the CPU is
fetching instructions through, and the chip dies on the next cache miss — no
exception, no backtrace, just `rst:0x8 (TG1WDT_SYS_RESET)` about 900 ms later.
Reproducible with nothing wired, on more than one unit. It dies *inside*
`pinMode()`:

```
GPIO17: pinMode(INPUT)
ets Jun  8 2016 00:22:57      <- gone, mid-call
```

**The PlatformIO variant header lies about this.** `ttgo-lora32-v21` declares
`OLED_RST 16`, and `Adafruit_SSD1306` drives that pin by default — so the
stock way to bring up the display bricks the board on every boot.

Hence `PIN_OLED_RST` defaults to **-1**. The panel does not need a reset line.
Every use is behind `#if PIN_OLED_RST >= 0`, so `-DPIN_OLED_RST=16` restores it
on hardware where those pins are genuinely free — verify first.

### GPS — the one thing you wire

> GPIO21/22 are the OLED's I2C pins. Guides that put a NEO-6M there describe
> the older T3_V1.0, where the OLED lived on GPIO4/15.

```
     NEO-6M                    TTGO T3_V1.6.1
       VCC ------------------- 3V3
       GND ------------------- GND
        TX ------------------> GPIO34   (input-only: ideal UART RX)
        RX <------------------ GPIO4    (only needed for UBX config)
       PPS - - - - - - - - - > GPIO36   (optional, see GPS_PPS_ENABLED)
```

Avoid GPIO12 — it is the MTDI strapping pin and must be low at reset, but a
UART TX line idles high. Avoid GPIO35, which is the battery-voltage divider.
The NEO-6M draws ~50 mA while acquiring; give it its own supply if acquisition
is flaky.

### There is no button

GPIO0 is the obvious candidate and it does not work: it sits in the USB-serial
auto-reset circuit, so pressing it yields a `POWERON_RESET` with no event ever
reaching the firmware. Screens rotate every `SCREEN_AUTO_CYCLE_S` seconds
instead, and `screen` / `lock` / `unlock` over serial cover the rest.

---

## Radio profiles

`src/radio_profiles.cpp` is the source of truth. **Slot *i* belongs to
`RADIO_PROFILES[i]`, so the table's order is part of the over-the-air
contract** — reorder it and reflash both boards.

| # | Profile | Parameters | Airtime @ 64 B |
|---|---|---|---|
| 0 | LoRa SF6 / BW125 | CR 4/5, implicit header | 67 ms |
| 1 | LoRa SF6 / BW500 | CR 4/5, implicit header | 17 ms |
| 2 | LoRa SF12 / BW125 | CR 4/5, LDRO on | 2466 ms |
| 3 | LoRa SF12 / BW500 | CR 4/5 | 617 ms |
| 4 | FSK 15.2 kbps | fdev 15.2 kHz, RX BW 50 kHz | 38 ms |
| 5 | FSK 100 kbps | fdev 100 kHz, RX BW 250 kHz | 6 ms |

Three things worth knowing before reading results:

**SF6 is implicit-header only.** The SX127x cannot send a LoRa header at SF6, so
both ends must agree the length in advance — which is why the payload is a fixed
64 bytes everywhere, and incidentally makes the airtime comparison fair.

**FSK 100 kbps is deliberately under-filtered.** Carson bandwidth is
`2×fdev + bitrate` = 300 kHz, but the SX1276's receive filter caps at 250 kHz.
It decodes at short range with reduced sensitivity. A property of the chip, not
a misconfiguration.

**FSK RSSI is approximate.** There is no per-packet RSSI register in FSK mode,
so the receiver samples the running one the instant `PayloadReady` fires. LoRa
rows carry a true latched packet RSSI. Do not compare the two across modems.

---

## Duty cycle

Measured airtime is **6.42 s per 120 s round — a 5.35 % duty cycle**. The EU
868.0–868.6 MHz sub-band (ETSI EN 300 220, band g1) allows **1 %**, so the
defaults are over the limit: fine for short attended runs on your own
equipment, not for unattended operation.

To comply without changing the profile set, stretch the round:

```
SLOT_MS = 107000        ->  round 642 s, duty cycle 1.0 %
```

Other levers: SF12/BW125 down to one repeat → 4.0 s (3.3 %); removed entirely →
1.5 s (1.2 %); or move to 869.4–869.65 MHz for 10 %, though that sub-band is
only 250 kHz wide so the BW500 profiles will not fit.

Centre frequency is **868.3 MHz**, not 868.0 — a BW500 carrier at 868.0 would
span 867.75–868.25 and overhang the band edge. Full arithmetic lives in
[include/config.h](include/config.h) beside the constants it justifies.

---

## Configuration

Everything tunable is in [include/config.h](include/config.h); the common ones
are also commented `-D` flags in [platformio.ini](platformio.ini).

| Define | Default | Meaning |
|---|---|---|
| `SLOT_MS` | 20000 | Length of one profile's slot |
| `REPEATS_PER_PROFILE` | 2 | Transmissions per profile |
| `SLOT_GUARD_MS` | 2000 | Dead time at the head of a slot; also the *early*-skew budget |
| `ARRIVAL_TOLERANCE_MS` | 2000 | Grace before a packet is written off; the *late*-skew budget |
| `RADIO_FREQ_MHZ` | 868.3 | Centre frequency |
| `RADIO_POWER_DBM` | 14 | Output power (EU ERP limit) |
| `TX_SITE_LAT` / `LON` | 0,0 | Transmitter coordinate — set it in `secrets.ini` |
| `SCREEN_AUTO_CYCLE_S` | 8 | Seconds per display screen; 0 holds |

**Set the site coordinate before any real run.** Every `distance_m` and
`bearing_deg` is measured against it.

### Secrets

Two things must not be committed. Both live in one gitignored file:

```bash
cp secrets.ini.example secrets.ini
```

```ini
[wifi]
ssid = MyNetwork
password = MySecret

[site]
lat = 51.5074
lon = -0.1278
```

Picked up via `extra_configs`; a missing file falls back to harmless
placeholders, so the repo stays buildable as published.

- **WiFi** is only for the transmitter's NTP sync. With placeholders left in it
  boots but **refuses to transmit** — an unsynchronised transmitter only fills
  slots nobody is watching.
- **The site coordinate** is the location of your equipment. `0,0` is the
  not-configured sentinel and **both boards say so at boot**, because a wrong
  coordinate produces a log that looks perfectly well-formed and is fiction:

```
*** TX SITE NOT SET - put real values in [site] of
*** secrets.ini. distance_m and bearing_deg will be
*** meaningless until you do, and nothing else will say so.
```

`.gitignore` also excludes `*.csv`: retrieved logs carry a GPS track of
wherever the receiver was taken, which is personal location data.

> Moving a coordinate out of a tracked file does not remove it from commits
> that already contain it. If yours was ever committed, rewrite the history or
> start a fresh repository — the old blob is still there otherwise.

---

## The receiver display

Four screens, rotating every 8 s (or `screen` over serial).

```
  ROUND r7318      GPS        GPS              GPS        PACKET          GPS
  --------------------        ------------------------    ---------------------
  SF6 BW125  2/2 -71          fix OK  sats 9              SF12 BW125
  SF6 BW500  1/2 -83          lat  51.50735               rssi -104.5 dBm
  SF12 BW125 2/2 -102         lon  -0.12776               snr  -7.5 dB
  SF12 BW500 2/2 -95          hdop 1.1                    toa  2466 ms
  FSK 15k2   0/2              dist 4.21 km                off +12 ms seq87
  FSK 100k   0/2
```

`ROUND` shows the **last completed** round: received/expected and best RSSI per
profile. The top-right corner shows the clock source (`GPS`, `NTP`, `PKT`,
`none`), or `LOCK` when pinned to one profile. A fourth `STATUS` screen carries
UTC, clock source, storage backend, free space and row count.

**Profile lock** (`lock <n>` / `unlock`) pins the receiver to one profile,
ignoring the schedule — for walking a range test through the set by hand, or a
long soak on one profile. Miss detection is suspended while locked away from the
scheduled slot, since nothing is due there.

---

## Log format

Two CSVs, on microSD if a card is present, otherwise internal LittleFS — same
paths and rows either way.

### `/rx_log.csv` — one row per expected packet

```
utc_time,round,slot,repeat,modem,profile,params,
rx_lat,rx_lon,gps_valid,gps_sats,gps_hdop,gps_alt_m,
gps_speed_kmh,gps_course_deg,gps_age_ms,
tx_lat,tx_lon,distance_m,
outcome,rssi_dbm,snr_db,freq_err_hz,
toa_calc_ms,toa_tx_ms,arrival_offset_ms,
tx_seq,tx_round_id,payload_len,payload_hex
```

| Column | Notes |
|---|---|
| `utc_time` | Receiver UTC, ISO 8601 with milliseconds |
| `round` / `slot` / `repeat` | Position in the schedule; `slot` is also the profile index |
| `params` | `SF12/BW125/CR4-5` or `BR100.0k/FD100.0k/RXBW250k` |
| `gps_*` | Full GPS state at the instant of the event. `gps_sats`, `gps_hdop` and `gps_age_ms` are written **even with no usable position** — they distinguish "under a bridge" from "antenna unplugged" |
| `distance_m` | Great-circle receiver → transmitter site, metres |
| `bearing_deg` | Bearing receiver → transmitter site, degrees clockwise from true north. Lets a run be sliced by direction, which is what turns a scatter of ranges into something readable as an antenna pattern or a terrain shadow |
| `rssi_dbm` | Latched packet RSSI for LoRa, sampled running RSSI for FSK |
| `snr_db` | **LoRa only** — empty for FSK, where SNR has no meaning |
| `freq_err_hz` | LoRa only; useful for spotting crystal drift |
| `toa_calc_ms` / `toa_tx_ms` | Airtime as computed by receiver / transmitter |
| `arrival_offset_ms` | Arrival minus *scheduled start + airtime*. Zero means the clocks agree; the sign says which board is ahead |
| `payload_hex` | The raw 64 bytes as they arrived. Populated on CRC-failure rows too. Empty only on `fail` and `rx_err<n>` |

Every row carries the complete GPS state, so it stands alone: recompute
distance against any reference, weight by fix quality, or drop rows taken at
speed — without joining to the GPS log on timestamp.

Columns that are not meaningful for a row are written **empty**, not zero, so
`pandas.read_csv` sees `NaN` rather than a real-looking measurement.

### Outcomes

Mutually exclusive. Only `fail` is synthesised by the schedule; every other row
answers an actual RxDone interrupt.

| Value | What happened | `rssi` | `payload_hex` |
|---|---|---|---|
| `received` | Magic and CRC good, profile index matches the slot | yes | yes |
| `fail` | **No radio event at all** — an expected repeat's window closed empty | — | — |
| `crc_filler` | Frame CRC failed, **but magic and payload CRC over bytes 0..21 pass** — damage confined to the filler, every field intact | yes | yes |
| `crc_error` | Frame CRC failed and the payload check fails too: header damaged, or never ours | yes | yes |
| `bad_payload` | Good frame CRC, but magic is not `LRX1` or payload CRC fails — someone else's traffic, or a false sync | yes | yes |
| `wrong_slot` | Our packet, intact, but its profile index is not the slot we were on — the clocks have drifted | yes | yes |
| `rx_err<n>` | Any other RadioLib error, `<n>` the numeric code | yes | — |

Ordered by how good the link was:

```
  received  >  crc_filler  >  crc_error  >  fail
  perfect      payload         frame        nothing
               intact,         damaged      detected
               filler hit      beyond use
```

**Why `crc_filler` is split out.** The radio's CRC covers all 64 bytes; the
payload's own CRC covers bytes 0..21. When the first fails and the second
passes, corruption must be in bytes 24..63 — the filler. Since that is *nearly
two thirds of the packet* now the coordinate has gone, it is where a lone bit error most often lands, and when it does,
everything the packet carried got through unharmed. These rows keep usable
`tx_seq`, `tx_round_id`, `toa_tx_ms` and `arrival_offset_ms`. They are **not**
counted as successes, but the distinction cannot be recovered afterwards without
redoing the check offline.

**One row per transmission.** A damaged arrival is attributed to its repeat — by
the packet's own repeat index when readable, otherwise by arrival time — so the
miss evaluator does not add a `fail` row on top. Counting rows gives the right
answer.

`wrong_slot` is a diagnostic, not a propagation result: a steady stream means
the transmitter's clock has drifted (check its `clk<N>m`) and the surrounding
`fail` rows are suspect.

### `/gps_log.csv` — one row every 10 s

```
utc_time,fix_valid,satellites,hdop,lat,lon,alt_m,speed_kmh,course_deg,
distance_to_tx_m,bearing_to_tx_deg
```

Separate because it samples continuously — it is the receiver's movement track,
whereas the packet log samples only when something was due on the air.

### Schema changes

The header is compared at boot. A `rx_log.csv` with a different layout is
renamed to `rx_log.csv.old` and a fresh file started:

```
[storage] /rx_log.csv had an older column layout, moved to /rx_log.csv.old
```

Appending new-format rows under an old header would load without complaint and
be silently wrong, which is worse than an obvious break.

---

## Retrieving the logs

**Pull the card.** That is the whole procedure with an SD card fitted, and by
far the fastest way to move a long run.

Otherwise use the serial shell at 115200:

```
help    ls    cat <path>    rm <path>    df    clock    stats    format
screen                  cycle the display          (RX only)
profiles                list the profile table     (RX only)
lock <n> / unlock       pin to one profile         (RX only)
```

`cat` brackets output with `---- BEGIN/END <path> ----` markers:

```bash
pio device monitor -p COM5 -b 115200 | \
  sed -n '/---- BEGIN /,/---- END /p' | sed '1d;$d' > rx_log.csv
```

At the defaults that is 12 packet rows and 12 GPS rows per 120 s round, ~200
bytes each — about **1.4 MB per 24 h**. Trivial for an SD card; on LittleFS it
fills the partition inside a day, so `rm /rx_log.csv` between cardless runs.

**Without an SD card**, boot never formats the filesystem. The usual
`LittleFS.begin(true)` idiom would silently format the 1.44 MB partition, and an
erase that size is a good way to lose the board to the 300 ms interrupt
watchdog. So you get:

```
[storage] no SD card, and LittleFS will not mount.
[storage] logging is DISABLED. Insert a card, or type
[storage] 'format' to prepare the internal filesystem.
```

Type `format` once, with the monitor open. The transmitter never mounts
anything — it writes no logs.

---

## Running an experiment

1. **Set `[site]` in `secrets.ini`** to where the transmitter will stand.
2. **Check both antennas are connected** — an open PA_BOOST port can damage the
   SX1276 output stage.
3. Put your WiFi credentials in the same `secrets.ini`.
4. Flash both boards; confirm they print **identical** `[sched]` lines.
5. Power the transmitter and wait for `[ntp] clock set:` — it will not transmit
   until that appears.
6. Power the receiver; give the GPS a few minutes for a cold fix. It records
   before that using a packet-bootstrapped clock.
7. Walk or drive the receiver. Each completed round prints a summary.
8. Pull the card, or `cat /rx_log.csv`.

Every round covers all six profiles, so one traverse gives a comparable set for
the whole matrix rather than one profile per outing.

**Keep the transmitter in WiFi range.** The receiver is GPS-disciplined and
needs nothing, but the transmitter drifts at ~20 ppm — about 72 ms/hour. The 2 s
guard and tolerance absorb roughly a day of that, after which packets start
falling outside their windows and appearing as `fail` rows that are really clock
drift. It re-syncs every 30 minutes during a round's idle tail, taking only as
long as the tail allows and never interrupting a transmission. `clk<N>m` on the
TX display and serial line is minutes since the last successful anchor; under 30
is healthy.

---

## Packet layout

64 bytes, little-endian, `#pragma pack(1)`. Binary rather than text: it costs
the same but lets the receiver verify a CRC, confirm which profile and repeat a
packet belongs to, and recover UTC when it has no fix.

```
  off  len  type     field
    0    4  char[4]  magic "LRX1"  = 4C 52 58 31
    4    1  uint8    profile index 0..5
    5    1  uint8    repeat index within the slot
    6    2  uint16   round id: low 16 bits of the round counter
    8    4  uint32   tx epoch seconds, UTC
   12    2  uint16   tx milliseconds within that second
   14    4  uint32   transmit sequence number
   18    4  uint32   airtime the transmitter computed, ms
   22    2  uint16   CRC-16/CCITT-FALSE over bytes 0..21
   24   40  uint8[]  filler 0x00,0x01,0x02 ... 0x27
```

**No location travels over the air.** The transmitter's coordinate used to ride
in bytes 18..25 and was removed: this is an unencrypted link on a public band,
so anyone with a matching radio could have read the site position straight off
it. The receiver takes it from config instead — which it always did for the
distance calculation, so the transmitted copy was pure exposure with no use.

**CRC** is CRC-16/CCITT-FALSE: poly `0x1021`, init `0xFFFF`, no reflection, no
final XOR (check value over `123456789` is `0x29B1`). It covers **bytes 0..21
only**.

The **filler is deliberately outside the CRC**. A packet whose filler is
corrupted still passes and logs as `received` — intentional, because the filler
is a known ramp so bit errors in it can be counted offline. Putting it under the
CRC would turn every single-bit error in half the packet into an outright
rejection rather than a measurement.

**Two independent CRCs:**

| | Where | Covers | On failure |
|---|---|---|---|
| Hardware | SX1276, `setCRC(true)` | all 64 bytes | `crc_error` / `crc_filler` |
| Software | payload bytes 22..23 | bytes 0..21 | `bad_payload` |

The hardware CRC catches corruption; the software CRC plus magic answers a
different question — *is this ours at all?*

---

## Source layout

| File | Responsibility |
|---|---|
| [src/main.cpp](src/main.cpp) | Common bring-up, then hands to the mode |
| [src/tx_app.cpp](src/tx_app.cpp) | NTP sync, slot-timed transmission |
| [src/rx_app.cpp](src/rx_app.cpp) | Slot following, miss detection, logging |
| [src/schedule.cpp](src/schedule.cpp) | Anchored UTC clock and slot arithmetic |
| [src/radio_profiles.cpp](src/radio_profiles.cpp) | Profile table and how to apply one |
| [src/radio_hw.cpp](src/radio_hw.cpp) | SX1276 instance and its SPI bus |
| [src/packet.cpp](src/packet.cpp) | The 64-byte payload: build, parse, CRC |
| [src/storage.cpp](src/storage.cpp) | SD/LittleFS logging and the serial shell |
| [src/gps_module.cpp](src/gps_module.cpp) | NEO-6M parsing and clock discipline |
| [src/ui.cpp](src/ui.cpp) | OLED screens |

Boot prints a flushed `[boot] <step>` breadcrumb before each stage. The
interrupt watchdog resets this chip silently, so the last line printed is often
the only diagnostic available — worth keeping.
