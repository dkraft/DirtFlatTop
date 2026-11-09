# DirtBeetle (DB) — Implementation Spec for Hermes

**Status:** SPEC ONLY — do not implement until David reviews the **Open Edge Cases / Logic Questions** section and approves/tweaks the rules.  
**Date:** 2026-11-06 (America/New_York)  
**Companion:** `~/DirtFlatTop/HERMES_CONTEXT.md` (BMP585 grade-radio proven path), `bmp585_monitor/bmp585_monitor.ino`  
**Goal of this phase:** Robust low-latency **LoRa PHY + role state machine** between identical DB clones. BMP585 is proven at **I2C 0x47**; signal refinement comes later.

---

## 1. Device identity

| Item | Value |
|------|--------|
| Name | **DirtBeetle** / **DB** |
| Hardware | Heltec WiFi LoRa 32 **V3** (ESP32-S3 + SX1262 + SSD1306 128×64) |
| Sensor | DFRobot Gravity BMP585 (SEN0664), I2C **0x47**, DIP = I2C |
| Firmware | **Identical clone firmware** on every DB — role is elected at runtime, not flashed differently |
| Port | Flash / serial **only** `/dev/ttyUSB0` (CP210x). **Never** touch `/dev/ttyACM0` (Heltec V4 meshbot) |
| FQBN | `esp32:esp32:heltec_wifi_lora_32_V3` |

### Proven pin map (reuse from `bmp585_monitor`)

| Function | Pin / bus |
|----------|-----------|
| BMP SDA / SCL | GPIO **19** / **20** on `TwoWire bmpWire(1)` |
| BMP INT | GPIO **47** (RISING, pulsed, high-active) |
| BMP I2C addr | **0x47** |
| OLED | SSD1306 `0x3c`, `SDA_OLED`/`SCL_OLED`/`RST_OLED` (Heltec macros ≈ 17/18/21) |
| OLED power | `Vext` (GPIO36) **LOW** = on |
| PRG / boot | GPIO **0** (future: re-zero / force role — TBD) |

### Heltec V3 onboard SX1262 (SPI)

| Signal | GPIO |
|--------|------|
| NSS (CS) | 8 |
| SCK | 9 |
| MOSI | 10 |
| MISO | 11 |
| NRST | 12 |
| BUSY | 13 |
| DIO1 | 14 |

---

## 2. Libraries (machine inventory + choice)

**Already installed (relevant):**

| Library | Version | Role |
|---------|---------|------|
| Heltec ESP32 Dev-Boards | 2.1.6 | OLED (`HT_SSD1306Wire`), board pins, bundled SX1262 radio stack |
| DFRobot_BMP58X | 1.0.1 | BMP585 driver (proven) |
| NimBLE-Arduino | 2.1.2 | Future APK command path (BLE GATT stub) |
| Adafruit SSD1306 | 2.5.13 | Present but **do not use** — stick to Heltec `HT_SSD1306Wire` for consistency |

**Not installed — required for this phase:**

| Library | Action |
|---------|--------|
| **RadioLib** | Install: `arduino-cli lib install "RadioLib"` |

**Decision — RadioLib SX1262 raw LoRa PHY (not Heltec LoRaWAN, not Meshtastic):**

- Sub-30 ms airtime at SF7 / BW250–500 for compact frames → supports **5–10 Hz** grade updates.
- Zero join / no mesh routing / no LoRaWAN duty-cycle stack overhead.
- Deterministic point-to-multipoint broadcast: Stationary DB TX, Monitoring DBs RX.
- Heltec’s bundled radio exists in `Heltec_ESP32_Dev-Boards`, but RadioLib is the portable, widely documented gold standard for explicit PHY params (SF/BW/CR/sync/preamble/CRC/TX power) on Heltec V3 ESP32-S3.

Example RadioLib module construction (Hermes must verify against installed RadioLib API):

```cpp
// NSS=8, DIO1=14, NRST=12, BUSY=13; SPI pins SCK=9,MISO=11,MOSI=10
SX1262 radio = new Module(8, 14, 12, 13);
```

---

## 3. Boot / Settling Phase (0–3 s)

1. `Serial.begin(115200)`, `vextOn()`, OLED init (reuse existing; keep or drop 180° rotation — **open Q**).
2. Init BMP585 on Wire1 (`19/20`, addr `0x47`); retry until `begin()` OK (proven).
3. For **SETTLE_MS = 3000**:
   - Drain BMP INT into settle window (reuse `win` / ODR 25 Hz pattern from `bmp585_monitor`).
   - OLED: **Grok Bot circular animation** — round face/eyes; **pupils/gaze look left then right** (toggle every ~250–400 ms).
   - Text: **`"scanning"`** (centered under/near the face).
4. After 3 s: lock local pressure baseline (`refPa` / `lockZero(mean1s)`) for local height math; enter **Listen Window** (role election).

Animation note for Hermes: keep drawing cheap (fillCircle / fillRect on 128×64). No bitmaps required for v1. Frame rate ~10–20 FPS is enough during settle.

---

## 4. LoRa PHY & packet protocol (~500 m, low latency)

### 4.1 PHY parameters (US915 jobsite)

| Param | Value | Rationale |
|-------|--------|-----------|
| Region | **US915 ISM**, center **915.0 MHz** (single fixed channel for v1) | Simple, legal unlicensed ISM |
| Spreading factor | **SF7** | Lowest latency LoRa SF still robust at ~500 m LOS |
| Bandwidth | **250 kHz** (prefer); **500 kHz** acceptable if airtime budget needs headroom | Shorter symbol time → lower latency |
| Coding rate | **CR 4/5** | Light FEC, minimal overhead |
| Sync word | **0x12** (private) preferred over public `0x34` | Avoid LoRaWAN/public traffic collisions |
| Preamble | **8** symbols | Standard / fast acquire |
| CRC | **Enabled** (RadioLib / SX1262 hardware CRC) | Drop corrupt frames |
| TX power | **+20 dBm** default (allow +22 if Heltec PA/lib supports safely) | Link budget for ~500 m dirt work |
| Implicit header | Explicit header ON | Safer for multi-version payloads |

**Why this beats LoRaWAN / Meshtastic here:** Grade radio needs **deterministic 5–10 Hz** relative elevation updates at jobsite range. LoRaWAN join + MAC + duty-cycle and Meshtastic mesh flooding add seconds-scale or multi-hop latency. Raw SX1262 + RadioLib with a **12–16 byte** binary frame targets **~15–35 ms airtime** (SF7 / BW250 / CR4/5 ballpark) → room for 5–10 Hz TX with listen gaps.

### 4.2 Update rate & airtime budget

- Stationary TX cadence: **5 Hz** default (200 ms); configurable **5–10 Hz**.
- After each TX, Stationary returns to RX briefly (future: APK / peer beacons) unless locked and TX-only mode is chosen (**open Q**).
- Monitoring DB: continuous RX; optional sparse ACK later (**out of scope v1**).

### 4.3 Binary frame (`DbPacket`, little-endian, packed)

Target **16 bytes** (fits one LoRa payload cleanly):

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 2 | `magic` | `0xDB01` |
| 2 | 1 | `ver` | Protocol version `1` |
| 3 | 1 | `flags` | Role + state bits (see below) |
| 4 | 4 | `uid` | Lower 32 bits of ESP32 eFuse MAC (stable clone ID) |
| 8 | 2 | `seq` | TX sequence, wraps |
| 10 | 2 | `height_tenth_in` | **int16**: relative height in **tenths of an inch** vs local zero (`+240` = +24.0"). Signed |
| 12 | 2 | `press_hPa_x10` | Optional telemetry: pressure ×10 (uint16); or reserved `0` |
| 14 | 2 | `crc16` | Soft CRC16-CCITT over bytes 0–13 **in addition to** radio HW CRC (defense in depth) — **or** omit soft CRC and shrink to 14 bytes (**open Q**) |

**`flags` bitfield (v1):**

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | `ROLE_STATIONARY` | Sender claims Stationary role |
| 1 | `ROLE_MONITOR` | Sender claims Monitoring (bucket) role (normally RX-only; reserved) |
| 2 | `STATE_LOCKED` | Stationary entered **"+24 inch Lock"** |
| 3 | `STATE_SETTLING` | Optional; usually not TX during settle |
| 4–7 | reserved | 0 |

Magic + ver + soft CRC let Monitoring ignore foreign traffic on the same SF/BW/sync.

---

## 5. Role election & runtime behavior (AUTHORITATIVE — David 2026-11-06)

### 5.1 Listen window after `"scanning"`

1. After settle (3 s), enter **LISTEN** for ~**2.0 s + small UID-seeded jitter** (~0–400 ms).
2. Optional force-base: if **PRG (GPIO0) is held LOW at the end of settle / during listen**, **force Stationary Base** role so a 2nd/3rd Base DB can be started next to an already-active Base.
3. During LISTEN, RX for valid `DbPacket` frames with Base / `ROLE_STATIONARY`.

### 5.2 Stationary Base DB (silence → Base; multi-base coexistence)

- If **no** Base beacon heard (and PRG force not used) → become **Stationary Base**.
- **Multiple Base DBs are explicitly supported.** A Stationary Base that later hears another Base **does NOT auto-demote**. Never demote on hearing a peer Base.
- TX cadence ~**5 Hz** with UID-seeded interval jitter (**185–225 ms**) + `scanChannel()` CAD/LBT before each TX.
- Beacon carries: `uid`, live filtered pressure, **stability metric**, `seq`, flags including `STATE_LOCKED` when armed.
- Continuously compute local height from boot/`PRG` zero at **1:1 true inches** (NO 6:1 scale).

### 5.3 `>+24"` → `"Locked"` (steady armed Base)

- When local height changes **positively by more than `24.0"`** from the local zero:
  - OLED → **`"Locked"`** (large, clear).
  - Latch `STATE_LOCKED` in TX flags.
  - **Keep transmitting** live `Pa` + stability + `STATE_LOCKED`.
- Lock **latches** as identified steady Base. Short `PRG` may re-zero local height if needed.

### 5.4 Monitoring DB (bucket) — always differential, multi-base aware

If ≥1 valid Base beacon heard during LISTEN (and not PRG-forced Base) → **Monitoring DB**.

- Scale: **1:1 true inches** only.
- Maintain an **active Bases table** (up to **8**). Expire a Base after ~**2.5 s** without a valid packet.
- Prefer Bases with `STATE_LOCKED` when any Locked Bases are present; still accept unlocked Bases if none are Locked yet.
- **Differential elevation always:** `dInches = hypsometric_inches(local_Pa, base_Pa)` (weather cancels). Apply local calibration offset from `PRG` / APK `ZERO`.
- **Narrowband / stability reject (device-side for OLED):** when aggregating multiple Bases, reject outliers using reported stability and/or disagreement vs median; prefer low-variance Locked Bases. APK will also compute its own multi-base average with narrowband reject on stability from streamed telemetry.
- OLED: **`"monitoring"`**, arrow **`↑`** / **`↓`**, inches **0–999**. If all Bases go stale → link-wait/stale indicator; do **not** flip into competing Stationary TX mid-job.

### 5.5 Relative math (mandatory)

```
rel_m = altMeters(local_Pa) - altMeters(base_Pa)
inches = rel_m * 39.3701f          // 1:1 TRUE inches
display = inches - calibOffsetIn   // ZERO / OFFSET
```

Do **not** subtract two independent boot zeros alone. Do **not** use 6:1.

---

## 6. Monitoring DB — Android APK BLE command stub (NimBLE)

Implement now with **NimBLE-Arduino** (already installed). No APK in this phase.

- Non-blocking BLE UART/GATT (Nordic UART-style UUIDs OK).
- Stream telemetry so APK can multi-base average + narrowband reject:
  - `local_pa`, `calib_in`, and per-Base `{uid, pa, stability, locked, rssi, age_ms}`.
- Commands (newline / `;` delimited), queued in `loop()`:
  | Command | Effect |
  |---------|--------|
  | `ZERO` / `CALIBRATE` | Snapshot current differential inches → 0" |
  | `OFFSET:<inches>` | Set calibration offset |
  | `STATUS` | Reply role, calib, Bases table |

BLE must never starve SX1262 / BMP ISR.

UUIDs:
- Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (APK→DB): `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX (DB→APK): `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

---

## 7. OLED UI states

| State | OLED |
|-------|------|
| Settle 0–3 s | Circular Grok Bot face, eyes L↔R, `"scanning"` |
| Listen | Keep `"scanning"` or brief `"listen"` |
| Stationary unlocked | `"BASE"` (+ optional debug on serial) |
| Stationary Locked | **`Locked`** |
| Monitoring | **`monitoring`** + `↑`/`↓` + ≤3 digit inches + `"` |
| Link stale (Monitor) | Flash `"lost"` / `"----"` — do not TX as Base |

Animation: cheap `fillCircle`/`fillRect`, ~10–20 FPS during settle.

---

## 8. Binary frame (`DbPacket`, little-endian, packed)

```c
struct __attribute__((packed)) DbPacket {
  uint16_t magic;       // 0xDB01
  uint8_t  ver;         // 1
  uint8_t  flags;       // ROLE_STATIONARY=1, STATE_LOCKED=2, ...
  uint32_t uid;         // ESP32 MAC low 32 bits
  uint16_t seq;
  uint32_t press_mPa;   // live filtered pressure, millipascals
  uint16_t pa_std_cPa;  // stability std-dev (centi-Pascals)
  int16_t  height_tenth_in; // Stationary local UI height ×10
  uint8_t  soft_crc;    // XOR/CRC8 over prior bytes
};
```

---

## 9. Safety / ops

- Antenna required before +20 dBm TX.
- Flash **ONLY** `/dev/ttyUSB0`. **Never** `/dev/ttyACM0`.
- Backup: `bmp585_monitor/bmp585_monitor.ino.bak_pre_db`.
- FQBN: `esp32:esp32:heltec_wifi_lora_32_V3`
- Libs: RadioLib 7.8.1, DFRobot_BMP58X, Heltec ESP32 Dev-Boards, NimBLE-Arduino.

---

## 10. Hermes implementation checklist

- [ ] Confirm backup `.ino.bak_pre_db`
- [ ] Rewrite `bmp585_monitor.ino` as identical clone DirtBeetle firmware
- [ ] Boot: Grok circular L/R eyes + `"scanning"` 3 s; BMP585 @ 0x47 OK
- [ ] RadioLib SX1262 US915 SF7/BW250/CR4/5 sync 0x12 +20 dBm
- [ ] Listen + optional PRG-force Base; silence→Base; hear Base→Monitor; **no demote**
- [ ] Base TX ~5 Hz + jitter + CAD; live Pa + stability + STATE_LOCKED
- [ ] `>+24"` → OLED `"Locked"`, latch flag, keep TX
- [ ] Monitor: 1:1 differential multi-base table, ↑/↓ + 3-digit inches, stale handling
- [ ] NimBLE stub: telemetry + ZERO/OFFSET/STATUS
- [ ] `arduino-cli compile` clean; `upload -p /dev/ttyUSB0` only; serial verify 8–15 s
- [ ] Update `HERMES_CONTEXT.md` with milestone, packet format, BLE UUIDs

## 11. Resolved decisions (do not re-open)

| Topic | Decision |
|-------|----------|
| Scale | **1:1 true inches** (drop 6:1) |
| Relative math | Always **differential live Pa** Base→Monitor |
| Multi-base | Supported; Bases **never auto-demote**; APK averages with narrowband reject on stability |
| Lock | `>+24"` identifies steady Base; OLED `"Locked"`; latch; keep TX live Pa + `STATE_LOCKED` |
| Force 2nd Base | Hold **PRG** at end of boot/listen → force Stationary |
| Flash port | `/dev/ttyUSB0` only |

## 12. Out of scope (this phase)

- Android APK UI (BLE stub only on DB)
- Meshtastic / LoRaWAN
- Flashing or touching `/dev/ttyACM0`
