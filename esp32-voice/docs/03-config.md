# 03 — Persistent configuration

**Status:** implemented · **Files:** [`include/configstore.h`](../include/configstore.h), [`src/configstore.cpp`](../src/configstore.cpp)

## What it is

Ten settings that survive a power cycle, stored in NVS, changeable over the
serial console without a toolchain.

```
config                                    show everything
config get <name>
config set <name> <value>                 one field
config set <n>=<v> [<n>=<v> ...]          several fields, ONE flash write
config reset                              back to build-time defaults
```

`config.h` holds **build-time** constants. This holds the handful of things
that have to be changeable in the field — the encryption key above all, because
**a key you can only change by rebuilding is a key nobody ever changes**.

## The settings

| Name | Range | Applies | What it does |
| --- | --- | --- | --- |
| `key` | 32 hex | restart | AES-128 key; both handsets must match |
| `station` | 0–254 | restart | station id in every packet; 0 = derive from MAC |
| `preset` | 0–7 | restart | preset to come up on |
| `encrypt` | on/off | restart | arm encryption at boot |
| `micgain` | 0–6 | **now** | microphone gain as a left shift |
| `cues` | on/off | **now** | cue tones at the edges of a transmission |
| `preroll` | 80–1000 | **now** | receive buffer before playback starts, ms |
| `txmax` | 0–300 | **now** | hard limit on one transmission, s; 0 = none |
| `loglevel` | debug/info/warn/error | **now** | minimum severity that reaches the event log |
| `presetmode` | fixed/last | **now** | boot on `preset`, or on whichever preset was last used |

`config` marks each field `now` or `restart`, and flags any field that no
longer holds its build-time default — that column answers *"what has somebody
changed on this board?"*, which is usually the real question.

### `txmax` is new behaviour

A stuck PTT button — or a handset sat on in a rucksack — otherwise transmits
until the battery dies, jamming the channel and spending the whole hour's duty
budget in the first two minutes. With `txmax` set, the transmission stops and
the display says `TX TIMEOUT`, because from the operator's side a transmission
that ended on its own is otherwise indistinguishable from a radio that died.

### `loglevel` takes names, not numbers

```
config set loglevel warn
```

A number here would be a number whose meaning depends on an enum the operator
cannot see — and getting it backwards, setting `0` expecting "silent" and
getting "log everything", is exactly the mistake worth designing out. `config`
reads it back as a name too.

See [`02-logging.md`](02-logging.md) for what the threshold does and what it
deliberately does not filter.

### `presetmode` remembers where you were

`fixed` (the default, and how every earlier build behaved) boots on whatever
`preset` says. `last` updates `preset` automatically whenever the operator
changes preset, so the handset comes back where it was left.

**The save is deferred by five seconds.** Cycling through eight presets to reach
the one you want would otherwise be eight flash writes in as many seconds, seven
of them recording a preset nobody stopped on. `configService()` commits it once
the choice has settled — from `loop()`, never from the task that changed it.

### It was added without bumping `CONFIG_VERSION`

Worth reading, because it is the mechanism [`07-config-migration.md`](07-config-migration.md)
exists to make unnecessary — and this is the one case where it genuinely was.

`logLevel` was claimed from the struct's **reserved padding**. The struct size
and every existing field offset are unchanged, so a blob written by a build
that predates the field still loads, still passes its CRC, and **keeps its
key**.

The trick that makes it safe is the encoding: the byte stores `LogLevel + 1`,
so **zero means "not set, use the default"**. Reserved bytes are zeroed, so an
old blob reads zero and behaves exactly as it did before. Storing the level
directly would have made every previously configured board silently come up at
`debug` — logging everything, filling the ring with noise and wearing the flash.

The field table's minimum for `loglevel` is therefore **0, not 1**: rejecting
the unset value at load would fail validation and throw away every *other*
setting on those boards, including the key. That is the opposite of the point.

This is what reserved padding is for.

**That reserve is now spent.** `presetMode` took the last byte, and config v2
grew the struct from 44 to 48 bytes to add a fresh four-byte reserve — plus a
real v1 → v2 migration, so nothing was lost in the process. See
[`07-config-migration.md`](07-config-migration.md).

A `static_assert(sizeof(VoiceConfig) == 48)` pins the layout, so the next change
to it is caught at compile time — the only point at which it can still be
reconsidered — rather than at load time, when "detected" already means "your
settings are gone".

## One blob, not a key per setting

The whole struct is stored under a single NVS key. That is deliberate:

- **one write**, so a `set` is one operation rather than eight that can be
  interrupted part-way through;
- **a CRC over the whole thing** catches a torn or corrupted write, which
  per-key storage cannot do at all;
- **version and size sit next to the data they describe**, so a blob written by
  an older firmware is recognisable as such rather than being silently
  reinterpreted through the current struct layout.

```c
struct VoiceConfig {
  uint32_t magic;      // 'VCFG'
  uint16_t version;    // CONFIG_VERSION
  uint16_t size;       // sizeof(VoiceConfig) as written
  uint32_t firmware;   // versionNumeric() of the build that wrote it
  uint8_t  key[16];
  uint8_t  keySet;
  ...
  uint32_t crc;        // CRC-32 over every byte before this one
};
```

Field order is chosen for alignment, not readability, so the compiler inserts
no padding that would then have to be CRC'd. Anything added later goes
immediately before `crc` and bumps `CONFIG_VERSION`.

## Loading never fails

Every problem falls back to build-time defaults and says which problem it was.
A handset with unreadable settings still has to be a handset — refusing to boot
over a bad checksum would be strictly worse than starting from known values.

| Result | Meaning |
| --- | --- |
| `CFG_LOAD_OK` | read from NVS and valid |
| `CFG_LOAD_MIGRATED` | read from an older version and upgraded in place |
| `CFG_LOAD_ABSENT` | nothing stored yet — first boot on this board |
| `CFG_LOAD_BAD_MAGIC` | stored bytes are not ours |
| `CFG_LOAD_BAD_CRC` | stored bytes are corrupt |
| `CFG_LOAD_BAD_SIZE` | right magic, wrong struct size |
| `CFG_LOAD_VERSION` | a version this build cannot read |
| `CFG_LOAD_OUT_OF_RANGE` | loaded, but a field failed validation |

`OK` and `MIGRATED` are successes. `ABSENT` logs as info; everything else logs as a **warning**, because something
was there and was wrong. `config` shows the reason at the top, so *"it forgot my
settings"* and *"it never had any"* are distinguishable.

## Validation happens twice

Once on `set` — obviously. And again **on load**, because flash rots, firmware
gets downgraded, and a value that was legal under a previous build's field table
may not be legal under this one.

Every rejection carries a reason. `"invalid"` sends somebody to read the source;
`"micgain must be 0-6"` does not.

These values reach a shift count, a DMA size and a radio power register. A bad
one is somewhere between silence and a chip that will not boot.

## The key

```
config set key 8f3a2b91c04d76e5a1b8c9d0e2f34567
```

**It is never printed back.** `config` shows `<set>` or `<not set>`, and the key
is identified by the four-character fingerprint on the SYS screen. Being able to
*write* a key over a serial line is the point of storing it here; being able to
*read* one back would make every terminal scrollback a copy of it, and that is a
bad habit to build into a tool even when the tool is a lab toy. The log records
the fingerprint, never the bytes.

An **all-zero key is refused** at both ends: `configstore` will not store one,
and `crypto` will not use one. An all-zero AES key is a published key, and a
handset showing `ENC` while using one would be lying about the only thing
encryption is for.

### One path to the key

`configBegin()` parses `VOICE_KEY_HEX` from `secrets.ini` into the *defaults*;
anything set later replaces it in NVS; `cryptoBegin()` reads the config and
nothing else.

Having `crypto.cpp` also read `secrets.ini` would give two sources of truth that
disagree the moment somebody runs `config set key` — and the disagreement would
present as *"the other handset cannot hear me"*, which is a very long way from
its cause.

## Batched writes

```
config set station=7 preset=5 cues=off txmax=120
```

**Four settings, one flash write, all or nothing.**

This is not just shorthand. Setting four fields one at a time is four NVS writes
and four windows in which power can be lost. It is also four chances to end up
half-configured: a typo in the third value leaves the first two applied and the
rest not, which is a state nobody asked for and nobody can see without checking
every field.

A batch stages every change against a private copy, validates **all** of them,
and only then writes once:

```
configBatchBegin();
configBatchStage("station", "7", &err);    // validated, nothing written
configBatchStage("preset",  "5", &err);
configBatchCommit();                        // one CRC, one NVS write
```

A rejection anywhere aborts the whole batch and the stored config is left
exactly as it was — which is why the error message names the field that failed
and says so out loud:

```
rejected: preset must be 0-7 - nothing was changed
```

The single-field form is implemented as a batch of one, so there is exactly one
code path that parses, validates and commits a value.

### What this does and does not fix

It collapses N write windows into one, which is a real reduction in exposure —
and for the common case of "configure a board from scratch", it is the
difference between eight windows and one.

It does **not** close the window. One write is still one write. The A/B slot
scheme in [`08-config-integrity.md`](08-config-integrity.md) is what would, and
that document argues for measuring before building it.

## Writes are immediate

`set` validates, writes to NVS, and logs — there is no separate `save` step.
A setting you made is a setting that survives, rather than one you forgot to
commit. Batching is what keeps that from meaning "one flash write per keystroke".

## What is deliberately not here

Anything that changes the *shape* of the firmware rather than its behaviour: the
codec mode, the oversampling factor, frames per packet, the pin map. Those size
fixed buffers and `static_assert`s at compile time, and making them
runtime-settable would mean sizing every buffer for the worst case and losing
the compile-time checks that currently catch the mistake.

## Adding a setting

One row in the `FIELDS[]` table in `configstore.cpp`, one default in
`fillDefaults()`, and — if it is a live field — one line in the console's `set`
handler to push it at whichever module owns it. Everything else (listing,
getting, validating, range display, restart flagging) is driven from the table.
