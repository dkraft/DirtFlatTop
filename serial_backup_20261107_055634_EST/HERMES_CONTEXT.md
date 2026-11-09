# Grade Radio System — Handoff Context

## What this is
A DIY barometric grade-indication system. The **Grade Radio System** reads a BMP585 barometric pressure sensor on a Heltec WiFi LoRa 32 **V3** (ESP32-S3), computes a signed-inch "grade" relative to a zero reference, and displays it on the V3's built-in OLED (128×64 SSD1306). The goal is a physical "grade radio" that shows cut/fill in inches with a directional arrow, so an operator can read grade at a distance.

## Hardware
- **MCU:** Heltec WiFi LoRa 32 V3 (ESP32-S3), CP210x USB on `/dev/ttyUSB0`.
- **FQBN:** `esp32:esp32:heltec_wifi_lora_32_V3`
- **OLED:** built-in SSD1306, 128×64, I2C addr 0x3c, SDA=GPIO17, SCL=GPIO18, RST=GPIO21. `Vext` pin (GPIO36) must be set LOW to enable the OLED.
- **BMP585 (DFRobot Gravity SEN0664):** I2C on a **second** TwoWire bus (bus 1), SDA=GPIO19, SCL=GPIO20, INT=GPIO47, addr 0x47. DIP switch must be set to I2C mode; power-cycle the sensor after DIP changes. Library: `DFRobot_BMP58X` (in `~/Arduino/libraries/`).
- **PRG button:** GPIO0 (boot button). Held LOW = re-zero to 0".
- **Do NOT touch the V4 meshbot** on `/dev/ttyACM0`. Only flash the V3 on `/dev/ttyUSB0`.

## Build & Flash
```bash
cd /home/a/DirtFlatTop
~/.local/bin/arduino-cli compile --fqbn esp32:esp32:heltec_wifi_lora_32_V3 -e bmp585_monitor
~/.local/bin/arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:heltec_wifi_lora_32_V3 bmp585_monitor
```
Verify boot via passive pyserial read (no DTR/RTS toggle needed after flash):
```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
time.sleep(0.5)
end = time.time() + 15
while time.time() < end:
    if s.in_waiting:
        for line in s.read_all().decode(errors='replace').splitlines():
            if line.strip(): print(line)
s.close()
"
```
Expected boot sequence:
```
BMP585 init OK
settling 3000 ms
zero locked at <Pa> Pa
ready, PRG holds to re-zero
```

## Architecture (single file: `bmp585_monitor/bmp585_monitor.ino`)

### Sensor pipeline
1. **Interrupt-driven reads:** BMP585 INT pin (GPIO47, RISING) → ISR sets `bmp_irq`. Main loop drains the flag, calls `bmp.readPressPa()` / `bmp.readTempC()`.
2. **Ring buffer `win`:** 100-sample window (25 Hz → 4 s). `mean1s()` returns the last 10 samples (1 s).
3. **0.5 s integration window `integBuf`:** 13-sample ring of `inchesDelta(pPa)`. `integMean()` returns the mean. This is the input to the smoother — it removes per-sample jitter before the ratchet.

### Grade model
- `refPa` — the zero reference (Pa), locked by `lockZero()`.
- `altMeters(pPa)` — standard-atmosphere hypsometric altitude in meters:
  ```
  h = (T0 / (g*L)) * (1 - (p/p0)^(Rd*L/g))
  T0=288.15 K, g=9.80665, L=0.0065, Rd=287.058, p0=101325
  ```
- `inchesDelta(pPa)` — signed inches from reference, **6:1 scale**:
  ```
  return dm * 39.3701f * 6.0f;
  ```
  The 6:1 scale means 1 inch of altitude change = 6 inches of displayed grade. **This was tuned by the user: 24" of real movement should read 24" on the display.**
- Positive = higher than reference (cut). Negative = lower (fill).

### Smoothing
- **Asymmetric one-pole ratchet** (`smoothInches`):
  - `tauRiseMs = 50` (fast attack — a real step shows now)
  - `tauFallMs = 200` (slow release — wiggles don't jump the number)
  - `alpha = 1 - exp(-dt/tau)`, sampled at 25 Hz.
- **0.5 s integration** feeds the ratchet, not raw readings.

### Config (API-style, for on-the-fly tuning by the main unit)
```cpp
struct GradeConfig {
  float deadzoneIn;    // inches; show 0 below this magnitude
  float arrowEps;      // inches; arrow appears above this (hysteresis)
  float tauRiseMs;     // smoother time-constant, rising edge
  float tauFallMs;     // smoother time-constant, falling edge
} gcfg = { 0.5f, 0.3f, 50.0f, 200.0f };
```
All knobs are runtime-updatable. The main unit will write to `gcfg` via serial/LoRa (TBD).

### Display layout (128×64, rotated 180°)
The display is rotated 180° via `display.screenRotate(ANGLE_180_DEGREE)`. After rotation, the left side of the buffer appears on the right visually, and vice versa.

Current layout (in buffer coordinates, pre-rotation):
- **Digits:** 3 × 30px wide + 2 × 3px gaps = 96px, starting at x=2, y=10, 42px tall.
  - Each digit: 5 cols × 6px = 30px wide, 7 rows × 6px = 42px tall.
  - 5×7 bitmap font, 10 digits (0-9), drawn with `fillRect`.
- **Quote mark:** after the digits, ArialMT_Plain_10.
- **Arrow / flat bar:** 24px wide, at x=101. Full-height triangle (52px tall) via `drawArrow()`. Flat bar (24×4) when grade is in the deadzone.
- **Temp:** ArialMT_Plain_10, at (2, 54).
- **Total width:** 2 + 96 + 4 + 24 + 3 = 129px (fits in 128 with 1px margin).

### Boot sequence
1. Init serial (115200), Vext (OLED power), display, rotate 180°.
2. Init BMP585 on bus 1 (retry loop).
3. Configure interrupt (pulsed, high-active, push-pull), attach to GPIO47 RISING.
4. Set ODR 25 Hz, measure mode normal.
5. **Settle 3 s** — drain INT into `win`, display "settle...".
6. `lockZero(mean1s)` — lock the 1 s mean as the 0" reference.
7. If PRG is held → `prgCalibrate()` (re-zero before going live).
8. First `drawFrame()`, print "ready, PRG holds to re-zero".

### Loop
1. Drain `bmp_irq` → read P/T, push to `win` and `integBuf`.
2. If PRG held → `prgCalibrate()`.
3. `smoothInches(integMean())` → updates `dispInches`.
4. `drawFrame(livePa)` → renders the display.

## Current state & known issues
- **Working:** sensor init, 3 s settle, zero lock, PRG re-zero, 0.5 s integration + ratchet smoothing, deadzone, arrow direction, scale (6:1), 180° rotation.
- **User's last complaint:** digits are overlapping. The 5×7 bitmap at 6px scale = 30px wide per digit, 3 digits + 2×3px gaps = 96px. The arrow is at x=101. After 180° rotation the arrow appears on the left and digits on the right. The user wants ~3px spacing between digits and the digits to be wide enough to read clearly.
- **User's intent:** the digits should be **big, clear, 3-digit zero-padded** (001, 002, 003…), with an arrow indicating direction. The number is the primary readout; the arrow is secondary.

## Key gotchas
- **Arduino preprocessor lifts function prototypes above file-scope statics.** If a non-static function (e.g. `lockZero`) references a static (e.g. `dispInches`), the lifted prototype compiles first and fails with "not declared." Forward-declare the statics near the top of the file, right after `refPa`.
- **GPIO0 (PRG) can read LOW at boot** if the button is held or there's a pull-down. `prgCalibrate()` has an 8 s watchdog and requires a clean release before arming to prevent trapping the UI.
- **BMP585 DIP switch:** I2C mode + power-cycle required. `begin()` fails silently otherwise.
- **OLED rotation:** `screenRotate(ANGLE_180_DEGREE)` mirrors the buffer. Left↔right and top↔bottom are both flipped. Design the layout in buffer coordinates and remember the visual result is mirrored.
- **Fonts available:** `ArialMT_Plain_10`, `ArialMT_Plain_16`, `ArialMT_Plain_24`. No built-in scaling API. Custom digits use `fillRect` on a 5×7 bitmap.

## Files
- `/home/a/DirtFlatTop/bmp585_monitor/bmp585_monitor.ino` — the entire sketch.
- `~/Arduino/libraries/Heltec_ESP32_Dev-Boards/src/HT_Display.h` — display API.
- `~/Arduino/libraries/Heltec_ESP32_Dev-Boards/src/HT_SSD1306Wire.h` — SSD1306Wire class.
- `~/Arduino/libraries/DFRobot_BMP58X/` — BMP585 library.

## TODO / next steps
- **Fix digit overlap / spacing** — user wants ~3px between digits, digits wide enough to read, no overlap with arrow.
- **Verify 180° rotation** — user said "no rotation occurred" in one pass. Confirm the display is actually rotated.
- **Wire up the config API** — main unit will push `gcfg` updates over serial/LoRa.
- **Consider persistence** — `refPa` is volatile; a power-cycle re-zeroes. NVS would survive.


## DirtBeetle phase
- Spec (do not implement until reviewed): `~/DirtFlatTop/DIRTBEETLE_SPEC.md`
- BMP585 proven at I2C **0x47**. Next: LoRa role state machine between identical DB clones.


---

## Milestone — DirtBeetle v1 LoRa + roles + BLE stub (2026-11-06 America/New_York)

**Status:** Firmware authored via Hermes (`TurboFCFusion IQ4_XS` / `Current meshbot LLM` on `127.0.0.1:56113` via Unsloth Studio `10.242.74.158:8888/v1`), API fixes applied for RadioLib 7.8.1 / Heltec OLED / NimBLE 2.1.2, compiled clean, flashed **ONLY** `/dev/ttyUSB0`.

**Backup:** `bmp585_monitor/bmp585_monitor.ino.bak_pre_db`

**Build:** flash **676,436** bytes (20%); RAM **32,420** bytes (9%). FQBN `esp32:esp32:heltec_wifi_lora_32_V3`.

**Boot observed (single DB, no peer yet):** BMP settle → zero lock → listen ~2.0–2.4 s → silence → **Stationary Base** → BLE advertising → `ready`. SX1262 @ 915.0 MHz SF7/BW250/CR4/5 sync 0x12 +20 dBm.

### Behavior locked in firmware
- Identical clone firmware; BMP585 `0x47` Wire1 `19/20` INT `47`; OLED Vext; PRG=`GPIO0`.
- Boot 3 s Grok circular face eyes L↔R + `"scanning"`.
- Role election: listen + UID jitter; PRG held → force Base; silence → Base; hear Base → Monitor; **Bases never auto-demote** (multi-base OK).
- Base TX ~5 Hz + jitter + CAD; live `press_mPa` + `pa_std_cPa` + `STATE_LOCKED`.
- Local height **1:1**; `>+24.0"` → OLED `"Locked"`, latch flag, keep TX.
- Monitor: bases table ≤8, expire ~2.5 s; prefer LOCKED; **differential** hypsometric inches vs base Pa; OLED `"monitoring"` + ↑/↓ + 3-digit inches; stale without becoming Base.
- Scale: **no 6:1**.

### DbPacket (packed, 19 bytes, LE)
`magic 0xDB01 | ver | flags (ROLE_STATIONARY=0x01, ROLE_MONITOR=0x02, STATE_LOCKED=0x04) | uid | seq | press_mPa | pa_std_cPa | height_tenth_in | soft_crc`

### BLE (NimBLE Nordic UART stub)
- Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX `6e400002-...` (WRITE): `ZERO`/`CALIBRATE`, `OFFSET:<in>`, `STATUS`
- TX `6e400003-...` (NOTIFY): multi-base / local Pa telemetry stream (~2 Hz)

**Do not flash `/dev/ttyACM0` (Heltec V4 meshbot).**


---

## Device map / 2nd-DB attempt (2026-11-06 ~16:40 America/New_York)

| Board | USB ID | by-id / node | MAC (known) | Notes |
|-------|--------|--------------|-------------|-------|
| Heltec V4 meshbot | `303a:0002` | `/dev/serial/by-id/usb-Espressif_Systems_heltec_wifi_lora_32_v4__16_MB_FLASH__2_MB_PSRAM__F85B1BBF093C-if00` → **`/dev/ttyACM0`** | serial `F85B1BBF093C` | **DO NOT FLASH** |
| Heltec V3 DirtBeetle #1 (original base) | was `10c4:ea60` CP210x | was `/dev/ttyUSB0` | **`e8:3d:c1:e2:ae:f0`** (from prior upload) | Kernel: **USB disconnect 16:39:47** — `cp210x converter now disconnected from ttyUSB0`. Not present on USB during 2nd-DB attempt. May still be running on battery as Base (unverified this turn). |
| Heltec V3 DirtBeetle #2 (new) | — | **not enumerated** | unknown | User reported connected; after ~45s poll: **no** `ttyUSB*`, **no** `10c4:` device. Cannot identify or flash safely. |

**Outcome:** Flash + base/monitor link test **blocked**. Existing compiled firmware remains at `~/.cache/arduino/sketches/B95339D9ABA178B2BB80C1913D0778DD/bmp585_monitor.ino.bin` (and DirtFlatTop build tree). No board was flashed this turn; `/dev/ttyACM0` untouched.


---

## 2nd DB flash + link attempt (2026-11-06 ~16:43 America/New_York)

| Role | USB | Node | by-id | MAC |
|------|-----|------|-------|-----|
| V4 meshbot | `303a:0002` | **`/dev/ttyACM0`** | `…F85B1BBF093C-if00` | serial `F85B1BBF093C` — **untouched** |
| V3 DirtBeetle **#2 (NEW)** | `10c4:ea60` CP2102 serial `0001` | **`/dev/ttyUSB0`** | `usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0` | **`e8:3d:c1:e2:b0:f0`** (≠ original `e8:3d:c1:e2:ae:f0`) |
| V3 DirtBeetle **#1 (original base)** | not on USB | — | — | **`e8:3d:c1:e2:ae:f0`** — presumed battery / offline USB |

**Flash:** `arduino-cli upload -p /dev/ttyUSB0` same DirtBeetle clone (676896 B app) — **OK**. Verified post-flash MAC still `e8:3d:c1:e2:b0:f0`.

**Boot serial (`serial_db2_boot.log` / `serial_db2_boot2.log`):**
```
BMP585 init OK
SX1262 init OK
settling 3000 ms
zero locked at ~91866–9187x Pa
listening 2312 ms for base beacons
silence: becoming Stationary Base
BLE advertising
ready
```

**Link / Monitoring evidence:** **None.** No `heard base uid=…`, no `becoming Monitoring DB`, no differential inches telemetry. New DB elected **Stationary** due to listen-window silence. Original base RF TX not confirmed from this host (no USB to #1; no packets heard at #2). Antenna / battery / #1 still running: **unverified**. No 500 m claims. No firmware redesign; original base not flashed.


---

## OLED Stationary diagnostics (2026-11-06 ~16:52 America/New_York)

**Hermes:** TurboFCFusion IQ4_XS (`Current meshbot LLM` via Unsloth Studio → llama-server `:56113`) authored the surgical OLED/counter patch. Coordinator finished compile/flash after Hermes stalled looking for `esptool` on PATH.

**Target:** `/dev/ttyUSB0` CP2102 — MAC **`e8:3d:c1:e2:b0:f0`** (V3 #2). `/dev/ttyACM0` untouched. Backup: `bmp585_monitor.ino.bak_pre_diag`.

**Display:** `GEOMETRY_128_64`, font `ArialMT_Plain_10`, left-aligned. Layout notes in `/tmp/db_oled_layout.txt`. Runtime `getStringWidth` not called (fixed 5-row Plain_10; estimated max widths ~30–52px ≪ 128).

| y | Content |
|---|---------|
| 0 | `LK +012.3"` if Locked, else `   +012.3"` — signed **1:1** local inches (display capped ±999) |
| 12 | `TX <n>` / `TX <n>k` — successful `transmit==ERR_NONE` only (**not** RF delivery proof) |
| 24 | `RX <n> <age>s` / `RX <n> --` — validated peer frames only |
| 36 | `E<n> st:<code>` — TX failures + last radio status |
| 48 | `<Pa>Pa` live pressure |

**Build/flash:** 672,316 B flash / 32,444 B RAM; upload OK to `/dev/ttyUSB0`.

**Serial evidence (`serial_db_diag.log`):** BMP585 OK, SX1262 OK, silence→Stationary, then throttled lines e.g. `diag TX=1..34 RX=0 E=0 st=0 in≈0 Pa≈91872 locked=0`. TX counter advances; RX stays 0 (no peer). Physical OLED pixels not agent-verified — user should confirm panel. Two-device link still unverified.


## Original DB diagnostic flash — 2026-11-06 17:25 EST
Original MAC e8:3d:c1:e2:ae:f0 identified on /dev/ttyUSB0 (USB path port 1); second MAC e8:3d:c1:e2:b0:f0 on /dev/ttyUSB1 (port 2). Generic CP2102 serial 0001 collides: do not rely on by-id alone. Both MAC reads reset boards via esptool; second not flashed. V4 ttyACM0 untouched. Latest unchanged diagnostic cache (672768-byte app binary; 672316 sketch flash/32444 RAM) uploaded successfully to ORIGINAL ONLY. See upload_db1_diag.log and serial_db1_diag.log. Captured boot/diagnostics:
```
ESP-ROM:esp32s3-20210327
Build:Mar 27 2021
rst:0x1 (POWERON),boot:0x29 (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:1
load:0x3fce2820,len:0x1188
load:0x403c8700,len:0x4
load:0x403c8704,len:0xbf0
load:0x403cb700,len:0x30e4
entry 0x403c88ac
BMP585 init OK
SX1262 init OK
settling 3000 ms
zero locked at 91900.3 Pa
listening 2312 ms for base beacons
silence: becoming Stationary Base
BLE advertising
ready
diag TX=1 RX=0 E=0 st=0 in=0.1 Pa=91900 locked=0
diag TX=5 RX=0 E=0 st=0 in=-0.0 Pa=91900 locked=0
diag TX=9 RX=0 E=0 st=0 in=-0.0 Pa=91900 locked=0
diag TX=13 RX=0 E=0 st=0 in=-0.3 Pa=91901 locked=0
diag TX=17 RX=0 E=0 st=0 in=-0.5 Pa=91902 locked=0
diag TX=22 RX=0 E=0 st=0 in=-0.4 Pa=91901 locked=0
diag TX=26 RX=0 E=0 st=0 in=0.1 Pa=91900 locked=0
diag TX=30 RX=0 E=0 st=0 in=-0.7 Pa=91902 locked=0
diag TX=34 RX=0 E=0 st=0 in=-1.0 Pa=91903 locked=0
diag TX=39 RX=0 E=0 st=0 in=-0.6 Pa=91902 locked=0

```
Successful TX is not proof of RF delivery; valid RX/Monitoring evidence must be evaluated separately. Serial port closed.


---
## VERIFIED LoRa repair — 2026-11-07 05:50 EST (supersedes old unverified claims)

### Root causes established, not hardware guesses
1. **Clone UID collision:** `getUid()` truncated the LOW 32 bits of little-endian `ESP.getEfuseMac()`. Both attached V3s therefore returned `0xe2c13de8`. All role/peer receive paths explicitly reject `pkt.uid == getUid()`, so valid peer packets were discarded as self! Fixed to `(uint32_t)(mac >> 16)` preserving distinguishing device bytes: original `f0aee2c1`, second `f0b0e2c1`. This is a 32-bit derived ID, not a globally collision-proof full MAC.
2. **Overnight diagnostic sketches had entirely wrong pins:** `lora_tx_test`, `lora_rx_test`, `radiolib_pingpong` used NSS17/DIO18/RST16/BUSY20 (OLED/sensor conflict), NOT Heltec V3 radio pins. Their recurring init `-2` was not evidence of bad RF hardware. Do not flash those sketches. Preserved unchanged in backup.
3. **Receive lifecycle hardened:** replaced repeated blocking 50/200ms receives with DIO1 RX flag + continuous `startReceive()`, exact packet length + version + checksum checks, rearm after read/error AND each TX. Errors are now logged instead of silently converted to no peer. Explicit 1.8V TCXO, DIO2 RF switch and CRC enabled with checked return codes. Prior RadioLib already enables DIO2 by default; do NOT claim omission of DIO2 was the proven root cause. No CAD was present in current source; no CAD claim is made for this patch.
4. **Height math defect confirmed:** old standard-atmosphere altitude had `T0/(g*L)` instead of `T0/L`, under-reading by exactly g≈9.80665. Overnight code had added a ×10 multiplier, masking the error. Corrected formula, removed multiplier from local AND differential paths. Known synthetic +24in: pressure 91900→91893.233760 Pa; old formula 2.4473in, corrected 24.0000in. Physical tape-measure verification is still required. Corrected stability cPa encoding (Pa×100), monitor quote buffer (5 bytes).

### Proven reference and bench isolation
Opened and compared:
- RadioLib 7.8.1 official interrupt RX example: https://raw.githubusercontent.com/jgromes/RadioLib/7.8.1/examples/SX126x/SX126x_Receive_Interrupt/SX126x_Receive_Interrupt.ino
  Installed copy: `~/Arduino/libraries/RadioLib/examples/SX126x/SX126x_Receive_Interrupt/SX126x_Receive_Interrupt.ino`.
- Official Heltec V3 pin map: https://raw.githubusercontent.com/HelTecAutomation/Heltec_ESP32/master/src/driver/board-config.h
- Installed Heltec `examples/LoRaBasic/LoRaSender/LoRaSender.ino`, `src/driver/sx126x.c` lines 82/87 (1.8V DIO3 TCXO and DIO2 RF switch).
- Installed ESP32 3.1.0 `variants/heltec_wifi_lora_32_V3/pins_arduino.h`.

V3 pins are NSS8/DIO1=14/RST12/BUSY13, SPI9/11/10. Vext36 LOW retained for OLED; BMP Wire1 19/20 INT47 addr0x47 unchanged, no radio pin conflicts.

Temporary identical `lora_repair_probe` image (0dBm,915MHz,SF7,BW250,CR4/5,private0x12) verified **8 received payloads in EACH direction**, return codes 0, RSSI about -36..-40dBm, SNR11.8..13.5dB. Actual bytes included peer UID and sequence, not just TX success. See `probe_serial.log`. Temporary test image subsequently replaced with full DB clone on both V3s.

### Current production settings/behavior
Same repaired clone image both boards, 19-byte DbPacket format unchanged. Base beacons every200ms + UID%80 jitter (~4Hz), continuous RX between TX. **TX power deliberately 0dBm for bench verification**, not the prior22dBm. Frequency915MHz/BW250/SF7/CR4/5/sync0x12 retained. Field power/regulatory profile and 500m range NOT validated; do not represent this bench profile as a proven field configuration.
3s settle/animation, multi-base semantics (bases never demote), +24in LOCKED latch, live differential monitor, BMP/UI/BLE commands preserved. Boot base first and monitor second. Simultaneous boot/automatic election robustness and multi-base stability-selection logic need separate tests; not redesigned here. PRG zero on a running unit was not implemented in the preexisting source (only setup PRG re-zero and BLE ZERO existed); do not claim new runtime PRG support.

### Hardware mapping verified using esptool read_mac
- `/dev/ttyUSB0`, USB physical port1: original e8:3d:c1:e2:ae:f0 → uid f0aee2c1, now Base.
- `/dev/ttyUSB1`, USB physical port2: second e8:3d:c1:e2:b0:f0 → uid f0b0e2c1, now Monitor.
- CP2102 serial0001 duplicated, so by-id alone is ambiguous. Reidentify MAC before future uploads.
- `/dev/ttyACM0` V4 meshbot **never opened, reset, or flashed**.

### Builds, backups and maintenance
Backup BEFORE repairs: `repair_backup_20261107_054430_EST/` includes production and overnight tests plus build outputs and context/spec. Production build: `bmp585_monitor/build_repair`, 673720 bytes flash,32468 RAM. Build log `repair_compile.log`. Upload logs `repair_upload_db1.log`, `repair_upload_db2.log`. Final serial `repair_serial.log`, verification helper `verify_repair.py`. Analytical conversion tests `repair_scale_test.log`.

Hermes runaway session `local-172502720a3611096e5bd3b225058a665da6bf8f5a0959ed4c1be48908a5b577` was repeatedly compiling/flashing the wrong-pin example (log reached API call>2100). Initial terminal-only pause did not stop gateway-owned task, and it overwrote one probe upload; detected via actual serial and corrected. Gateway was briefly paused, then its single active turn deliberately canceled using supported `hermes gateway stop --yes`. Runtime restored with `hermes gateway ensure --timeout 30` (no service installed). Interactive terminal restored. Existing llama-server/Unsloth left running. **Do not resume the old runaway turn.** Future Hermes tasks must be bounded, grounded in these successful logs, and must not launch unattended flash loops. No new Hermes coding run was needed for this repair.

### Remaining measurement limitations
Live differential telemetry verified, NOT physical accuracy. Current raw sensor readings show several-Pa offset and ~inch-scale-to-many-inch jitter, now visible at proper scale; calibration/averaging/filter design remains. APK averaging/narrowband stability rejection not developed. Neither 24in physical movement nor LOCKED threshold was exercised remotely. No 500m test, no antenna inspection, no BLE client test or physical OLED pixel inspection. Keep antenna attached when transmitting.

### Final clean two-board verification
```text
/dev/ttyUSB0: SX1262 init OK MAC=f0aee2c13de8 uid=f0aee2c1 power=0 dBm
/dev/ttyUSB0: silence: becoming Stationary Base
/dev/ttyUSB1: SX1262 init OK MAC=f0b0e2c13de8 uid=f0b0e2c1 power=0 dBm
/dev/ttyUSB1: hearing base: becoming Monitoring DB
Validated peer packets: 76, sequences 69..144, missing=0
/dev/ttyUSB1: RX uid=f0aee2c1 seq=69 len=19 Pa=92210.625 RSSI=-36.0 SNR=12.2
/dev/ttyUSB1: RX uid=f0aee2c1 seq=144 len=19 Pa=92210.109 RSSI=-36.0 SNR=12.8
/dev/ttyUSB0: diag TX=143 RX=0 E=0 st=0 in=10.7 Pa=92209 locked=0
/dev/ttyUSB1: monitor RX=76 bases=1 localPa=92216.062 basePa=92210.109 diffIn=-21.01 rxErr=0 reject=0
```
Both upload logs contain verified flash hashes; serial handles closed after capture.
