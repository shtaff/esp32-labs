// =============================================================================
// radio_hw.h - the single SX1276 instance and its SPI bus.
//
// The radio sits on VSPI with a non-default pin mapping, so SPI.begin() must be
// given the board's pins explicitly before RadioLib touches the chip. Getting
// this wrong is silent: every SPI read returns 0xFF and begin() simply reports
// that the chip is missing.
// =============================================================================
#pragma once

#include <RadioLib.h>

extern SX1276 radio;

// Bring up VSPI on the board's pins and reset the radio. Returns false if the
// SX1276 does not answer, in which case there is nothing useful the firmware
// can do beyond reporting it.
bool radioHwBegin();
