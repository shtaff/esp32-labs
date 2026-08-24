# esp32-partitions

A small learning lab: print the ESP32 flash **partition table twice**, once
through the library and once by reading the flash chip directly, then compare.

```
pio run -t upload -t monitor
```

Press any key in the monitor to list again.

## The two ways

| | way 1 - the library | way 2 - the chip |
|---|---|---|
| file | [src/list_api.cpp](src/list_api.cpp) | [src/list_raw.cpp](src/list_raw.cpp) |
| API | `esp_partition_find()` / `esp_partition_next()` | `esp_flash_read()` at address `0x8000` |
| data source | a linked list ESP-IDF parsed and cached at boot | 32-byte records still sitting in SPI flash |
| you must know | nothing | the table offset, the record layout, endianness |
| can fail | no | yes - bad offset, encrypted flash, corrupt table |

### Way 1: `esp_partition`

The bootloader reads the table long before `setup()` runs and the partition
component keeps a parsed copy in RAM. Iterating it is just walking that list -
no flash access happens at all. You also get things that are *not* in the table:
which slot is currently executing, which one boots next, and the app image
description compiled into the running binary.

### Way 2: raw flash

The table is a 3 KB region at flash offset `0x8000` holding an array of 32-byte
records:

```
offset  size  field
0       2     magic    0x50AA for a partition entry
2       1     type     0 = app, 1 = data, 0x40+ = custom
3       1     subtype
4       4     offset   byte address of the partition
8       4     size     bytes
12      16    label    ASCII, not necessarily NUL-terminated
28      4     flags    bit0 = encrypted, bit4 = read-only
```

The array ends at the first record whose magic is not `0x50AA` - either `0xEBEB`
(an MD5 checksum record the build tooling appends) or `0xFFFF` (erased flash).
`esp_flash_read()` takes an *absolute chip address*: no bounds, no translation,
no safety net. It does handle disabling the instruction cache around the
transfer, which is the part you really do not want to hand-roll.

## What to look for in the output

The partition rows should match exactly. The interesting part is what does not:

1. **Way 1 knows about runtime state.** "running from" and "boot slot" are not
   in the table. They come from the otadata partition and from where the CPU is
   actually executing. After an OTA update they disagree until the next reboot.
2. **Way 2 shows the MD5 record.** The `esp_partition` API filters it out; it is
   not a partition, so as far as the library is concerned it does not exist.
3. **`encrypted` is not the flag bit.** Way 2 prints the raw flags byte from
   flash. Way 1 prints the *effective* answer, which is false unless flash
   encryption is actually switched on for this chip.
4. **Turn flash encryption on and way 2 breaks.** The partition table is
   encrypted too, so the raw read returns ciphertext: no valid magics, zero
   entries. Way 1 keeps working, because the bootloader decrypted the table
   before caching it. That is the clearest demonstration in this lab of what the
   library is doing on your behalf.
5. **Custom subtypes.** `userdata` uses subtype `0x40`, which the IDF does not
   name. Both listings fall back to `?` for the name, and
   `esp_partition_find_first()` still finds it by label.

## The table itself

[partitions.csv](partitions.csv) is the source of truth. The build turns it into
the blob at `0x8000` that both listings read back. Change a size there, reflash,
and watch both listings agree on the new number - that round trip is the point.

Layout as shipped (16 MB flash):

```
nvs        data nvs       0x009000   20K
otadata    data ota       0x00E000    8K
app0       app  ota_0     0x010000    4M
app1       app  ota_1     0x410000    4M
spiffs     data spiffs    0x810000    3M
userdata   data 0x40      0xB10000    1M
coredump   data coredump  0xC10000   64K
```

## Other boards

The board here is a 4D Systems gen4-ESP32 (ESP32-S3, 16 MB flash). For anything
else, change `board` in [platformio.ini](platformio.ini) and make sure
`partitions.csv` fits that chip's flash - or comment out
`board_build.partitions` to use the stock table. On a classic ESP32 (not S3) you
can also drop `-DARDUINO_USB_CDC_ON_BOOT=1`, which is only there because the S3
talks to the host over native USB.
