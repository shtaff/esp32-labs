# esp32-voice — design notes

One document per subsystem. The main [`README`](../README.md) covers wiring,
presets, the radio plan and the duty-cycle discussion; these cover the parts
added to make the lab rig survive contact with a field.

| | Document | Status |
| --- | --- | --- |
| 01 | [Firmware version and build provenance](01-version.md) | implemented |
| 02 | [Binary event log](02-logging.md) | implemented |
| 03 | [Persistent configuration](03-config.md) | implemented |
| 04 | [Power-on self test](04-post.md) | implemented |
| 05 | [Over-the-air protocol specification](05-protocol.md) | implemented, v1 |
| 06 | [Field checklist](06-field-checklist.md) | procedure |
| 07 | [Config version migration](07-config-migration.md) | implemented, v1 → v2 |
| 08 | [Breaking the config on purpose](08-config-integrity.md) | **plan only** |

## Three version numbers

They move on their own rules, and fusing any two of them would be a mistake.

| Number | Lives in | Changes when | Who cares |
| --- | --- | --- | --- |
| Firmware semver | `version.h` | this codebase changes | you |
| Protocol version | `link.h` | the bytes on the air change | the other handset |
| Config version | `configstore.h` | the NVS layout changes | this board's own flash |

Config is at **v2**; a v1 blob is migrated forward on first boot, keeping every
setting including the key. `config test` verifies that on the device.

A cosmetic display fix should not force a protocol mismatch. A new setting
should not stop two boards talking.

## The console, in full

```
help              this list
stat              everything the display screens show, at once
version           firmware version and git provenance          [01]
log [raw|clear]   event log: decoded, hex, or wipe it          [02]
config            show settings; get/set/reset, batched        [03]
config test       verify the v1 -> v2 migration on this board   [07]
post              re-run the power-on self test                [04]
presets           the preset table with duty arithmetic
preset [0-7]      show or set the preset (alias: ch)
enc [on|off]      show or set encryption
screen [0-4]      show or set the display screen
ptt [ms]          key up for ms, then release
tone [on|off]     force the synthetic test signal
beep [on|off]     cue tones, and play all four
reboot            restart
```

## A principle these share

Each of these subsystems exists to answer a question *after* the moment has
passed, when nobody was watching the terminal:

- **which build was this?** → version, stamped into every log and boot banner
- **what happened before it died?** → a log that survives the reset that killed it
- **what did somebody change?** → a config that reports which fields are no
  longer at their defaults
- **is the hardware actually working, or did we just configure it?** → a POST
  that distinguishes *measured* from *asserted*, and says which

The last one is the load-bearing idea. A self test that reports what it
configured rather than what it measured manufactures confidence, and confidence
is the thing you least want to be wrong about in a field.
