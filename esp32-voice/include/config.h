// =============================================================================
// config.h - every build-time tunable for the LoRa digital voice handset.
//
// Board: LilyGO TTGO LoRa32 T3_V1.6.1  (PlatformIO board id: ttgo-lora32-v21)
// Radio: Semtech SX1276, 868 MHz
// Mic:   InvenSense INMP441   (I2S, 24-bit, mono)
// Amp:   Maxim MAX98357A      (I2S, class D, mono)
//
// See README.md for wiring, the channel plan and the duty-cycle discussion.
// =============================================================================
#pragma once

#include <stdint.h>

// =============================================================================
// DO NOT TOUCH GPIO16 OR GPIO17. NOT EVEN pinMode().
//
// On this module those two pins carry the flash / PSRAM bus. Calling pinMode()
// on either re-muxes a pin the CPU is fetching instructions through, and the
// chip dies on the next cache miss - instantly, with no panic output and no
// backtrace, roughly 900 ms into the boot:
//
//     rst:0x8 (TG1WDT_SYS_RESET)
//
// The ttgo-lora32-v21 variant header declares OLED_RST as GPIO16 and
// Adafruit_SSD1306 drives it by default, so the obvious way to bring the
// display up bricks the board on every boot. See PIN_OLED_RST below.
// =============================================================================

// -----------------------------------------------------------------------------
// SX1276 radio, on the ESP32 VSPI bus. Fixed by the board routing.
// -----------------------------------------------------------------------------
#define PIN_LORA_SCK    5
#define PIN_LORA_MISO   19
#define PIN_LORA_MOSI   27
#define PIN_LORA_CS     18
#define PIN_LORA_RST    23
#define PIN_LORA_DIO0   26   // TxDone / RxDone interrupt - the only IRQ we use
#define PIN_LORA_DIO1   33   // driven by the radio; do not repurpose
#define PIN_LORA_DIO2   32   // driven by the radio; do not repurpose

// -----------------------------------------------------------------------------
// SSD1306 128x64 OLED, hard-wired to I2C on 21/22.
// -----------------------------------------------------------------------------
#define PIN_OLED_SDA    21
#define PIN_OLED_SCL    22
#define OLED_I2C_ADDR   0x3C

// OLED reset: DISABLED (-1). The pin the variant header nominates for it is
// GPIO16 - see the warning above. The panel leaves power-on reset by itself
// and initialises fine over I2C without a reset line.
#ifndef PIN_OLED_RST
#define PIN_OLED_RST    -1
#endif

// =============================================================================
// Audio and buttons - the six pins this application actually chooses.
//
// The choice is tightly constrained. Once the radio (5/19/27/18/23/26) and the
// OLED (21/22) are accounted for, 16/17 are struck out, and 1/3 are left to the
// serial console, what the T3_V1.6.1 actually breaks out is:
//
//     bidirectional : 0, 2, 4, 12, 13, 14, 15, 25
//     input only    : 34, 35, 36, 39      (37 and 38 are NOT on the headers)
//     radio-owned   : 32, 33              (wired to the SX1276's DIO1/DIO2)
//
// and most of that is already spoken for:
//
//     GPIO0    sits in the USB-serial auto-reset circuit on this hardware.
//              Pressing a button on it produces a POWERON_RESET without the
//              firmware ever seeing an edge, and driving it fights the reset
//              circuit. Unusable in either direction.
//     GPIO12   is the flash-voltage strapping pin (MTDI) and must read LOW at
//              boot. A button to ground with a pull-up reads HIGH, which sets
//              the flash rail to 1.8 V and the board never boots. It is fine
//              as an OUTPUT, which is undriven at boot - but never as a
//              pulled-up input. Left unused here.
//     GPIO25   is the onboard LED, tied through a resistor and the LED to
//              ground. As an input with a pull-up it floats around 2.0 V,
//              below V_IH - so it cannot be a reliable button. Fine as an
//              output. Kept as the status LED, which is what it is for.
//     GPIO34/35/36/39 are input-only AND have no internal pull-ups. Perfect
//              for a data input, useless for a button unless you fit a
//              resistor. GPIO35 is also the battery-voltage divider.
//
// So exactly TWO pins can be buttons with no external parts - GPIO4 and GPIO2 -
// and five more can be outputs: 12, 13, 14, 15, 25.
//
// Two I2S peripherals would need six pins: a clock pair and a data line each
// for the microphone and the amplifier. That does fit, but only just, and only
// by spending GPIO25 - so the price of full duplex is the status LED, or an
// external pull-up to put the second button on GPIO39.
//
// It is not worth paying, because this is a HALF-DUPLEX handset. It never
// records and plays at the same time, so it does not need two peripherals at
// all: one is reconfigured between RX (microphone) and TX (amplifier) on each
// transition, and the bit clock and word select lines are shared by both
// devices. Four pins instead of six, no external components, the LED kept, and
// each direction still gets its own channel format - which matters; see the
// INMP441 left/right quirk in audio.cpp.
//
// (The thing full duplex would buy is sidetone, and sidetone on a handset with
// the speaker five centimetres from the microphone is an acoustic feedback
// loop. Half duplex is what stops it howling. See runSelfTestRecord() in
// app.cpp.)
//
// Three of the four I2S pins - 14, 15 and 13 - are the microSD slot's, and so
// is the MODE button on GPIO2. Between them that is the entire card slot.
// Nothing here uses it, which is what makes those pins available - but DO NOT
// FIT A CARD while this firmware is running.
// =============================================================================

// I2S bit clock. Shared: INMP441 SCK and MAX98357A BCLK.        [SD slot SCK]
#define PIN_I2S_BCLK    14

// I2S word select / frame clock. Shared: INMP441 WS, MAX98357A LRC.
//
// GPIO15 is a strapping pin (MTDO) that must read HIGH at boot. It has a
// default internal pull-up and we do not drive it until i2s_set_pin() runs, so
// it is high through the whole ROM bootloader. Correct by construction.
//                                                              [SD slot MOSI]
#define PIN_I2S_WS      15

// I2S data out, ESP32 -> MAX98357A DIN.                          [SD slot CS]
#define PIN_I2S_DOUT    13

// I2S data in, INMP441 SD -> ESP32.
//
// Input-only pin, which is exactly what a data input wants, and spending one
// here is what frees a bidirectional pin for a button.        [was the GPS RX]
#define PIN_I2S_DIN     34

// Push to talk. Active low, internal pull-up, no strapping role. [was GPS TX]
#define PIN_BTN_PTT     4

// Mode button: short press = next channel, double press = encryption on/off,
// long press = next display screen. Active low, internal pull-up.
//
// GPIO2 is a strapping pin, but in the harmless direction: it must be LOW or
// floating to enter the serial bootloader. Released, it floats through the ROM
// (our pull-up is only enabled in setup()); held, it is pulled to ground,
// which is the state the bootloader wants anyway.              [SD slot MISO]
#define PIN_BTN_MODE    2

// Onboard LED. Lit while transmitting, blinked on a received packet.
#define PIN_LED         25

// -----------------------------------------------------------------------------
// Button timing. All of it is applied in buttons.cpp, in a task fed by a GPIO
// interrupt - nothing here is polled.
// -----------------------------------------------------------------------------

// Contact bounce window. Edges closer together than this are the same edge.
#define BTN_DEBOUNCE_MS      25UL

// How long after a release to keep waiting for a second press before calling
// it a single press. This is dead time on every single press of the mode
// button, which is why PTT does not use it - PTT acts on the edge itself.
#define BTN_DOUBLE_GAP_MS    350UL

// Hold time that promotes a press to a long press.
#define BTN_LONG_PRESS_MS    900UL

// =============================================================================
// Radio
// =============================================================================

// Output power is NOT set here - it is a property of each preset, because it is
// a property of the sub-band. See VOICE_PRESETS[] in link.cpp:
//
//   14 dBm (25 mW)  in 863 - 868.6, which is the ERP limit there
//   17 dBm (50 mW)  in 869.4 - 869.65, which permits far more but where the
//                   SX1276's own +20 dBm mode is rated for 1 % duty and so is
//                   unusable for voice
//    7 dBm  (5 mW)  in 869.7 - 870.0, which is the ERP limit there
//
// All of it goes out of the PA_BOOST pin, which is what the antenna connector
// on this board is wired to. RadioLib picks PA_BOOST for any power above
// 2 dBm, so there is nothing to configure.

// PA over-current protection, mA. 100 mA covers the whole range above; the
// +17 dBm setting draws around 90 mA on this part.
#define VOICE_CURRENT_LIMIT_MA 100

// LoRa sync word. 0x12 = private network (0x34 is reserved for LoRaWAN).
#define VOICE_SYNC_WORD 0x12

#define VOICE_PREAMBLE_SYMBOLS 8

// Spreading factor, bandwidth (kHz) and coding rate denominator (5 = 4/5), for
// every LoRa preset in the table.
//
// SF7 / BW125 / CR4:5 is a standard EU 125 kHz channel and carries roughly
// 5470 bit/s of raw payload. It is also the ONLY LoRa setting the default
// codec mode fits into: SF8 needs 195 ms for a packet carrying 240 ms of
// audio, which is 81 % duty and leaves no room to breathe, and SF9 needs
// 349 ms, which does not fit at all. See the airtime table below.
#ifndef VOICE_SF
#define VOICE_SF 7
#endif
#ifndef VOICE_BW_KHZ
#define VOICE_BW_KHZ 125.0f
#endif
#ifndef VOICE_CR
#define VOICE_CR 5
#endif

// -----------------------------------------------------------------------------
// FSK parameters, for every FSK preset in the table.
//
// 50 kbit/s with 25 kHz deviation is a modulation index of 1.0, which puts the
// occupied bandwidth at roughly br + 2*fdev = 100 kHz. That fits inside the
// 250 kHz-wide 869.4 - 869.65 sub-band with room on both sides.
//
// The receiver bandwidth has to cover half the bit rate, plus the deviation,
// plus the frequency error of both crystals - at +/-10 ppm on 869 MHz that is
// about 17 kHz between two boards:
//
//     25 + 25 + 17 = 67 kHz  ->  the next SX127x step up is 83.3 kHz
//
// Wider than that only lets in more noise. Narrower and two cold boards drift
// out of each other's passband, which presents as a link that works on the
// bench and not in the garden.
#ifndef VOICE_FSK_BR_KBPS
#define VOICE_FSK_BR_KBPS 50.0f
#endif
#ifndef VOICE_FSK_FDEV_KHZ
#define VOICE_FSK_FDEV_KHZ 25.0f
#endif
#ifndef VOICE_FSK_RXBW_KHZ
#define VOICE_FSK_RXBW_KHZ 83.3f
#endif

// FSK preamble, in BITS (RadioLib divides by 8 for the register). Four bytes
// is the conventional minimum for reliable bit synchronisation.
#define VOICE_FSK_PREAMBLE_BITS 32

// FSK sync word - the conventional 0x2D 0xD4 pair, same as the sibling
// propagation lab, so a receiver there can see this traffic.
#define VOICE_FSK_SYNC_0 0x2D
#define VOICE_FSK_SYNC_1 0xD4

// -----------------------------------------------------------------------------
// Presets.
//
// A preset is a complete operating point - frequency, modem, power, and the
// duty-cycle limit of the sub-band it lands in - not just a frequency. The
// mode button steps through them, and both handsets stay in step because both
// operators press the button the same number of times.
//
// That matters more than it sounds. Two handsets that disagree about the
// MODULATION cannot hear each other at all, not even enough to notice the
// disagreement and complain about it - there is no negotiation channel here
// and no way to build one. Making the modem part of the thing the existing
// button already steps through is what keeps that from being a new way to get
// lost.
//
// The table itself is in link.cpp. This is only the count and the default.
// -----------------------------------------------------------------------------
#define VOICE_PRESET_COUNT 8

// Boot preset, an index into VOICE_PRESETS[]. 0 is 868.1 MHz LoRa.
#ifndef VOICE_DEFAULT_PRESET
#define VOICE_DEFAULT_PRESET 0
#endif

// -----------------------------------------------------------------------------
// ETSI EN 300 220 sub-bands, for duty-cycle accounting.
//
// The 1 % is assessed per sub-band, so the firmware has to keep a separate
// rolling hour for each. Transmitting on 868.1 does not spend any of the
// budget for 869.525, and lumping them together would make the g3 presets look
// far worse than they are - which is the exact opposite of useful, since g3 is
// the one place this application can actually be legal.
// -----------------------------------------------------------------------------
enum VoiceBand {
  VOICE_BAND_G = 0,   // 863.0 - 868.0    25 mW    1 %
  VOICE_BAND_G1,      // 868.0 - 868.6    25 mW    1 %
  VOICE_BAND_G3,      // 869.4 - 869.65  500 mW   10 %
  VOICE_BAND_G4,      // 869.7 - 870.0     5 mW  100 %
  VOICE_BAND_COUNT,
};

// =============================================================================
// Audio
// =============================================================================

// Codec2 sample rate. Not a choice - every Codec2 mode below 450PWB is 8 kHz.
#define VOICE_SAMPLE_RATE 8000

// Codec2 mode.
//
//   mode   frame    samples   bytes/frame   quality
//   -----  -------  -------   -----------   -------------------------------
//   3200    20 ms     160          8        best, but see the airtime table
//   1600    40 ms     320          8        default - clearly intelligible
//   700C    40 ms     320          4        robotic, but it gets through
//
// Whatever you pick here must also be enabled in platformio.ini's
// CODEC2_MODE_*_EN flags, or codec2_create() returns NULL.
#ifndef VOICE_CODEC_MODE
#define VOICE_CODEC_MODE CODEC2_MODE_1600
#endif

// Upper bounds, so every buffer in the firmware can be a fixed-size array and
// nothing in the audio path ever calls malloc(). 320 samples and 8 bytes cover
// all three modes above.
#define VOICE_MAX_SAMPLES_PER_FRAME 320
#define VOICE_MAX_BYTES_PER_FRAME   8

// Codec2 frames bundled into one LoRa packet.
//
// This is the central latency/efficiency trade. Every packet costs a fixed
// 12.25 symbols of preamble and header whatever it carries, so bundling more
// frames raises the ratio of audio to airtime - and adds exactly
// (frames x frame duration) to mouth-to-ear latency, because the first frame
// in a packet waits for the last.
#ifndef VOICE_FRAMES_PER_PACKET
#define VOICE_FRAMES_PER_PACKET 6
#endif

// =============================================================================
// AIRTIME AND DUTY CYCLE - read this before changing the modem or the codec
//
// Airtime for one packet carrying 9 header bytes plus VOICE_FRAMES_PER_PACKET
// codec frames. LoRa figures are the Semtech formula for explicit header, CRC
// on, 8-symbol preamble; FSK figures add the 4-byte preamble, 2-byte sync
// word, length byte and 2-byte CRC. The firmware prints what RadioLib actually
// measures for the active preset, as [link] airtime.
//
//   codec  frames  payload   LoRa SF7/125   FSK 50k   audio    duty (LoRa/FSK)
//   -----  ------  -------   ------------   -------   ------   ---------------
//   700C     6       33 B         72 ms        6 ms   240 ms      30 %  / 2.6 %
//   1600     6       57 B        108 ms       10 ms   240 ms      45 %  / 4.3 %
//   3200     6       57 B        108 ms       10 ms   120 ms      90 %  / 8.7 %
//
// "Duty while keyed" is the fraction of real time the PA is on while somebody
// is holding PTT. It has to stay comfortably under 100 % or the encoder
// outruns the transmitter, the outbound queue backs up and the far end falls
// further and further behind. Under about 60 % leaves room for the ~5 ms
// turnaround and the CPU cost of the codec.
//
// On LoRa, CODEC2_MODE_3200 therefore needs a wider channel:
// -DVOICE_BW_KHZ=250.0f halves every LoRa airtime above. On FSK it fits as it
// is, with room to spare - which is the point of having FSK presets.
//
// -----------------------------------------------------------------------------
// WHAT FSK BUYS, AND WHAT IT COSTS
//
// FSK is about ten times more airtime-efficient than LoRa SF7 for the same
// payload. It is not free: it has no processing gain, so the receiver is far
// less sensitive.
//
//   modem              sensitivity   vs LoRa SF7   airtime for one packet
//   ----------------   -----------   -----------   ----------------------
//   LoRa SF7 / BW125     -123 dBm         -              108 ms
//   FSK  25 kbit/s       -110 dBm      -13 dB              21 ms
//   FSK  50 kbit/s       -106 dBm      -17 dB              10 ms
//   FSK 100 kbit/s       -104 dBm      -19 dB               5 ms
//
// 17 dB is roughly a factor of seven in free-space range, and worse than that
// with terrain in the way. So FSK does not make this rig better; it converts
// a duty-cycle problem into a range problem. Which one you would rather have
// is the experiment.
//
// -----------------------------------------------------------------------------
// THE PART THAT IS NOT COMPLIANT, AND THE PRESET THAT IS
//
// ETSI EN 300 220 allows 1 % duty cycle in 868.0 - 868.6 MHz, assessed over any
// one-hour window. A handset at 45 % while keyed blows through that after
// about 80 seconds of talking per hour. Voice is not a 1 % application and no
// amount of tuning makes it one.
//
// The way out is the sub-band, not the modem. 869.4 - 869.65 MHz allows 10 %
// at 500 mW, and it costs nothing in sensitivity - it is the same radio on a
// different frequency. Putting FSK there as well is what actually closes the
// gap:
//
//   preset                          duty while keyed   limit    verdict
//   -----------------------------   ----------------   ------   -----------
//   868.1  LoRa SF7   (presets 0-3)       45 %           1 %    45x over
//   869.5  LoRa SF7   (preset 4)          45 %          10 %    4.5x over
//   869.5  FSK 50k    (preset 5)         4.3 %          10 %    COMPLIANT
//   869.9  FSK 50k    (preset 6)         4.3 %         100 %    always legal
//
// Preset 5 is the one to reach for if you want this to be defensible: legal
// duty cycle, legal power, and a few hundred metres of range instead of a few
// kilometres. Preset 6 trades nearly all the range for a band with no duty
// limit at all - 5 mW ERP is across a room.
//
// One trap on power: the SX1276's +20 dBm PA_BOOST mode is itself rated by
// Semtech for 1 % duty, so it is unusable here even though 869.4 - 869.65
// legally permits far more. +17 dBm is the practical ceiling for continuous
// operation, and that is what the g3 presets ask for.
//
// The firmware does not enforce any of this. It measures the rolling hour PER
// SUB-BAND, shows it against the active preset's limit, and works out how many
// seconds of talking are left. What you do with that is your business.
// =============================================================================

// -----------------------------------------------------------------------------
// Receive buffering
// -----------------------------------------------------------------------------

// Audio buffered before playback starts, milliseconds.
//
// A packet carries VOICE_FRAMES_PER_PACKET frames but arrives all at once, so
// playback empties the queue in bursts and refills it in steps. The pre-roll
// has to cover at least one whole packet plus the jitter between packets, or
// the first gap in the air becomes an audible gap in the speaker.
//
// This is the dominant term in mouth-to-ear latency. At the defaults:
//   240 ms filling the packet + 108 ms airtime + 320 ms pre-roll ~= 670 ms.
#ifndef VOICE_PREROLL_MS
#define VOICE_PREROLL_MS 320UL
#endif

// Depth of the queue between the radio task and the audio task, in frames.
// 64 x 40 ms is 2.5 s of slack - far more than the pre-roll, so a burst of
// late packets is absorbed rather than dropped.
#define VOICE_RX_QUEUE_FRAMES 64

// Depth of the outbound packet queue. Three packets of head room; if the
// transmitter falls further behind than that the oldest is dropped, because in
// a live voice stream stale audio is worse than missing audio.
#define VOICE_TX_QUEUE_PACKETS 3

// Silence after the last packet of a stream before the receiver decides the
// transmission is over and goes back to idle. Comfortably longer than one
// packet interval, so a single lost packet does not close the squelch.
#define VOICE_RX_STREAM_TIMEOUT_MS 1200UL

// =============================================================================
// Cue tones
//
// Short beeps that mark the edges of a transmission. On a half-duplex link the
// audio simply stops when the other station lets go, and "finished talking"
// and "fell off the air" sound identical - which on a marginal link is the
// difference between replying and repeating yourself.
//
// Four cues, and which end of the link generates each one matters:
//
//   KEYUP     LOCAL, on the talking handset when PTT is pressed. Confirms the
//             radio actually keyed, which is otherwise only visible on the LED
//             and the screen - no use with the radio on your belt. It plays
//             during the microphone settling delay, so it doubles as the "go
//             ahead" that stops you talking over your own dead time.
//   INCOMING  at the LISTENING handset when a transmission starts. Plays
//             inside the pre-roll wait, which is dead time that already
//             existed, so it costs nothing at all.
//   ROGER     at the LISTENING handset when the stream ends cleanly, i.e. a
//             packet arrived carrying VOICE_FLAG_END. "They finished, your
//             turn."
//   LOST      at the LISTENING handset when the stream just stops without an
//             end flag. "They dropped out - you did not hear the end of that."
//
// -----------------------------------------------------------------------------
// WHY THE ROGER BEEP IS NOT TRANSMITTED
//
// The obvious implementation is to append a tone to the outgoing audio. Every
// reason not to points the same way:
//
//   1. A transmitted beep cannot tell you the one thing worth knowing. If the
//      link fails, the beep fails with it - so its absence means either "they
//      are still talking" or "they are gone", which is exactly the ambiguity
//      it was supposed to resolve. Deciding at the RECEIVER lets a missing end
//      flag produce a DIFFERENT sound rather than no sound.
//   2. Codec2 models a human vocal tract. Pure tones come out of it warbling
//      and unpredictable - a synthesised tone at the receiver is clean every
//      time, because it never goes near the codec.
//   3. It would cost airtime on a link that is already over its duty budget.
//
// Nothing extra goes on the air for any of this. VOICE_FLAG_END already exists
// in the packet header, and it already means what the roger beep needs it to.
// =============================================================================
#ifndef VOICE_CUE_TONES
#define VOICE_CUE_TONES 1
#endif

// Cue amplitude, as a fraction of full scale in percent.
//
// Deliberately below speech level. A beep that is louder than the voice it is
// punctuating is a beep you will turn off within the hour.
#ifndef VOICE_CUE_LEVEL_PCT
#define VOICE_CUE_LEVEL_PCT 35
#endif

// =============================================================================
// Station identity
//
// One byte in the packet header, so the receiver can say who is talking. With
// two handsets that is a nicety; with three it stops being one.
//
// Left undefined, it is derived from the board's factory MAC, so two boards
// flashed from the same image still come up with different identities and
// nobody has to remember to configure anything. Define it to pin a board to a
// number you have written on its case.
//
// Zero is reserved as "not set" and is never generated.
// =============================================================================
// #define VOICE_STATION_ID 7

// =============================================================================
// Self test
//
// Preset 7 is not a radio channel. It records while PTT is held, then plays
// the recording back through the amplifier when you let go - microphone, I2S
// capture, Codec2 encode, Codec2 decode, I2S playback and the direction switch
// between them, all exercised with the radio in standby.
//
// It exists because it is the tool you want on a board you have just soldered,
// and because it is the one test that isolates the audio chain from the link.
// If speech comes back recognisable here and the far end still hears nothing,
// the problem is the radio, and vice versa.
//
// It costs almost nothing to keep the recording, because what is stored is
// encoded frames, not samples: ten seconds of Codec2 1600 is 2000 bytes.
// =============================================================================
#ifndef VOICE_SELFTEST_MS
#define VOICE_SELFTEST_MS 10000UL
#endif

// Worst case is the shortest frame period of any supported mode (20 ms, for
// CODEC2_MODE_3200), which sets the size of the fixed buffer.
#define VOICE_SELFTEST_MAX_FRAMES (VOICE_SELFTEST_MS / 20UL)

// -----------------------------------------------------------------------------
// Microphone
// -----------------------------------------------------------------------------

// Settling time after the bit clock starts, milliseconds.
//
// The INMP441 needs its internal filters to converge before its output means
// anything; the first samples out of a cold start are a large DC step. Feeding
// that to Codec2 makes every transmission open with a thud. We clock the part
// for this long and throw the samples away.
#define VOICE_MIC_SETTLE_MS 60UL

// Fixed gain applied to microphone samples before the codec, as a left shift.
//
// The INMP441 is a 24-bit part and we take the top 16 bits, which leaves
// ordinary speech at maybe 5 % of full scale - too quiet for Codec2's pitch
// estimator to lock onto. Shifting left by 3 (x8) puts a normal speaking voice
// at a sensible level. Raise it if you have to shout, lower it if the VU meter
// pins. Clipping saturates rather than wrapping; see audio.cpp.
#ifndef VOICE_MIC_GAIN_SHIFT
#define VOICE_MIC_GAIN_SHIFT 3
#endif

// =============================================================================
// Encryption
//
// AES-128 in counter mode, keyed from [voice] in secrets.ini. Off at boot
// unless VOICE_ENCRYPT_ON_BOOT is defined; the mode button's double press
// toggles it.
//
// What this gives you: a listener with an SDR and a matching LoRa receiver
// hears noise instead of your conversation.
//
// What it does NOT give you, and none of it is an oversight:
//   - No authentication. CTR mode is malleable. Anyone can flip bits in a
//     packet and the far end will decode the result and play it. The LoRa CRC
//     catches accidental corruption, not deliberate corruption.
//   - No key exchange and no forward secrecy. One static key, compiled into
//     both images. Whoever reads the flash has it, forever.
//   - No replay protection. A recorded packet plays again if it is resent.
//
// It is a lab toy for watching a stream go from intelligible to noise when you
// double-tap a button. Do not put anything behind it that matters.
// =============================================================================

#define VOICE_KEY_BYTES 16

#ifndef VOICE_KEY_HEX
#define VOICE_KEY_HEX "00000000000000000000000000000000"
#endif

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------
#define DISPLAY_REFRESH_MS 250UL
