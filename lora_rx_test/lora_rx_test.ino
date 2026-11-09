// Minimal LoRa RX test - listens for packets and prints data
#include <Arduino.h>
#include <RadioLib.h>

// SX1262 pins for Heltec V3
#define SX1262_CS       17
#define SX1262_DIO1     18
#define SX1262_RST      16
#define SX1262_BUSY     20
#define SX1262_TX       21
#define SX1262_RX       22

SX1262 radio = new Module(SX1262_CS, SX1262_DIO1, SX1262_RST, SX1262_BUSY);

uint32_t rxCount = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== LoRa RX Test ===");

  int state = radio.begin(915.0);
  Serial.printf("begin: %d\n", state);

  state = radio.setSpreadingFactor(7);
  Serial.printf("setSF: %d\n", state);

  state = radio.setBandwidth(250.0);
  Serial.printf("setBW: %d\n", state);

  state = radio.setCodingRate(5);
  Serial.printf("setCR: %d\n", state);

  state = radio.setSyncWord(0x12);
  Serial.printf("setSync: %d\n", state);

  state = radio.setPreambleLength(16);
  Serial.printf("setPreamble: %d\n", state);

  // Don't start continuous receive - use timed receive in loop
  Serial.println("RX test ready - waiting for packets");
}

void loop() {
  uint8_t payload[64];
  int state = radio.receive(payload, 64, 1000);

  if (state == RADIOLIB_ERR_NONE) {
    rxCount++;
    Serial.printf("RX %u: len=%d data=", rxCount, sizeof(payload));
    for (int i = 0; i < 4; i++) {
      Serial.printf("%02X ", payload[i]);
    }
    Serial.println();
  } else if (state != RADIOLIB_IRQ_TIMEOUT) {
    Serial.printf("RX ERR %d\n", state);
  }
}
