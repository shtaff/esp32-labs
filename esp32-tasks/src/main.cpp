#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_task_wdt.h>
#include <RadioLib.h>
#include <SPI.h>

#define BUTTON_PIN 14

// button poll ms
#define DOUBLE_CLICK_TIME 200
#define DEBOUNCE_TIME 70
#define POLL_TIME 10

// led blinking config
#define LED_PERIOD_CNT 4
#define LED_PERIOD1 250
#define LED_PERIOD2 500
#define LED_PERIOD3 1000
#define LED_PERIOD4 2000

// hang when switching to this, set anything not in period list to not hang
#define LED_PERIOD_HANG -1

// LED blinks at this period while a radio transmit is in progress
#define TX_BLINK_PERIOD_MS 100

// if ledTask sees no TX_OFF within this long after a TX_ON, it assumes the
// radio task crashed/hung and resets to normal blinking on its own. Actual
// airtime for SF11/BW125/32B is well under 1s, so this leaves a big margin.
#define TX_TIMEOUT_MS 3000

// onboard screen
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_I2C_ADDR 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

// radio pins
#define SPI_SCK 5
#define SPI_MISO 19
#define SPI_MOSI 27
#define LORA_CS 18
#define LORA_RST 23
#define LORA_DIO0 26

#define RADIO_SF 11   // high SF = slow airtime, so the TX blink is easy to see
#define RADIO_PAYLOAD_LEN 32

#define BUTTON_TASK_CORE 0
#define LED_TASK_CORE 1
#define DISPLAY_TASK_CORE BUTTON_TASK_CORE
#define RADIO_TASK_CORE LED_TASK_CORE

enum LedCommand : uint8_t {
  LED_CMD_BTN_1  = 1,  // single click - cycle period forward
  LED_CMD_BTN_2  = 2,  // double click - cycle period backward
  LED_CMD_TX_ON  = 3,  // radio transmit started - blink fast, ignore BTN_1/BTN_2
  LED_CMD_TX_OFF = 4,  // radio transmit finished (or timed out) - resume normal blinking
};

struct LedQueueMessage { uint32_t eventTime; LedCommand command; };
struct RadioTriggerMessage { uint32_t eventTime; };
struct DisplayMessage { uint8_t periodIndex; bool txActive; };

uint16_t periods[LED_PERIOD_CNT] = {LED_PERIOD1, LED_PERIOD2, LED_PERIOD3, LED_PERIOD4};

QueueHandle_t ledCmdQueue;
QueueHandle_t displayQueue;
QueueHandle_t radioTriggerQueue;

// only used to simulate a hang when switching to the 1000ms period, see ledTask
SemaphoreHandle_t demoMutex;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
SX1278 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, RADIOLIB_NC);

void buttonTask(void *pvParameters) {
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  while(1) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      // debounce press
      vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_TIME));

      // wait for release
      while (digitalRead(BUTTON_PIN) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
      }

      // count consecutive clicks: 1 = single, 2 = double, 3 = triple
      uint8_t clickCount = 1;
      while (clickCount < 3) {
        bool nextClickDetected = false;
        uint32_t waitTime = 0;

        while (waitTime < DOUBLE_CLICK_TIME) {
          if (digitalRead(BUTTON_PIN) == LOW) {
            nextClickDetected = true;
            break;
          }
          waitTime += POLL_TIME;
          vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
        }

        if (!nextClickDetected) break;

        // wait for release
        while (digitalRead(BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
        }

        clickCount++;
      }

      uint32_t eventTime = micros();

      if (clickCount == 3) {
        RadioTriggerMessage triggerMessage = {eventTime};
        xQueueSend(radioTriggerQueue, &triggerMessage, portMAX_DELAY);
        Serial.printf("[Button Task] [%09d] Triple click - sent radio trigger\n", eventTime);
      } else {
        LedCommand command = (clickCount == 1) ? LED_CMD_BTN_1 : LED_CMD_BTN_2;
        LedQueueMessage ledMessage = {eventTime, command};
        xQueueSend(ledCmdQueue, &ledMessage, portMAX_DELAY);
        Serial.printf("[Button Task] [%09d] Sent LED command: %d\n", eventTime, command);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
  }
}

// moves periodIndex by a signed step count, wrapping around LED_PERIOD_CNT
void applyPeriodSteps(uint8_t &periodIndex, int8_t steps) {
  int16_t newIndex = ((int16_t)periodIndex + steps) % LED_PERIOD_CNT;
  if (newIndex < 0) newIndex += LED_PERIOD_CNT;
  periodIndex = (uint8_t)newIndex;
}

void ledTask(void *pvParameters) {
  pinMode(LED_BUILTIN, OUTPUT);

  // subscribe to the Task Watchdog Timer
  esp_task_wdt_add(NULL);

  uint8_t periodIndex = 0;
  bool ledOn = false;
  bool txActive = false;
  uint32_t txStartMillis = 0;
  LedQueueMessage ledMessage;

  // initial disaply
  DisplayMessage initialDisplay = {periodIndex, false};
  xQueueSend(displayQueue, &initialDisplay, portMAX_DELAY);

  while (1) {
    uint16_t waitMs = txActive ? TX_BLINK_PERIOD_MS : periods[periodIndex];

    // waits for the next command; if one arrives in time it is applied,
    // otherwise the wait times out and the LED keeps blinking at previous speed
    if (xQueueReceive(ledCmdQueue, &ledMessage, pdMS_TO_TICKS(waitMs)) == pdPASS) {
      if (ledMessage.command == LED_CMD_TX_ON) {
        txActive = true;
        txStartMillis = millis();
        Serial.println("[LED Task] TX_ON - blinking fast until TX_OFF");
      } else if (ledMessage.command == LED_CMD_TX_OFF) {
        txActive = false;
        Serial.println("[LED Task] TX_OFF - resuming normal blinking");
      } else if (ledMessage.command == LED_CMD_BTN_1) {
        // single click - cycle forward
        applyPeriodSteps(periodIndex, 1);
        Serial.printf("[LED Task] Period switched to %d ms\n", periods[periodIndex]);
      } else if (ledMessage.command == LED_CMD_BTN_2) {
        // double click - cycle back
        applyPeriodSteps(periodIndex, -1);
        Serial.printf("[LED Task] Period switched to %d ms\n", periods[periodIndex]);
      }

      DisplayMessage displayMessage = {periodIndex, txActive};
      xQueueSend(displayQueue, &displayMessage, portMAX_DELAY);

      if (periods[periodIndex] == LED_PERIOD_HANG &&
          (ledMessage.command == LED_CMD_BTN_1 || ledMessage.command == LED_CMD_BTN_2)) {
        // xSemaphoreTake is not recursive so second call will hang
        Serial.println("\n\n=== SIMULATING HANG ON PURPOSE (switched to 1000ms) ===\n");
        vTaskDelay(pdMS_TO_TICKS(100));  // flush serial

        xSemaphoreTake(demoMutex, portMAX_DELAY);
        xSemaphoreTake(demoMutex, portMAX_DELAY);
      }
    }

    // radio task never reported TX_OFF in time - assume it crashed and recover
    if (txActive && (millis() - txStartMillis > TX_TIMEOUT_MS)) {
      Serial.println("[LED Task] TX timeout - assuming radio task crashed, resetting to TX_OFF");
      txActive = false;

      DisplayMessage displayMessage = {periodIndex, false};
      xQueueSend(displayQueue, &displayMessage, portMAX_DELAY);
    }

    esp_task_wdt_reset();
    ledOn = !ledOn;
    digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
  }
}

void drawPeriods(uint8_t periodIndex, bool txActive) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("LED PERIOD");

  if (txActive) {
    // right-aligned in the header row, 6px per char at text size 1
    display.setCursor(OLED_WIDTH - 2 * 6, 0);
    display.print("TX");
  }

  display.drawFastHLine(0, 9, OLED_WIDTH, SSD1306_WHITE);

  for (uint8_t i = 0; i < LED_PERIOD_CNT; i++) {
    bool selected = (i == periodIndex);
    int16_t y = 13 + i * 10;

    char label[16];
    snprintf(label, sizeof(label), "%s%u ms", selected ? "> " : "  ", periods[i]);

    display.setCursor(0, y);
    display.print(label);
  }

  display.drawFastHLine(0, 53, OLED_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 55);
  display.print("1x:fwd  2x:back");

  display.display();
}

void displayTask(void *pvParameters) {
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR, true, false)) {
    Serial.println("[Display Task] SSD1306 init failed");
    vTaskDelete(NULL);
  }

  DisplayMessage displayMessage;

  while (1) {
    if (xQueueReceive(displayQueue, &displayMessage, portMAX_DELAY) == pdPASS) {
      drawPeriods(displayMessage.periodIndex, displayMessage.txActive);
    }
  }
}

void radioTask(void *pvParameters) {
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, LORA_CS);

  int16_t radioState = radio.begin();

  if (radioState == RADIOLIB_ERR_NONE) {
    Serial.println("[Radio Task] Radio initialized successfully");

    radio.setFrequency(868.0);
    radio.setSpreadingFactor(RADIO_SF);
    radio.setBandwidth(125);
  } else {
    Serial.printf("[Radio Task] Radio initialization error: %d\n", radioState);
    vTaskDelete(NULL);
  }

  RadioTriggerMessage triggerMessage;
  uint32_t txCounter = 0;

  while (1) {
    if (xQueueReceive(radioTriggerQueue, &triggerMessage, portMAX_DELAY) == pdPASS) {
      LedQueueMessage txOn = {(uint32_t)micros(), LED_CMD_TX_ON};
      xQueueSend(ledCmdQueue, &txOn, portMAX_DELAY);

      char payload[RADIO_PAYLOAD_LEN];
      memset(payload, 0, sizeof(payload));
      snprintf(payload, sizeof(payload), "TRIPLE CLICK TX #%lu", (unsigned long)(++txCounter));

      Serial.printf("[Radio Task] Transmitting (SF%d): %s\n", RADIO_SF, payload);
      int16_t transmitResult = radio.transmit((uint8_t*)payload, RADIO_PAYLOAD_LEN);

      if (transmitResult == RADIOLIB_ERR_NONE) {
        Serial.println("[Radio Task] Transmit done");
      } else {
        Serial.printf("[Radio Task] Transmit failed: status %d\n", transmitResult);
      }

      LedQueueMessage txOff = {(uint32_t)micros(), LED_CMD_TX_OFF};
      xQueueSend(ledCmdQueue, &txOff, portMAX_DELAY);
    }
  }
}

void setup() {
  Serial.begin(115200);
  // delay for serial to connect
  delay(200);
  Serial.println("[Init] LED period task test starting...");

  ledCmdQueue = xQueueCreate(5, sizeof(LedQueueMessage));
  displayQueue = xQueueCreate(5, sizeof(DisplayMessage));
  radioTriggerQueue = xQueueCreate(2, sizeof(RadioTriggerMessage));
  demoMutex = xSemaphoreCreateMutex();

  if (ledCmdQueue == NULL || displayQueue == NULL || radioTriggerQueue == NULL || demoMutex == NULL) {
    Serial.println("[Init] Failed to create queues...");
    return;
  }

  Serial.println("[Init] Queues created.");

  xTaskCreatePinnedToCore(buttonTask, "Button task", 2048, NULL, 3, NULL, BUTTON_TASK_CORE);
  xTaskCreatePinnedToCore(ledTask, "LED task", 2048, NULL, 1, NULL, LED_TASK_CORE);
  xTaskCreatePinnedToCore(displayTask, "Display task", 4096, NULL, 1, NULL, DISPLAY_TASK_CORE);
  xTaskCreatePinnedToCore(radioTask, "Radio task", 4096, NULL, 1, NULL, RADIO_TASK_CORE);

  Serial.println("[Init] Tasks created.");
  Serial.println("[Init] Init done.");
}

void loop() {
  // destroy main loop task to not consume resources
  vTaskDelete(NULL);
}
