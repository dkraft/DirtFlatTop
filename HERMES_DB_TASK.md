# DirtBeetle firmware task (EXECUTE NOW)

You are Hermes on the local workstation. Model must be TurboFCFusion IQ4_XS via the configured Unsloth/llama endpoint (`Current meshbot LLM` / mesh-llama-56113). Work ONLY in `/home/a/DirtFlatTop`.

## Goal
Implement identical clone DirtBeetle (DB) firmware in `bmp585_monitor/bmp585_monitor.ino` per `DIRTBEETLE_SPEC.md` (AUTHORITATIVE sections 5–11 already resolved by David). Then compile, flash ONLY `/dev/ttyUSB0`, verify serial, update `HERMES_CONTEXT.md`.

## Already done for you
- Backup exists: `bmp585_monitor/bmp585_monitor.ino.bak_pre_db`
- RadioLib 7.8.1 installed
- NimBLE-Arduino, DFRobot_BMP58X, Heltec boards installed
- BMP585 proven at 0x47 on Wire1 SDA=19 SCL=20 INT=47
- Heltec V3 on `/dev/ttyUSB0` (10c4:ea60). NEVER touch `/dev/ttyACM0`.

## Must implement
1. Boot 3s: circular Grok Bot face, eyes look left/right, text "scanning"; settle BMP585; lock local refPa.
2. RadioLib SX1262: pins NSS=8 DIO1=14 NRST=12 BUSY=13; SPI SCK=9 MISO=11 MOSI=10; 915.0 MHz SF7 BW250 CR4/5 sync 0x12 preamble 8 CRC on +20 dBm.
3. DbPacket with magic 0xDB01, uid from MAC, flags ROLE_STATIONARY|STATE_LOCKED, seq, press_mPa, pa_std_cPa stability, height_tenth_in, soft_crc.
4. Listen ~2s + UID jitter; PRG held → force Stationary Base. Silence → Base. Hear Base → Monitoring. Bases NEVER auto-demote (multi-base OK).
5. Base TX ~5 Hz with 185–225 ms UID jitter + scanChannel CAD/LBT; live Pa + stability.
6. Local height 1:1 true inches (NO 6:1). If height > +24.0" → OLED "Locked", latch STATE_LOCKED, keep TX.
7. Monitoring: bases table up to 8, expire ~2.5s; prefer LOCKED; differential hypsometric inches vs base Pa; OLED "monitoring" + ↑/↓ + 3-digit inches; stale → "lost"/"----" without becoming Base.
8. NimBLE Nordic UART service UUIDs in spec; stream multi-base telemetry; commands ZERO/CALIBRATE, OFFSET:<in>, STATUS. Non-blocking.
9. Keep HT_SSD1306Wire OLED + Vext; PRG=GPIO0 re-zero.

## Build / flash / verify (run these yourself)
```bash
cd /home/a/DirtFlatTop
~/.local/bin/arduino-cli compile --fqbn esp32:esp32:heltec_wifi_lora_32_V3 bmp585_monitor
# Fix any compile errors until 0 errors.
~/.local/bin/arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:heltec_wifi_lora_32_V3 bmp585_monitor
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
time.sleep(0.5)
end = time.time() + 12
while time.time() < end:
    if s.in_waiting:
        for line in s.read_all().decode(errors='replace').splitlines():
            if line.strip(): print(line)
s.close()
"
```

## HERMES_CONTEXT.md
Append a milestone: DirtBeetle v1 LoRa+roles+BLE stub; packet format; BLE UUIDs; backup path; 1:1 differential multi-base; Locked >+24; flash port /dev/ttyUSB0 only.

## Deliverable
When done, print a short summary: compile flash/RAM sizes, flash OK, serial boot lines observed, any caveats.
