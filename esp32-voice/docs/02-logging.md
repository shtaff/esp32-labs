# 02 — Binary event log

**Status:** implemented · **Files:** [`include/log.h`](../include/log.h), [`src/log.cpp`](../src/log.cpp)

## What it is

A ring of fixed 16-byte binary records in RTC memory, holding the last 192
events. It is **not** a replacement for `Serial.printf()` — printf is for a
human with a terminal attached at the time. This answers the question you ask
afterwards: *the board rebooted an hour ago in a field, what led up to it?*

```
log             recent records from RTC, decoded
log flash       the full history from flash, decoded
log raw         RTC records as hex, for archiving or offline decode
log flash raw   flash records as hex
log flush       write pending records to flash now
log clear       wipe the RTC ring
log erase       wipe the flash history
```

## Two tiers

| | RTC ring | Flash partition |
| --- | --- | --- |
| Size | 192 records (3 kB) | 4080 records (64 kB) |
| Survives reset | yes | yes |
| **Survives power off** | **no** | **yes** |
| Write cost | a spinlock and 16 bytes | erase + cache stall on both cores |
| Safe from an ISR | yes | **no** |

The ring is the write path: free enough that `logWrite()` can be called from an
interrupt. Flash is the archive, written from `loop()` on a timer, only while
the handset is idle.

**A reset loses nothing. A power cut loses at most the last few seconds.**

## It survives a reset

The ring lives in `RTC_NOINIT_ATTR` memory, which the startup code does not
zero. It survives:

| Survives | Does not survive |
| --- | --- |
| `esp_restart()` / `reboot` | power removal |
| watchdog reset | |
| panic / crash reset | |
| brownout reset | |

So the log of what happened **before** a crash is still readable after it. That
is the entire reason to have one, and it is why the first thing `logBegin()`
records is `esp_reset_reason()` — the single most useful number in the whole
log, because it separates "somebody pressed reset" from "the watchdog fired"
from "it panicked".

The boot line says which happened:

```
[boot] log ring survived a reset - 47 records from boot #12, `log` to read them
```

**Uninitialised RTC memory is not trusted.** On a cold power-up those cells
contain whatever they settled to, and uninitialised SRAM is perfectly capable
of looking like a valid log. The header carries a magic word and an FNV-1a hash
over its own fields, and `head`/`count` are bounds-checked before anything
walks the array — a corrupted index is a crash, not a wrong answer. If any
check fails the ring is reinitialised rather than decoded into fiction.

Budget: 192 × 16 = 3072 bytes plus a 28-byte header, out of the 8 kB of RTC
slow RAM. Confirmed in the ELF as `.rtc_noinit` = 3112 bytes.

## It survives power off

A dedicated 64 kB `eventlog` partition — see
[`partitions_voice.csv`](../partitions_voice.csv). Sixteen 4 kB erase sectors,
each holding a 16-byte header and 255 records.

### Finding the write position needs no stored pointer

That is the property that makes it survivable. A pointer is a thing that can be
stale or half-written; the position here is *derived from the data*:

1. Read all 16 sector headers. The valid one with the highest sequence number
   is the sector currently being filled.
2. Within it, the first all-`0xFF` slot is where the next record goes.

Wrapping is sector-granular, because NOR flash erases in sectors and nothing
smaller. When the current sector fills, the next one is erased and opened with
`seq + 1` — losing 255 records at a time. That is the price of not having a
filesystem, and the reason the partition holds twenty times the RTC ring.

### Wear is not a concern, and here is why

A sector is erased once per 255 records. At 500 records a day — heavy use — each
sector sees an erase about every eight days. NOR flash is good for 100,000
cycles, which is over two thousand years.

The thing that would break that arithmetic is logging from the audio path, at
50 records a second instead of 500 a day. Nothing does, and nothing should.

### When it flushes

`logService(idle)` is called from `loop()`. It writes when either trigger fires:

- **pending ≥ 96 records** (half the ring) — bounds what a power cut can lose
  when the handset is busy
- **10 seconds since the last flush** — bounds it when the handset is not

…and only when `idle` is true, meaning `appState() == VOICE_IDLE`.

**The idle rule is the important one.** `esp_partition_write()` disables the
instruction cache on *both* cores for the duration, so a flush during a
transmission stalls the codec and glitches the audio. Waiting is free — the
records are safe in RTC memory, and only a power cut in the next few seconds
would lose them.

The one exception is the urgent trigger: if half the ring is unflushed, records
are being lost either way, so it takes the glitch.

`log flush` forces it regardless. That is what you want before pulling a
battery, and `reboot` does it automatically.

### Torn records

An interrupted write can leave one record partly programmed — flash erased to
`0xFF`, then only partly written. At most one per power cut, always the newest.

This is **detected on read rather than prevented on write**: the `level` and
`module` fields each have a handful of legal values and `0xFF` is not one of
them. A torn record prints as:

```
  <torn record - power was lost part way through a write>
```

Seeing one is informative, not alarming — it tells you exactly when power went.

## Minimum level

```
config set loglevel warn
```

| Level | Keeps |
| --- | --- |
| `debug` | everything |
| `info` | **default** — everything currently emitted |
| `warn` | clipping, underruns, config problems, slow codec, POST warnings |
| `error` | POST failures, radio failures, failed NVS writes |

**Filtering happens on the way in, not on the way out.** A record below the
threshold is discarded in `logWrite()` and never enters the ring.

That is the right choice for a fixed-size ring: it means the 192 slots hold 192
records you care about instead of 192 slots of which most are noise. It also
keeps filtered records off the flash tier entirely, and flash writes are the
only expensive thing this subsystem does.

The cost is the usual one — **a filtered record is gone, not hidden**. Raise the
threshold to make the log quieter, not to make it faster.

`log` prints the active threshold at the top, because a log that looks emptier
than expected is nearly always a threshold somebody raised and forgot, and that
answer should be on the same screen as the symptom.

### What it does not filter

**`BOOT` is always recorded, whatever the threshold says.**

It carries the reset reason, and it is the boundary that separates one boot from
the next in a dump. Losing it would not make the log quieter — it would make it
*unreadable*: several boots' records running together, with a timestamp that
restarts somewhere in the middle and nothing to explain why.

It is written through a private `writeRecord()` that bypasses the filter.
Exactly one caller needs that, which is why the bypass is not part of the public
API.

The records emitted by `configBegin()` are also unaffected, for a related
reason: they run before the stored threshold has been applied. A board should be
able to say why its configuration failed to load whatever that configuration
says about logging.

### Nothing emits at `debug` yet

The level exists so that adding tracing later does not *also* require inventing
a way to switch it off — which is the point at which people give up and reach
for `Serial.printf` instead. `logDebug()` is there when it is wanted.

## Record format

```c
struct LogRecord {     // exactly 16 bytes, static_assert'd
  uint32_t ms;         // millis() when written; restarts each boot
  uint8_t  level;      // 0 debug, 1 info, 2 warn, 3 error
  uint8_t  module;     // boot, post, radio, audio, codec, crypto, config, app, ui
  uint16_t code;       // LogEvent
  int32_t  a;          // meaning is per-event
  int32_t  b;
};
```

**No strings.** An event is an enum plus two signed integers, and what those
integers mean is documented per event in `log.h` — in a binary log that comment
is the only place the meaning exists. The cost is that you cannot log something
you did not define an event for, which is the point: it keeps the log a record
of *things that happen* rather than a stream of prose.

The benefit is that a record is cheap enough to write from anywhere, including
an interrupt. `logWrite()` does no allocation, no formatting and no I/O; it
takes a spinlock for the handful of instructions needed to bump the head and
copy 16 bytes. A spinlock rather than a mutex specifically because it must work
with the scheduler suspended.

## Event numbering is append-only

Codes are explicit and **must never be reused or renumbered**. A dump taken from
an older build gets decoded against a newer table all the time. Retire a code by
leaving a gap; never recycle it.

For the same reason the decoder never fails on an unknown code — it prints
`event 84`, because a log from a newer firmware should still be readable by an
older tool.

## Reading it

```
log       47 records held, 47 written since clear, boot #12, ring survived the last reset

     time  L  module  event             a           b
  -------  -  ------  ----------------  ----------  ----------
     0.001  I  boot    BOOT                       3          12
     0.412  I  crypto  CRYPTO_KEY                 1       22636
     0.688  I  codec   CODEC_UP                   1           8
     1.104  I  audio   MIC_PROBE                  1          98
     1.560  I  radio   RADIO_UP              868100          14
     8.221  I  app     TX_START                   0           0
    11.043  I  app     TX_END                    12        2822
    11.044  W  audio   AUDIO_CLIP               418           0
```

If the ring has wrapped it says how many records were lost:

```
          312 older records were overwritten by the ring
```

That number comes from `total - count`, and `total` keeps counting past the
wrap precisely so the difference is available.

## Offline decoding

`log raw` emits a self-describing hex dump — the header names the format
version, record count and record size, so a file recovered from terminal
scrollback months later does not need to be matched up with the firmware that
produced it.

```
#VOICELOG v1 records=47 size=16 boots=12 total=47
E8030000 01 02 1400 ...
...
#END
```

Each line is one record, little-endian exactly as stored, so a decoder is a
`struct.unpack` and nothing else:

```python
#!/usr/bin/env python3
"""Decode a `log raw` dump. Usage: voicelog.py < dump.txt"""
import struct, sys

EVENTS = {1:"BOOT", 2:"BOOT_READY", 3:"HALT", 4:"STACK_LOW",
          10:"POST_ITEM", 11:"POST_SUMMARY",
          20:"RADIO_UP", 21:"RADIO_FAIL", 22:"PRESET", 23:"TX_START",
          24:"TX_END", 25:"RX_START", 26:"RX_END", 27:"RX_REJECT",
          28:"DUTY_OVER",
          40:"MIC_PROBE", 41:"AUDIO_CLIP", 42:"AUDIO_UNDERRUN", 43:"SELFTEST",
          50:"CODEC_UP", 51:"CODEC_SLOW",
          60:"CRYPTO_KEY", 61:"ENC_STATE",
          70:"CFG_LOAD", 71:"CFG_DEFAULTS", 72:"CFG_SAVE", 73:"CFG_SET",
          74:"CFG_RESET", 75:"CFG_BAD"}
MODULES = ["boot","post","radio","audio","codec","crypto","config","app","ui"]
LEVELS  = "DIWE"

for line in sys.stdin:
    line = line.strip()
    if not line or line.startswith("#"):
        if line.startswith("#VOICELOG"):
            print(line)
        continue
    ms, lvl, mod, code, a, b = struct.unpack("<IBBHii", bytes.fromhex(line))
    print(f"{ms/1000:9.3f}  {LEVELS[lvl & 3]}  "
          f"{MODULES[mod] if mod < len(MODULES) else '?':6}  "
          f"{EVENTS.get(code, f'event {code}'):16}  {a:10}  {b:10}")
```

## What gets logged

Boot and reset reason, POST results, radio bring-up and failures, preset
changes, every transmission (start, end, duration, packets), every reception
(who, RSSI, whether they signed off or vanished), microphone probe result,
clipping per transmission, underruns per stream, codec bring-up, key presence
(as a fingerprint — never the key), encryption state changes, and every config
load, save, set and reset.

Clipping and underruns are recorded **per transmission** rather than as running
totals, because "this over was distorted" is the useful unit — a lifetime
counter cannot tell you which one.
