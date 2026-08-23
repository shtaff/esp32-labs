// =============================================================================
// config.h - all build-time tunables for the LoRa/FSK RX-TX research rig.
//
// Board: LilyGO TTGO LoRa32 T3_V1.6.1  (PlatformIO board id: ttgo-lora32-v21)
// Radio: Semtech SX1276, 868 MHz
//
// Everything you might want to change between experiments lives here or is
// overridable from platformio.ini via -D flags. See README.md for the wiring
// diagram and the measurement procedure.
// =============================================================================
#pragma once

#include <stdint.h>

// -----------------------------------------------------------------------------
// Pin map - LilyGO T3_V1.6.1
//
// These are NOT free choices; they are how the board is routed. The only pins
// we actually pick are the two GPS pins, and they are constrained by what the
// board leaves unused (see README "Wiring").
//
// =============================================================================
// DO NOT TOUCH GPIO16 OR GPIO17. NOT EVEN pinMode().
//
// On this module those two pins carry the flash / PSRAM bus. Calling pinMode()
// on either re-muxes a pin the CPU is fetching instructions through, and the
// chip dies on the very next cache miss - instantly, before it can print a
// single character. All you see is:
//
//     rst:0x8 (TG1WDT_SYS_RESET)
//
// with no panic output and no backtrace, roughly 900 ms later. It is entirely
// reproducible, it happens with nothing wired to the board, and it reproduces
// on more than one unit.
//
// This is not theoretical: the ttgo-lora32-v21 variant header declares
// OLED_RST as GPIO16, and Adafruit_SSD1306 drives that pin by default, so the
// obvious way to bring the display up bricks the board on every boot. Taking
// the variant header at its word cost several hours of debugging.
//
// See README "The GPIO16/17 trap".
// =============================================================================

// SX1276 radio, on the ESP32 VSPI bus.
#define PIN_LORA_SCK    5
#define PIN_LORA_MISO   19
#define PIN_LORA_MOSI   27
#define PIN_LORA_CS     18
#define PIN_LORA_RST    23
#define PIN_LORA_DIO0   26   // RxDone / TxDone interrupt
#define PIN_LORA_DIO1   33   // unused here, broken out for completeness
#define PIN_LORA_DIO2   32   // unused here

// SSD1306 128x64 OLED, hard-wired to I2C on 21/22.
// NOTE: this is why the GPS cannot live on 21/22 as originally specified.
#define PIN_OLED_SDA    21
#define PIN_OLED_SCL    22
#define OLED_I2C_ADDR   0x3C

// OLED reset: DISABLED (-1), because the pin the variant header nominates for
// it is GPIO16 - see the warning above. The panel does not need a reset line
// anyway; it leaves power-on reset by itself and initialises fine over I2C.
//
// On a board where GPIO16/17 are genuinely free, -DPIN_OLED_RST=16 puts it
// back. Every use is guarded by #if PIN_OLED_RST >= 0. Verify on your own
// hardware before trusting it.
#ifndef PIN_OLED_RST
#define PIN_OLED_RST    -1
#endif

// microSD slot, on the ESP32 HSPI bus - a second, independent SPI bus, so it
// never contends with the radio.
#define PIN_SD_SCK      14
#define PIN_SD_MISO     2
#define PIN_SD_MOSI     15
#define PIN_SD_CS       13

// NEO-6M GPS on UART2 (RX mode only).
//   GPIO34 is input-only, which makes it a perfect UART RX line and costs us
//   nothing, since input-only pins cannot serve much else.
//   GPIO4 is a spare general-purpose pin, needed only if you want to push UBX
//   configuration into the module. Leave it unconnected otherwise.
#define PIN_GPS_RX      34   // ESP32 input  <- GPS TX
#define PIN_GPS_TX      4    // ESP32 output -> GPS RX  (optional)
#define GPS_BAUD        9600

// Optional 1PPS input from the GPS. Wire it and define GPS_PPS_ENABLED and the
// receiver clock becomes microsecond-accurate instead of roughly 50 ms.
//
// GPIO36, not GPIO35: on TTGO T3 boards GPIO35 is the battery-voltage divider,
// which is consistent with it reading high with nothing attached. GPIO36 is
// input-only and free.
// #define GPS_PPS_ENABLED
#define PIN_GPS_PPS     36   // input-only pin, unused unless GPS_PPS_ENABLED

// There is no button.
//
// GPIO0 is the obvious candidate and it does not work: on this hardware it sits
// in the USB-serial auto-reset circuit, so pressing it produces a
// POWERON_RESET without the firmware ever seeing an event. Screens rotate on a
// timer instead, and the `screen` / `lock` / `unlock` serial commands cover the
// rest.
//
// Seconds per screen; 0 disables the rotation.
#ifndef SCREEN_AUTO_CYCLE_S
#define SCREEN_AUTO_CYCLE_S 8
#endif

// -----------------------------------------------------------------------------
// Radio front-end
// -----------------------------------------------------------------------------

// Centre frequency in MHz.
//
// 868.3 rather than 868.0: at BW 500 kHz a carrier at 868.0 would occupy
// 867.75 - 868.25 MHz, hanging over the bottom edge of the EU g1 sub-band
// (868.0 - 868.6). Centring at 868.3 keeps even the widest profile inside it.
#ifndef RADIO_FREQ_MHZ
#define RADIO_FREQ_MHZ  868.3f
#endif

// Output power in dBm into the PA_BOOST pin.
// 14 dBm == 25 mW, the EU ERP limit for the 868.0 - 868.6 sub-band.
#ifndef RADIO_POWER_DBM
#define RADIO_POWER_DBM 14
#endif

// PA over-current protection, mA. 100 mA is ample for 14 dBm.
#define RADIO_CURRENT_LIMIT_MA 100

// LoRa sync word. 0x12 = private network (0x34 is reserved for LoRaWAN).
#define LORA_SYNC_WORD  0x12

// FSK sync word bytes - the conventional 0x2D 0xD4 pair.
#define FSK_SYNC_BYTE_0 0x2D
#define FSK_SYNC_BYTE_1 0xD4

// Preamble lengths.
#define LORA_PREAMBLE_SYMBOLS 8
#define FSK_PREAMBLE_BITS     32

// -----------------------------------------------------------------------------
// Packet
// -----------------------------------------------------------------------------

// Fixed payload size for every profile, so airtime comparisons are apples to
// apples. Also required by SF6, which can only run in implicit-header mode and
// therefore needs a length both ends agree on in advance.
#define PACKET_LEN 64

// -----------------------------------------------------------------------------
// Round schedule
//
// Both boards derive UTC independently (TX from NTP, RX from GPS) and step
// through the profile list off the wall clock. A round begins at every instant
// where  epoch_seconds % ROUND_PERIOD_S == 0.  Within a round, profile i owns
// the slot  [i*SLOT_MS, (i+1)*SLOT_MS).  Inside its slot:
//
//   slot start + 0                              -> receiver reconfigures
//   slot start + SLOT_GUARD_MS + r*REPEAT_STRIDE_MS  -> repeat r transmitted
//
// Each repeat gets an equal share of the slot, and those offsets are IDENTICAL
// for every profile - they do not depend on airtime. See REPEAT_STRIDE_MS.
//
// The receiver therefore always knows which profile is due and, crucially,
// knows which packets should have arrived - which is what lets it write
// outcome=fail rows for packets that never showed up.
// -----------------------------------------------------------------------------

// Number of profiles in RADIO_PROFILES[]. Keep the two in sync; a static_assert
// in radio_profiles.cpp enforces it.
#define RADIO_PROFILE_COUNT 6

// Length of one profile's slot, milliseconds.
#ifndef SLOT_MS
#define SLOT_MS 20000UL
#endif

// Transmissions of the same payload per profile.
#ifndef REPEATS_PER_PROFILE
#define REPEATS_PER_PROFILE 2
#endif

// Spacing between repeats: the usable part of a slot divided evenly.
//
// This replaced a configurable gap measured from the end of the previous
// transmission, which made the offsets airtime-dependent and therefore
// different in every slot. Two things came out of the change:
//
//   1. The offsets are now identical for all six profiles, so the two boards
//      agree on them without either one needing to know any airtimes.
//   2. The repeats are much further apart - 9750 ms rather than the ~2 s the
//      fast profiles used to get - which is what sets the ceiling on
//      ARRIVAL_TOLERANCE_MS below.
//
// The cost: with 2 repeats they are ~9.75 s apart, so a moving receiver logs
// them from noticeably different places. That is not a loss of information,
// because every row carries its own GPS position and distance, but the pair is
// no longer two looks at the same spot.
#define REPEAT_STRIDE_MS ((SLOT_MS - SLOT_GUARD_MS) / REPEATS_PER_PROFILE)

// Dead time at the head of each slot.
//
// This and ARRIVAL_TOLERANCE_MS protect OPPOSITE directions of clock skew, and
// are deliberately equal so the protection is symmetric:
//
//   guard      covers the transmitter being EARLY. If it is ahead by d, it
//              transmits before the receiver has retuned, so d must stay under
//              (guard - retune time), and retuning costs about 20 ms.
//   tolerance  covers the transmitter being LATE, before a packet that is
//              still in flight gets written off as lost.
//
// At 2 s each the rig absorbs roughly a day of transmitter drift in either
// direction if it loses its NTP server (~72 ms/hour). The slot still has ~6.5 s
// of idle tail afterwards, so the extra guard costs nothing.
#ifndef SLOT_GUARD_MS
#define SLOT_GUARD_MS 2000UL
#endif

// Extra tolerance either side of an expected arrival before the receiver gives
// up on a packet and books it as lost.
//
// The hard ceiling is set by repeat windows not being allowed to overlap:
//
//     REPEAT_STRIDE_MS  >  longest airtime + 2 * ARRIVAL_TOLERANCE_MS
//
// At the defaults that is 9000 > 2466 + 4000, which holds with room to spare.
// scheduleReportAirtimes() checks it at boot against the real measured
// airtimes and warns if it ever stops holding.
//
// Note the trade: a generous tolerance costs repeat count. At 2 s, a 20 s slot
// fits exactly two repeats. Wanting four means either a longer SLOT_MS or a
// smaller tolerance - the boot warning will tell you which way you have gone.
#ifndef ARRIVAL_TOLERANCE_MS
#define ARRIVAL_TOLERANCE_MS 2000UL
#endif

// Full round length, derived. Do not set directly.
#define ROUND_PERIOD_MS ((uint32_t)(SLOT_MS) * (uint32_t)(RADIO_PROFILE_COUNT))
#define ROUND_PERIOD_S  (ROUND_PERIOD_MS / 1000UL)

// =============================================================================
// DUTY CYCLE - read this before changing SLOT_MS or REPEATS_PER_PROFILE
//
// Airtime for a 64-byte payload, CR 4/5, CRC on. These are MEASURED - the
// figures RadioLib's getTimeOnAir() returned on the hardware, printed by both
// boards at boot as the [sched] table:
//
//     profile            airtime    x2 repeats
//     -----------------  --------   ----------
//     LoRa SF6  BW125      67 ms      134 ms
//     LoRa SF6  BW500      17 ms       34 ms
//     LoRa SF12 BW125    2466 ms     4932 ms   <-- dominates the budget
//     LoRa SF12 BW500     617 ms     1234 ms
//     FSK  15.2 kbps       38 ms       76 ms
//     FSK  100  kbps        6 ms       12 ms
//     -----------------  --------   ----------
//     total                          6422 ms
//
// With 6 profiles x SLOT_MS 20 s the round is 120 s, so:
//
//     duty cycle = 6.422 s / 120 s = 5.35 %
//
// The EU 868.0 - 868.6 MHz sub-band (ETSI EN 300 220, band g1) allows 1 %.
// 5.35 % is over that. It is fine for short attended bench and field runs on
// your own equipment, which is what this default is tuned for, but it is not
// compliant for unattended operation.
//
// To become compliant without changing the profile set, stretch the round:
//
//     ROUND_PERIOD_S >= 6.422 / 0.01 = 642 s   ->  SLOT_MS >= 107000
//
// giving roughly 5.6 rounds per hour. Other levers, in order of payoff:
//
//   1. SF12/BW125 down to a single repeat  -> 4.0 s airtime  (3.3 % at 120 s)
//   2. SF12/BW125 removed entirely         -> 1.5 s airtime  (1.2 % at 120 s)
//   3. Move to 869.4 - 869.65 MHz          -> 10 % allowed, but that sub-band
//      is only 250 kHz wide, so the BW500 profiles will not fit in it.
//
// The 1 % budget is per sub-band, assessed over a one-hour window, so a short
// high-rate burst followed by a long silence also satisfies it. The firmware
// does not enforce any of this for you.
// =============================================================================

// -----------------------------------------------------------------------------
// Transmitter site
//
// The receiver measures distance and bearing to this fixed point for every row
// it logs. It comes from [site] in secrets.ini, which is gitignored.
//
// It is deliberately NOT sent over the air. The link is unencrypted on a public
// band, so a coordinate in the payload could be read straight off it by anyone
// with a matching radio. The receiver always took it from config for the
// distance calculation anyway, so the transmitted copy was pure exposure with
// no use whatsoever.
//
// 0,0 is the "not configured" sentinel; both modes complain at boot, because a
// wrong coordinate yields a log that looks well-formed and is entirely fiction.
// -----------------------------------------------------------------------------
#ifndef TX_SITE_LAT
#define TX_SITE_LAT 0.0
#endif
#ifndef TX_SITE_LON
#define TX_SITE_LON 0.0
#endif

// True once a real coordinate has been supplied.
static inline bool txSiteConfigured() {
  double lat = (double)(TX_SITE_LAT);
  double lon = (double)(TX_SITE_LON);
  return (lat < -0.0001 || lat > 0.0001) || (lon < -0.0001 || lon > 0.0001);
}

// -----------------------------------------------------------------------------
// Transmitter WiFi / NTP
//
// Supply real values from platformio.ini, ideally via a gitignored
// wifi_secrets.ini (see README). With the placeholders left in place the
// transmitter still boots but refuses to transmit, because an unsynchronised
// transmitter would only spray packets into slots nobody is watching.
// -----------------------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

#define NTP_SERVER_PRIMARY   "time.nist.gov"
#define NTP_SERVER_SECONDARY "pool.ntp.org"

// How often the transmitter re-syncs from NTP. The ESP32 crystal drifts on the
// order of 10-20 ppm, under 40 ms per half hour, well inside SLOT_GUARD_MS.
// WiFi is powered down between syncs.
#ifndef NTP_RESYNC_INTERVAL_S
#define NTP_RESYNC_INTERVAL_S 1800UL
#endif

#define WIFI_CONNECT_TIMEOUT_MS 20000UL
#define NTP_SYNC_TIMEOUT_MS     15000UL

// -----------------------------------------------------------------------------
// Receiver logging
// -----------------------------------------------------------------------------
#define LOG_PATH_RX  "/rx_log.csv"
#define LOG_PATH_GPS "/gps_log.csv"

// How often a GPS position row is appended, seconds.
#ifndef GPS_LOG_INTERVAL_S
#define GPS_LOG_INTERVAL_S 10UL
#endif

// Profile the receiver parks on before it has a clock, so a single received
// packet can bootstrap it. Index into RADIO_PROFILES; 2 is SF12/BW125, the most
// robust of the set.
#ifndef BOOTSTRAP_PROFILE_INDEX
#define BOOTSTRAP_PROFILE_INDEX 2
#endif

// -----------------------------------------------------------------------------
// User interface
// -----------------------------------------------------------------------------
#define DISPLAY_REFRESH_MS   250UL
