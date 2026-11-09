/**
 * DirtBeetle (DB) v1 — Identical clone firmware for all units.
 *
 * Role is elected at runtime:
 *   - Silence during listen → Stationary Base
 *   - Hear Base beacon     → Monitoring DB
 *   - PRG held at end of settle → force Base
 *
 * LoRa: RadioLib SX1262, US915, SF7/BW250/CR4/5, sync 0x12, +20 dBm
 * Sensor: BMP585 at 0x47 on Wire1 (SDA=19, SCL=20, INT=47)
 * Display: Heltec HT_SSD1306Wire OLED
 * BLE: NimBLE Nordic UART service (stub for APK)
 *
 * Scale: 1:1 true inches (differential hypsometric).
 * Lock: >+24.0" from zero → OLED "Locked", latch STATE_LOCKED.
 */

#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Esp.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include "HT_SSD1306Wire.h"
#include "DFRobot_BMP58X.h"

/* ============ pins ============ */
#define BMP_SDA      19
#define BMP_SCL      20
#define BMP_INT      47
#define BMP_ADDR     0x47
#define PRG_PIN      0

/* ============ LoRa PHY constants ============ */
#define DB_FREQUENCY  915.0f
#define DB_SF         7
#define DB_BW         250
#define DB_CR         5
#define DB_SYNC       0x12
#define DB_PREAMBLE   8
#define DB_TX_POWER   0  // bench power; range/regulatory configuration not yet validated

/* ============ protocol constants ============ */
#define DB_MAGIC      0xDB01
#define DB_VER        1
#define ROLE_STATIONARY 0x01
#define ROLE_MONITOR    0x02
#define STATE_LOCKED    0x04
#define STATE_SETTLING  0x08
#define DB_TX_INTERVAL_MS 200
#define DB_JITTER_MS      80
#define DB_BASE_TIMEOUT_MS 2500
#define DB_LISTEN_MS      2000
#define DB_LISTEN_JITTER  400
#define DB_LOCK_INCHES    24.0f
#define MAX_BASES         8

/* ============ BLE UUIDs (Nordic UART) ============ */
static const char* DB_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char* DB_RX_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char* DB_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

/* ============ packet ============ */
struct __attribute__((packed)) DbPacket {
  uint16_t magic;
  uint8_t  ver;
  uint8_t  flags;
  uint32_t uid;
  uint16_t seq;
  uint32_t press_mPa;
  uint16_t pa_std_cPa;
  int16_t  height_tenth_in;
  uint8_t  soft_crc;
};

static_assert(sizeof(DbPacket) == 19, "DbPacket must be 19 bytes");

/* ============ base entry (monitor side) ============ */
struct BaseEntry {
  uint32_t uid;
  float    pa;
  uint16_t stability_cPa;
  bool     locked;
  int8_t   rssi;
  unsigned long lastSeenMs;
};

/* ============ globals ============ */
TwoWire bmpWire(1);
DFRobot_BMP58X_I2C bmp(&bmpWire, BMP_ADDR);
static SSD1306Wire display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED);

SX1262 radio = new Module(8, 14, 12, 13);

volatile bool bmp_irq = false;
float livePa = 101325.0f;
float liveC = 20.0f;
float refPa = 101325.0f;
float calibOffsetIn = 0.0f;
float smoothedPaEma = 101325.0f;
#define EMA_ALPHA 0.1f
int16_t seq = 0;
unsigned long lastTxMs = 0;
unsigned long lastBleMs = 0;

/* diagnostics counters */
uint32_t txOk = 0;
uint32_t txFail = 0;
uint32_t rxOk = 0;
int16_t lastRadioStatus = 0;
unsigned long lastRxMs = 0;
unsigned long lastDiagMs = 0;
volatile bool radioRxFlag = false;
volatile uint32_t radioRxIsrCount = 0;
uint32_t rxErrors = 0, rxRejects = 0;
float lastPacketRssi = 0, lastPacketSnr = 0;
void IRAM_ATTR radioRxIsr() { radioRxFlag = true; radioRxIsrCount++; }
static bool radioCheck(const char* op, int16_t state) {
  Serial.printf("radio %s=%d\n", op, state);
  lastRadioStatus = state;
  return state == RADIOLIB_ERR_NONE;
}
static void resumeRadioRx() {
  radioRxFlag = false;
  int16_t state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) { lastRadioStatus = state; rxErrors++; }
}

/* role: 0=undecided, 1=stationary base, 2=monitor */
uint8_t role = 0;
bool locked = false;

/* monitor's base table */
BaseEntry bases[MAX_BASES];
int baseCount = 0;
unsigned long lastBaseHearMs = 0;

/* stability window for base TX */
#define STAB_WIN 10
float stabBuf[STAB_WIN];
uint8_t stabHead = 0;
uint8_t stabCount = 0;

/* ============ helper: CRC8 ============ */
static uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
  }
  return crc;
}

/* ============ helper: device UID from MAC ============ */
static uint32_t getUid() {
  uint64_t mac = ESP.getEfuseMac();
  // ESP.getEfuseMac() is little-endian: low 32 bits omit distinguishing ae/b0 bytes!
  return (uint32_t)((mac >> 16) & 0xFFFFFFFFu);
}

/* ============ helper: altMeters (hypsometric) ============ */
static float altMeters(float pPa) {
  const float T0 = 288.15f, g = 9.80665f, L = 0.0065f, Rd = 287.058f, p0 = 101325.0f;
  float x = pPa / p0;
  if (x <= 0.0f) x = 0.001f;
  return (T0 / L) * (1.0f - powf(x, (Rd * L) / g));
}

/* ============ helper: inches delta (1:1 true scale) ============ */
static float inchesDelta(float pPa) {
  float dm = altMeters(pPa) - altMeters(refPa);
  return dm * 39.3701f;
}

/* ============ helper: hypsometric differential inches (1:1 true scale) ============ */
static float hypsometricInches(float localPa, float basePa) {
  float dm = altMeters(localPa) - altMeters(basePa);
  return dm * 39.3701f;
}

/* ============ stability tracking ============ */
static void stabPush(float pPa) {
  stabBuf[stabHead] = pPa;
  stabHead = (stabHead + 1) % STAB_WIN;
  if (stabCount < STAB_WIN) stabCount++;
}

static uint16_t stabStdCpa() {
  if (stabCount < 2) return 0;
  float sum = 0;
  for (uint8_t i = 0; i < stabCount; i++) sum += stabBuf[i];
  float mean = sum / stabCount;
  float sq = 0;
  for (uint8_t i = 0; i < stabCount; i++) {
    float d = stabBuf[i] - mean;
    sq += d * d;
  }
  float var = sq / stabCount;
  float std = sqrtf(var);
  return (uint16_t)fminf(std * 100.0f, 65535.0f);
}

/* smoothed pressure: exponential moving average */
static float smoothedPa() {
  return smoothedPaEma;
}

/* ============ BMP interrupt ============ */
void bmpIsr() {
  bmp_irq = true;
}

/* ============ BMP data drain ============ */
static void drainBmp() {
  while (bmp_irq) {
    bmp_irq = false;
    if (bmp.getIntStatus() & bmp.eIntDataReady) {
      float p = bmp.readPressPa();
      float t = bmp.readTempC();
      livePa = p;
      liveC = t;
      // Exponential moving average filter for smoother readings
      if (stabCount > 0) {
        smoothedPaEma = EMA_ALPHA * p + (1.0f - EMA_ALPHA) * smoothedPaEma;
      } else {
        smoothedPaEma = p;
      }
      stabPush(p);
    }
  }
}

/* ============ OLED: Grok Bot face animation ============ */
static void drawGrokFace(bool lookLeft) {
  display.clear();

  // circular face
  display.setColor(WHITE);
  display.fillCircle(64, 28, 22);

  // eyes (pupils)
  int eyeX = lookLeft ? 54 : 74;
  display.setColor(BLACK);
  display.fillCircle(eyeX, 24, 4);
  display.fillCircle(74 - (eyeX - 54), 24, 4);

  // mouth
  display.drawLine(56, 34, 72, 34);

  // "scanning" text
  display.setColor(WHITE);
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 52, "scanning");

  display.display();
}

/* ============ OLED: role states ============ */
static void drawBase(bool isLocked) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setColor(WHITE);

  // Row 0: role label
  display.drawString(0, 0, "base");

  // Row 1: lock state + inches (1:1)
  float localIn = inchesDelta(smoothedPa());
  if (localIn > 999.0f) localIn = 999.0f;
  if (localIn < -999.0f) localIn = -999.0f;
  char row1[20];
  if (isLocked) {
    snprintf(row1, sizeof(row1), "LK %+6.1f\"", localIn);
  } else {
    snprintf(row1, sizeof(row1), "   %+6.1f\"", localIn);
  }
  display.drawString(0, 12, row1);

  // Row 2: TX success count
  char row2[16];
  if (txOk < 10000) {
    snprintf(row2, sizeof(row2), "TX %lu", txOk);
  } else {
    snprintf(row2, sizeof(row2), "TX %luk", txOk / 1000);
  }
  display.drawString(0, 24, row2);

  // Row 3: RX success count + age
  char row3[24];
  unsigned long now = millis();
  unsigned long ageMs = (now > lastRxMs) ? (now - lastRxMs) : 0;
  if (rxOk < 10000) {
    if (ageMs < 10000) {
      snprintf(row3, sizeof(row3), "RX %lu %lus", rxOk, ageMs / 1000);
    } else {
      snprintf(row3, sizeof(row3), "RX %lu --", rxOk);
    }
  } else {
    snprintf(row3, sizeof(row3), "RX %luk %lus", rxOk / 1000, ageMs / 1000);
  }
  display.drawString(0, 36, row3);

  // Row 4: TX errors + last radio status
  char row4[20];
  snprintf(row4, sizeof(row4), "E%lu st:%d", txFail, lastRadioStatus);
  display.drawString(0, 48, row4);

  display.display();
}

static void drawMonitor(float inches, bool hasBase) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setColor(WHITE);

  // Row 0: role label
  display.drawString(0, 0, "monitor");

  if (!hasBase) {
    // Row 1: lost
    display.drawString(0, 12, "lost");
    // Row 2: no value
    display.drawString(0, 24, "----");
  } else {
    // Row 1: inches with arrow
    char row1[20];
    if (inches > 0.05f) {
      snprintf(row1, sizeof(row1), "  ^ %+6.1f\"", inches);
    } else if (inches < -0.05f) {
      snprintf(row1, sizeof(row1), "  v %+6.1f\"", inches);
    } else {
      snprintf(row1, sizeof(row1), "  - %+6.1f\"", inches);
    }
    display.drawString(0, 12, row1);

    // Row 2: RX count
    char row2[16];
    if (rxOk < 10000) {
      snprintf(row2, sizeof(row2), "RX %lu", rxOk);
    } else {
      snprintf(row2, sizeof(row2), "RX %luk", rxOk / 1000);
    }
    display.drawString(0, 24, row2);
  }

  display.display();
}

/* ============ radio: build and send packet ============ */
static void sendPacket() {
  DbPacket pkt;
  pkt.magic = DB_MAGIC;
  pkt.ver = DB_VER;
  pkt.flags = ROLE_STATIONARY;
  if (locked) pkt.flags |= STATE_LOCKED;
  pkt.uid = getUid();
  pkt.seq = seq++;
  pkt.press_mPa = (uint32_t)(smoothedPa() * 1000.0f);
  pkt.pa_std_cPa = stabStdCpa();
  float localIn = inchesDelta(smoothedPa());
  pkt.height_tenth_in = (int16_t)(localIn * 10.0f);

  uint8_t* raw = (uint8_t*)&pkt;
  pkt.soft_crc = crc8(raw, sizeof(DbPacket) - 1);

  radio.standby();
  radioRxFlag = false;
  int state = radio.transmit((uint8_t*)&pkt, sizeof(pkt));
  lastRadioStatus = state;
  if (state == RADIOLIB_ERR_NONE) {
    txOk++;
  } else {
    txFail++;
    lastRadioStatus = state;
  }
  resumeRadioRx();  // transmit leaves the SX1262 out of receive mode
}

/* ============ radio: receive and parse ============ */
static bool recvPacket(DbPacket* pkt, int timeoutMs) {
  (void)timeoutMs;  // polling only: never chop packets with repeated timed receive()
  if (!radioRxFlag) return false;
  radioRxFlag = false;
  size_t n = radio.getPacketLength();
  uint8_t raw[255];
  int state = radio.readData(raw, n > sizeof(raw) ? sizeof(raw) : n);
  lastPacketRssi = radio.getRSSI();
  lastPacketSnr = radio.getSNR();
  resumeRadioRx();  // rearm on success AND CRC/parser failure
  if (state != RADIOLIB_ERR_NONE) {
    lastRadioStatus = state; rxErrors++;
    Serial.printf("RX error=%d len=%u\n", state, (unsigned)n);
    return false;
  }
  if (n != sizeof(DbPacket)) { rxRejects++; return false; }
  memcpy(pkt, raw, sizeof(DbPacket));
  if (pkt->magic != DB_MAGIC || pkt->ver != DB_VER ||
      crc8(raw, sizeof(DbPacket)-1) != pkt->soft_crc) {
    rxRejects++; return false;
  }
  Serial.printf("RX uid=%08lx seq=%u len=%u Pa=%.3f RSSI=%.1f SNR=%.1f\n",
                pkt->uid, pkt->seq, (unsigned)n, pkt->press_mPa/1000.0f,
                lastPacketRssi, lastPacketSnr);
  return true;
}

/* ============ monitor: update base table ============ */
static void updateBaseTable(DbPacket* pkt) {
  unsigned long now = millis();
  bool found = false;

  for (int i = 0; i < baseCount; i++) {
    if (bases[i].uid == pkt->uid) {
      bases[i].pa = pkt->press_mPa / 1000.0f;
      bases[i].rssi = (int8_t)lastPacketRssi;
      bases[i].stability_cPa = pkt->pa_std_cPa;
      bases[i].locked = (pkt->flags & STATE_LOCKED) != 0;
      bases[i].lastSeenMs = now;
      found = true;
      break;
    }
  }

  if (!found && baseCount < MAX_BASES) {
    bases[baseCount].uid = pkt->uid;
    bases[baseCount].pa = pkt->press_mPa / 1000.0f;
    bases[baseCount].rssi = (int8_t)lastPacketRssi;
    bases[baseCount].stability_cPa = pkt->pa_std_cPa;
    bases[baseCount].locked = (pkt->flags & STATE_LOCKED) != 0;
    bases[baseCount].lastSeenMs = now;
    baseCount++;
  }
}

/* ============ monitor: get best base ============ */
static BaseEntry* getBestBase() {
  unsigned long now = millis();
  BaseEntry* best = nullptr;
  bool haveLocked = false;

  for (int i = 0; i < baseCount; i++) {
    if (now - bases[i].lastSeenMs > DB_BASE_TIMEOUT_MS) continue;
    if (bases[i].locked) haveLocked = true;
    if (haveLocked && !bases[i].locked) continue;
    if (!best || bases[i].stability_cPa < best->stability_cPa) {
      best = &bases[i];
    }
  }
  return best;
}

/* ============ BLE stub ============ */
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;

class DBServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    Serial.println("BLE client connected");
  }
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    Serial.println("BLE client disconnected");
  }
};

class DBNotifyCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    std::string cmd = pChar->getValue();
    Serial.printf("BLE cmd: %s\n", cmd.c_str());

    if (cmd == "ZERO" || cmd == "CALIBRATE") {
      // calibrate: snapshot current differential as 0
      if (role == 2) {
        BaseEntry* best = getBestBase();
        if (best) {
          calibOffsetIn = hypsometricInches(livePa, best->pa);
        }
      }
    } else if (cmd.length() > 7 && cmd.substr(0, 7) == "OFFSET:") {
      calibOffsetIn = atof(cmd.substr(7).c_str());
    } else if (cmd == "STATUS") {
      char buf[128];
      snprintf(buf, sizeof(buf), "role=%d locked=%d bases=%d calib=%.1f",
               role, locked, baseCount, calibOffsetIn);
      if (pTxChar) {
        pTxChar->setValue(buf);
        pTxChar->notify();
      }
    }
  }
};

static void initBle() {
  NimBLEDevice::init("DirtBeetle");
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new DBServerCallbacks());

  NimBLEService* pService = pServer->createService(DB_SERVICE_UUID);

  NimBLECharacteristic* pRxChar = pService->createCharacteristic(
    DB_RX_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pRxChar->setCallbacks(new DBNotifyCallbacks());

  pTxChar = pService->createCharacteristic(
    DB_TX_CHAR_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  pService->start();
  NimBLEAdvertising* pAdvertising = pServer->getAdvertising();
  pAdvertising->start();
  Serial.println("BLE advertising");
}

static void streamBle() {
  if (!pTxChar) return;
  unsigned long now = millis();
  if (now - lastBleMs < 500) return;
  lastBleMs = now;

  BaseEntry* best = getBestBase();
  char buf[192];
  if (best) {
    float diffIn = hypsometricInches(smoothedPa(), best->pa) - calibOffsetIn;
    snprintf(buf, sizeof(buf), "T:%.0f L:%.1f B:%.1f D:%.2f S:%d LK:%d RSSI:%d",
             now / 1000.0f, livePa, best->pa, diffIn, best->stability_cPa,
             best->locked, best->rssi);
  } else {
    snprintf(buf, sizeof(buf), "T:%.0f L:%.1f NOBASE", now / 1000.0f, livePa);
  }
  pTxChar->setValue(buf);
  pTxChar->notify();
}

/* ============ setup ============ */
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(100);

  display.init();
  display.display();

  bmpWire.begin(BMP_SDA, BMP_SCL);
  while (!bmp.begin()) {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 24, "BMP585 init fail");
    display.display();
    Serial.println("Sensor init fail, retrying...");
    delay(1000);
  }
  Serial.println("BMP585 init OK");

  bmp.configInterrupt(bmp.eIntModePulsed, bmp.eIntHighActive, bmp.eIntPushPull);
  bmp.setIntSource(bmp.eIntDataReady);
  pinMode(BMP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BMP_INT), bmpIsr, RISING);
  bmp.setODR(bmp.eOdr25Hz);
  bmp.setMeasureMode(bmp.eNormal);
  // Enable IIR filter (7 = good noise reduction vs response time)
  bmp.configIIR(bmp.eFilter7, bmp.eFilter7);

  // radio init (Heltec V3 SX1262 SPI pins)
  SPI.begin(9, 11, 10, 8);
  // Explicit Heltec V3 TCXO (DIO3 1.8V) and DIO2 RF switch.
  bool radioOk = radioCheck("begin", radio.begin(DB_FREQUENCY, DB_BW, DB_SF,
      DB_CR, DB_SYNC, DB_TX_POWER, DB_PREAMBLE, 1.8f, false));
  radioOk &= radioCheck("DIO2", radio.setDio2AsRfSwitch(true));
  radioOk &= radioCheck("CRC", radio.setCRC(0));
  radioOk &= radioCheck("sync", radio.setSyncWord(0x12));
  if (!radioOk) {
    Serial.println("SX1262 FAILED: refusing silent radio failure");
    while (true) delay(1000);
  }
  radio.setPacketReceivedAction(radioRxIsr);
  Serial.printf("SX1262 init OK MAC=%012llx uid=%08lx power=%d dBm\n",
                ESP.getEfuseMac(), getUid(), DB_TX_POWER);

  // settle 3s with Grok face animation
  Serial.println("settling 3000 ms");
  unsigned long t0 = millis();
  bool lookLeft = true;
  unsigned long lastLookMs = 0;
  while (millis() - t0 < 3000) {
    drainBmp();
    unsigned long now = millis();
    if (now - lastLookMs > 300) {
      lookLeft = !lookLeft;
      drawGrokFace(lookLeft);
      lastLookMs = now;
    }
  }

  // debug: print pressure and altitude values
  Serial.printf("livePa=%.2f Pa\n", livePa);
  Serial.printf("altMeters(livePa)=%.6f m\n", altMeters(livePa));
  Serial.printf("altMeters(101325)=%.6f m\n", altMeters(101325.0f));
  Serial.printf("diff=%.6f m = %.4f in\n", altMeters(livePa) - altMeters(101325.0f), (altMeters(livePa) - altMeters(101325.0f)) * 39.3701f);

  // lock local refPa
  refPa = livePa;
  Serial.printf("zero locked at %.1f Pa\n", refPa);

  // Role assignment based on MAC address
  // DB2 (b0:f0) = base, DB1 (ae:f0) = monitor
  uint64_t mac = ESP.getEfuseMac();
  uint8_t mac4 = (uint8_t)((mac >> 32) & 0xFF);  // 4th byte (ae or b0)
  Serial.printf("MAC=%012llx mac4=0x%02x\n", mac, mac4);
  if (mac4 == 0xb0) {
    role = 1;
    Serial.println("MAC b0:f0: Stationary Base (DB2)");
  } else {
    role = 2;
    Serial.println("MAC ae:f0: Monitoring DB (DB1)");
  }

  // init BLE
  initBle();

  Serial.println("ready");
}

/* ============ loop ============ */
void loop() {
  drainBmp();

  if (role == 1) {
    // Stationary Base
    float localIn = inchesDelta(livePa);
    if (!locked && localIn > DB_LOCK_INCHES) {
      locked = true;
      Serial.println("LOCKED: >+24 inches");
    }

    drawBase(locked);

    // TX at ~5 Hz with jitter
    unsigned long now = millis();
    uint32_t uid = getUid();
    int jitter = (int)(uid % DB_JITTER_MS);
    if (now - lastTxMs >= DB_TX_INTERVAL_MS + jitter) {
      lastTxMs = now;
      sendPacket();
    }

    // brief non-blocking RX window to track peer frames (diagnostics only)
    DbPacket pkt;
    if (recvPacket(&pkt, 200)) {
      if ((pkt.flags & ROLE_STATIONARY) && pkt.uid != uid) {
        rxOk++;
        lastRxMs = now;
      }
      // Process monitor packets and display differential
      if ((pkt.flags & ROLE_MONITOR) && pkt.uid != uid) {
        float monitorPressure = pkt.press_mPa / 1000.0f;
        float localPressure = smoothedPa();
        float deltaPa = monitorPressure - localPressure;
        float inches = deltaPa * 0.003595f;  // Pa to inches of water
        Serial.printf("base: monitor diff %.2f inches\n", inches);
      }
    }

    streamBle();

    // serial diagnostics every ~1s
    if (now - lastDiagMs >= 1000) {
      lastDiagMs = now;
      Serial.printf("diag TX=%lu RX=%lu E=%lu st=%d in=%.1f Pa=%.0f locked=%d\n",
                    txOk, rxOk, txFail, lastRadioStatus, localIn, livePa, locked);
    }

  } else if (role == 2) {
    // Monitoring DB
    DbPacket pkt;
    if (recvPacket(&pkt, 200)) {
      if ((pkt.flags & ROLE_STATIONARY) && pkt.uid != getUid()) {
        lastBaseHearMs = millis();
        rxOk++;
        lastRxMs = millis();
        updateBaseTable(&pkt);
      }
    }

    BaseEntry* best = getBestBase();
    if (best) {
      float diffIn = hypsometricInches(smoothedPa(), best->pa) - calibOffsetIn;
      drawMonitor(diffIn, true);
    } else {
      drawMonitor(0, false);
    }

    // TX at ~5 Hz with jitter (same as base)
    unsigned long now = millis();
    uint32_t uid = getUid();
    int jitter = (int)(uid % DB_JITTER_MS);
    if (now - lastTxMs >= DB_TX_INTERVAL_MS + jitter) {
      lastTxMs = now;
      // Send monitor packet with ROLE_MONITOR flag
      DbPacket mpkt;
      mpkt.magic = DB_MAGIC;
      mpkt.ver = DB_VER;
      mpkt.flags = ROLE_MONITOR;
      mpkt.uid = uid;
      mpkt.seq = seq++;
      mpkt.press_mPa = (uint32_t)(smoothedPa() * 1000.0f);
      mpkt.pa_std_cPa = stabStdCpa();
      float localIn = inchesDelta(smoothedPa());
      mpkt.height_tenth_in = (int16_t)(localIn * 10.0f);
      uint8_t* raw = (uint8_t*)&mpkt;
      mpkt.soft_crc = crc8(raw, sizeof(DbPacket) - 1);
      radio.standby();
      radioRxFlag = false;
      int state = radio.transmit((uint8_t*)&mpkt, sizeof(mpkt));
      lastRadioStatus = state;
      if (state == RADIOLIB_ERR_NONE) {
        txOk++;
      } else {
        txFail++;
      }
      resumeRadioRx();
    }

    streamBle();
    now = millis();
    if (now - lastDiagMs >= 1000) {
      lastDiagMs = now;
      Serial.printf("monitor RX=%lu bases=%d localPa=%.3f basePa=%.3f diffIn=%.2f rxErr=%lu reject=%lu isr=%lu rssi=%.1f len=%u\n",
        rxOk, baseCount, livePa, best ? best->pa : 0.0f,
        best ? hypsometricInches(livePa, best->pa)-calibOffsetIn : 0.0f,
        rxErrors, rxRejects, radioRxIsrCount, radio.getRSSI(), radio.getPacketLength());
    }
  }
  delay(1);
}
