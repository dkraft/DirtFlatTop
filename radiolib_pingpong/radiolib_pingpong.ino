/*
  RadioLib SX1262 Ping-Pong Test for Heltec WiFi LoRa 32 V3
  TX node: sends "Hello World!" every 2 seconds
  RX node: listens and prints received packets
*/

#include <RadioLib.h>

// SX1262 pinout for Heltec WiFi LoRa 32 V3
// NSS: 17, DIO1: 18, NRST: 16, BUSY: 20
SX1262 radio = new Module(17, 18, 16, 20);

// Uncomment on TX node
#define TX_NODE

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== RadioLib PingPong Test ===");

  // Initialize SPI explicitly
  SPI.begin(14, 15, 13, 17);
  Serial.println("[SPI] Initialized");

  // Initialize at 915 MHz
  Serial.print("[Radio] Initializing ... ");
  ConfigLoRa_t config;
  config.frequency = 915.0;
  int state = radio.begin(config);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("success!");
  } else {
    Serial.print("failed, code ");
    Serial.println(state);
    while (true) { delay(10); }
  }

#ifdef TX_NODE
  Serial.println("[TX] Starting transmission loop...");
#else
  Serial.print("[RX] Starting to listen ... ");
  state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println("success!");
  } else {
    Serial.print("failed, code ");
    Serial.println(state);
    while (true) { delay(10); }
  }
#endif
}

void loop() {
#ifdef TX_NODE
  static unsigned long lastTx = 0;
  if (millis() - lastTx >= 2000) {
    lastTx = millis();
    Serial.print("[TX] Sending packet ... ");
    int state = radio.transmit("Hello World!");
    if (state == RADIOLIB_ERR_NONE) {
      Serial.println("success!");
    } else {
      Serial.print("failed, code ");
      Serial.println(state);
    }
  }
#else
  int state = radio.receive();
  if (state == RADIOLIB_ERR_NONE) {
    String str;
    radio.readData(str);
    Serial.println("[RX] Received packet!");
    Serial.print("[RX] Data: ");
    Serial.println(str);
    Serial.print("[RX] RSSI: ");
    Serial.print(radio.getRSSI());
    Serial.println(" dBm");
  }
#endif
}
