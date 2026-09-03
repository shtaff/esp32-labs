#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_task_wdt.h>

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
#define LED_PERIOD_HANG LED_PERIOD3

// onboard screen
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_I2C_ADDR 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

#define BUTTON_TASK_CORE 0
#define LED_TASK_CORE 1
#define DISPLAY_TASK_CORE BUTTON_TASK_CORE

struct ButtonQueueMessage { uint32_t eventTime; uint8_t event; };

uint16_t periods[LED_PERIOD_CNT] = {LED_PERIOD1, LED_PERIOD2, LED_PERIOD3, LED_PERIOD4};

QueueHandle_t buttonQueue;
QueueHandle_t displayQueue;

// only used to simulate a hang when switching to the 1000ms period, see ledTask
SemaphoreHandle_t demoMutex;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

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

      bool doubleClickDetected = false;
      uint32_t waitTime = 0;

      // wait for second click
      while(waitTime < DOUBLE_CLICK_TIME) {
        if (digitalRead(BUTTON_PIN) == LOW) {
          doubleClickDetected = true;
          break;
        }
        waitTime += POLL_TIME;
        vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
      }

      uint8_t event = 0;
      if (doubleClickDetected) {
        // wait for second click release
        while (digitalRead(BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
        }

        event = 2;
      } else {
        event = 1;
      }

      uint32_t eventTime = micros();
      ButtonQueueMessage btMessage = {eventTime, event};

      xQueueSend(buttonQueue, &btMessage, portMAX_DELAY);
      Serial.printf("[Button Task] [%09d] Sent button message: %d\n", btMessage.eventTime, btMessage.event);
    }

    vTaskDelay(pdMS_TO_TICKS(POLL_TIME));
  }
}

void ledTask(void *pvParameters) {
  pinMode(LED_BUILTIN, OUTPUT);

  // subscribe to the Task Watchdog Timer
  esp_task_wdt_add(NULL);

  uint8_t periodIndex = 0;
  bool ledOn = false;
  ButtonQueueMessage btMessage;

  // initial disaply
  xQueueSend(displayQueue, &periodIndex, portMAX_DELAY);

  while (1) {
    // waits for the period update; if a click arrives in time it is applied,
    // otherwise the wait times out and the LED keeps blinking at previous speed
    if (xQueueReceive(buttonQueue, &btMessage, pdMS_TO_TICKS(periods[periodIndex])) == pdPASS) {
      if (btMessage.event == 1) {
        // single click - cycle forward
        periodIndex = (periodIndex + 1) % LED_PERIOD_CNT;
      } else if (btMessage.event == 2) {
        // double click - cycle back
        periodIndex = (periodIndex + LED_PERIOD_CNT - 1) % LED_PERIOD_CNT;
      }

      Serial.printf("[LED Task] Period switched to %d ms\n", periods[periodIndex]);
      xQueueSend(displayQueue, &periodIndex, portMAX_DELAY);

      if (periods[periodIndex] == LED_PERIOD_HANG) {
        // xSemaphoreTake is not recursive so second call will hang
        Serial.println("\n\n=== SIMULATING HANG ON PURPOSE (switched to 1000ms) ===\n");
        vTaskDelay(pdMS_TO_TICKS(100));  // flush serial

        xSemaphoreTake(demoMutex, portMAX_DELAY);
        xSemaphoreTake(demoMutex, portMAX_DELAY);
      }
    }

    esp_task_wdt_reset();
    ledOn = !ledOn;
    digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
  }
}

void drawPeriods(uint8_t periodIndex) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("LED PERIOD");
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

  uint8_t periodIndex;

  while (1) {
    if (xQueueReceive(displayQueue, &periodIndex, portMAX_DELAY) == pdPASS) {
      drawPeriods(periodIndex);
    }
  }
}

void setup() {
  Serial.begin(115200);
  // delay for serial to connect
  delay(200);
  Serial.println("[Init] LED period task test starting...");

  buttonQueue = xQueueCreate(5, sizeof(ButtonQueueMessage));
  displayQueue = xQueueCreate(5, sizeof(uint8_t));
  demoMutex = xSemaphoreCreateMutex();

  if (buttonQueue == NULL || displayQueue == NULL || demoMutex == NULL) {
    Serial.println("[Init] Failed to create queues...");
    return;
  }

  Serial.println("[Init] Queues created.");

  xTaskCreatePinnedToCore(buttonTask, "Button task", 2048, NULL, 3, NULL, BUTTON_TASK_CORE);
  xTaskCreatePinnedToCore(ledTask, "LED task", 2048, NULL, 1, NULL, LED_TASK_CORE);
  xTaskCreatePinnedToCore(displayTask, "Display task", 4096, NULL, 1, NULL, DISPLAY_TASK_CORE);

  Serial.println("[Init] Tasks created.");
  Serial.println("[Init] Init done.");
}

void loop() {
  // destroy main loop task to not consume resources
  vTaskDelete(NULL);
}
