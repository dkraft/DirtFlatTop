// Minimal LoRa TX test - sends counter packet every 2s
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

uint32_t txCount = 0;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== LoRa TX Test ===");

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

  state = radio.setOutputPower(22, true);
  Serial.printf("setPower: %d\n", state);

  state = radio.setPreambleLength(16);
  Serial.printf("setPreamble: %d\n", state);

  Serial.println("TX test ready");
}

void loop() {
  uint8_t payload[4];
  payload[0] = 0xAA;
  payload[1] = 0x55;
  payload[2] = (uint8_t)(txCount >> 8);
  payload[3] = (uint8_t)(txCount & 0xFF);

  Serial.printf("TX %u starting...\n", txCount);
  int state = radio.transmit(payload, 4);
  Serial.printf("transmit returned: %d\n", state);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.printf("TX %u OK\n", txCount);
  } else {
    Serial.printf("TX %u ERR %d\n", txCount, state);
  }
  txCount++;
  delay(2000);
}
