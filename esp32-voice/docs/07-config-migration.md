# 07 — Config version migration

**Status: IMPLEMENTED.** v1 → v2 migration is live. Files:
[`include/config_v1.h`](../include/config_v1.h) (frozen),
[`src/configstore.cpp`](../src/configstore.cpp).

## The problem it solved

Before this, a `CONFIG_VERSION` bump reset **every setting including the
encryption key**. On a pair of handsets that means both fall back to the
build-time key, the fingerprints change, and the operator's first symptom is
*"we can hear each other but it's noise"* — several steps removed from *"I
upgraded the firmware"*.

v2 was the first bump, and it was taken deliberately while nothing was at
stake — which is what the original version of this document recommended.

### What v2 added

`presetMode`: whether the handset boots on a fixed preset or on whichever one
was last in use.

That field would have fitted in v1's last reserved padding byte, with no version
bump at all — the trick `logLevel` used. **It was not done that way on
purpose.** The reserve was down to one byte, so the next field would have forced
a bump regardless, and it was better to spend the last byte *and* build the
machinery at a moment when the only v1 blobs in existence were on the bench.

v2 is 48 bytes rather than 44: it takes the last padding byte and adds a fresh
four-byte reserve, so the next three or four settings are free again.

## How it works

```
NVS blob ──▶ magic? ──▶ declared size == stored size? ──▶ CRC? ──▶ version?
                                                                     │
                        ┌────────────────────────────────────────────┤
                        │                          │                 │
                   > CONFIG_VERSION          == CONFIG_VERSION   < CONFIG_VERSION
                        │                          │                 │
                   refuse, defaults           use directly      migrate chain
                                                                     │
                                                            validate, use, SAVE
```

### The CRC is checked before migrating

There is no point carrying corrupt fields forward into a shiny new layout.

Checking it needs a version-agnostic CRC, which is why `crc` is required to be
the **last field in every version**: it lives at `(size - 4)` whatever the rest
of the struct looks like, so a v2 build can verify a v1 blob without knowing v1's
layout at all. That requirement is stated on the field itself in
`configstore.h`, because it is the sort of thing that looks like a style choice
until somebody moves it.

### Frozen structs

Each historical layout has its own header, with a `static_assert` on its size:

```c
// config_v1.h - FROZEN. Never edit.
struct VoiceConfigV1 { ... };
static_assert(sizeof(VoiceConfigV1) == 44, "the v1 layout is frozen");
```

**The assert is what enforces "frozen"; a comment does not.** Somebody will
eventually add a field there by accident, and the assert is what stops that
becoming a silent misread of every stored blob.

Reusing the *current* struct and guarding fields with `#ifdef` defeats the whole
exercise — then the "old" layout changes every time the new one does.

### Fields are copied one at a time, by name

```c
b->stationId     = a->stationId;
b->preset        = a->preset;
b->encryptOnBoot = a->encryptOnBoot;
...
// b->presetMode stays at its default.
```

Never `memcpy` of a common prefix. That is exactly the shortcut that silently
mis-assigns a field the day somebody reorders the struct for alignment — and it
would do so without a word from the compiler.

### The new field's default reproduces old behaviour

`presetMode` defaults to `fixed`, and that is not an arbitrary pick: v1 had no
such concept and always booted on the stored preset, so `fixed` **is** how a v1
board behaved.

A migration that changes observable behaviour is a worse outcome than one that
loses a setting, because nobody thinks to look for it.

### Validated after migrating, then saved

The current field table's range check runs on the migrated result — a value that
was legal under v1's table may not be legal under v2's.

The migrated blob is then written back **immediately**, so the migration happens
exactly once rather than on every boot for the rest of the board's life.

### Chaining

`v1 → v3` is `v1 → v2` followed by `v2 → v3`, applied in sequence in a pair of
buffers sized by `CONFIG_MAX_BLOB`. Adding v3 means: copy the current struct to
`config_v2.h`, freeze it, add one row to `MIGRATIONS[]`, write one function.

## Testing

```
config test
migration self test PASSED - a v1 blob survives intact
```

`configTestMigration()` synthesises a v1 blob with distinctive values — all
different from the defaults, so a field silently taking its default instead of
being carried across shows up — runs it through the **real** migration chain, and
checks every field arrived, plus the version, size, CRC and post-migration
validation.

**It runs on the device, not on a host.** That is deliberate. What can go wrong
here is a struct layout disagreeing with what the migration assumes, and the
layout is whatever *this* compiler produced for *this* target. A host test with a
different ABI would happily pass while the device mis-assigned a field.

It is also how the migration stays exercised once nobody owns a board with a v1
blob on it any more — which, a week after the bump, is nobody.

## Still not done

- **Rollback slot.** Keep the pre-migration bytes under a second NVS key until
  the new blob has been read back and verified, giving `config rollback` and a
  recovery path if power is lost mid-migration. This composes with the A/B
  scheme in [`08-config-integrity.md`](08-config-integrity.md) — the inactive
  slot *is* the rollback copy.
- **Per-field loss reporting.** `LOG_EV_CFG_FIELD_LOST` is reserved but unused.
  Nothing is lost in v1 → v2, so there is nothing yet to report; it matters the
  first time a field is dropped or has its range narrowed.
- **A captured golden blob.** `config test` synthesises its input. A real dump
  from a real board, committed to the repo, would additionally catch the case
  where the *device's* v1 layout was never what `config_v1.h` claims. That
  window is closed now — v1 is gone from the bench — so this is a habit for
  next time rather than something recoverable.

## Log events

```c
LOG_EV_CFG_MIGRATE      = 76,  // a = from version, b = to version
LOG_EV_CFG_MIGRATE_FAIL = 77,  // a = from version, b = to (or -1 size, 0 none)
```
