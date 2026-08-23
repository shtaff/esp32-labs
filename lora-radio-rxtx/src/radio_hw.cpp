#include "radio_hw.h"

#include <Arduino.h>
#include <SPI.h>

#include "config.h"
#include "radio_profiles.h"
#include "schedule.h"

// Module(cs, irq, rst, gpio). DIO0 is the interrupt line for both TxDone and
// RxDone on the SX127x family; DIO1/DIO2 are not needed for this application.
SX1276 radio = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, RADIOLIB_NC);

bool radioHwBegin() {
  // The default VSPI mapping is SCK 18 / MISO 19 / MOSI 23 / SS 5, which is not
  // how this board is routed. Pass the real pins.
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);

  // Probe with the first profile in the table; whichever mode the application
  // wants is applied properly a moment later.
  int16_t state = radioApplyProfile(radio, RADIO_PROFILES[0]);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] SX1276 init failed, RadioLib code %d\n", state);
    Serial.println("[radio] check SPI wiring: SCK 5, MISO 19, MOSI 27, CS 18, RST 23");
    return false;
  }

  Serial.printf("[radio] SX1276 up on %.3f MHz at %d dBm\n",
                (double)RADIO_FREQ_MHZ, (int)RADIO_POWER_DBM);

  // Measure every profile's airtime. The receiver needs it to know when an
  // expected packet should have finished arriving, and the schedule uses it
  // only to check that each profile fits the share of the slot it is given.
  radioCacheAirtimes(radio);
  scheduleReportAirtimes(radioAirtimeTable(), RADIO_PROFILE_COUNT);

  return true;
}
