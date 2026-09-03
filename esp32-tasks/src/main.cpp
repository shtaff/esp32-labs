#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

#define BUTTON_PIN 14

// button poll ms
#define DOUBLE_CLICK_TIME 200
#define DEBOUNCE_TIME 70
#define POLL_TIME 10

// radio pins
#define SPI_SCK 5
#define SPI_MISO 19
#define SPI_MOSI 27
#define LORA_CS 18
#define LORA_RST 23
#define LORA_DIO0 26

#define MAX_RADIO_MESSAGE_SIZE 32

struct ButtonQueueMessage { uint32_t eventTime; uint8_t event; };
struct RadioQueueMessage { 
  uint32_t initialEventTime; 
  uint32_t eventTime; 
  char event[MAX_RADIO_MESSAGE_SIZE];

  RadioQueueMessage() {
  }
  
  RadioQueueMessage(uint32_t initialEventTime, uint32_t eventTime, char* sourceEvent) {
    this->initialEventTime = initialEventTime;
    this->eventTime = eventTime;

    strncpy(event, sourceEvent, MAX_RADIO_MESSAGE_SIZE - 1);
    event[MAX_RADIO_MESSAGE_SIZE - 1] = '\0';
  }
};

SX1278 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, RADIOLIB_NC);

QueueHandle_t buttonQueue;
QueueHandle_t radioQueue;

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

void radioTask(void *pvParameters) {
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, LORA_CS);

  int16_t radioState = radio.begin();

  if (radioState == RADIOLIB_ERR_NONE) {
    Serial.println("[Radio Task] Radio initialized successfully");

    radio.setFrequency(868.0);
    radio.setSpreadingFactor(7);
    radio.setBandwidth(125); 
  } else {
    Serial.printf("[Radio Task] Radio initialization error: %d\n", radioState);
    vTaskDelete(NULL);
  }

  RadioQueueMessage queueMessage;

  while (1) {
    if (xQueueReceive(radioQueue, &queueMessage, portMAX_DELAY) == pdPASS) {
      uint32_t radioTime = micros();
      Serial.printf("[Radio Task]  [%09d] Message transmit starting at %d: %s\n", queueMessage.initialEventTime, radioTime, queueMessage.event);

      int16_t transmitResult = radio.transmit(queueMessage.event);

      if (transmitResult == RADIOLIB_ERR_NONE)
      {
        radioTime = micros();
        Serial.printf("[Radio Task]  [%09d] Message transmitted at %d: %s\n", queueMessage.initialEventTime, radioTime, queueMessage.event);
      } else {
        Serial.printf("[Radio Task]  [%09d] Message failed at %d: status %d\n", radioTime, queueMessage.initialEventTime, transmitResult);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  // delay for serial to connect
  delay(200);
  Serial.println("[Init] LoRa tasks test starting...");

  buttonQueue = xQueueCreate(5, sizeof(ButtonQueueMessage));
  radioQueue = xQueueCreate(5, sizeof(RadioQueueMessage));

  if (buttonQueue == NULL || radioQueue == NULL) {
    Serial.println("[Init] Failed to create queues...");
    return;
  }

  Serial.println("[Init] Queues created.");

  xTaskCreate(buttonTask, "Button task", 2048, NULL, 1, NULL);
  xTaskCreate(radioTask, "Radio task", 4096, NULL, 2, NULL);

  Serial.println("[Init] Tasks created.");
  Serial.println("[Init] Init done.");
}

void loop() {
  ButtonQueueMessage btMessage;

  if (xQueueReceive(buttonQueue, &btMessage, portMAX_DELAY) == pdPASS) {
    char textMessage[MAX_RADIO_MESSAGE_SIZE];

    if (btMessage.event == 1) {
      strcpy(textMessage, "SINGLE CLICK");
    } else if (btMessage.event == 2) {
      strcpy(textMessage, "DOUBLE CLICK");
    }

    uint32_t buttonReceiveTime = micros();

    RadioQueueMessage radioQueueMessage = {btMessage.eventTime, buttonReceiveTime, textMessage};
    xQueueSend(radioQueue, &radioQueueMessage, portMAX_DELAY);

    Serial.printf("[Main Loop]   [%09d] Sent message to radio at %d: %s\n", radioQueueMessage.initialEventTime, radioQueueMessage.eventTime, radioQueueMessage.event);
  }
}