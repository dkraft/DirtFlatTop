# DirtBeetle OLED Stationary diagnostics (SURGICAL PATCH ONLY)

Work in `/home/a/DirtFlatTop/bmp585_monitor/bmp585_monitor.ino`.
Backup already exists: `bmp585_monitor.ino.bak_pre_diag`. Do NOT touch `/dev/ttyACM0`.

## Goal
Replace the large `"BASE"` OLED screen with compact **true diagnostics** for Stationary/Locked role on Heltec V3 SSD1306 **128×64** (`GEOMETRY_128_64`). Keep Monitoring UI, boot/scanning, role election, calibration, and LoRa packet format unchanged.

## Required counters (actual success, not attempts)
Add globals (uint32_t ok; separate err counters):
- `txOk` — incremented **only** when `radio.transmit(...) == RADIOLIB_ERR_NONE` (after successful TX). Do **not** treat TX count as RF delivery proof.
- `txFail` — increment on transmit != ERR_NONE; store `lastRadioStatus` = that state code.
- `rxOk` — increment **only** when `recvPacket()` fully validates magic+CRC (and for base loop, when peer ROLE_STATIONARY heard). During Stationary role, still count any validated peer frames in `rxOk` if you add a brief non-blocking RX window between TXes **without changing role logic** (optional). Minimum: count validated packets wherever `recvPacket` already succeeds (listen window + monitor path). For Stationary diagnostics, also attempt short RX between TX (e.g. recvPacket 5–15ms) solely to increment `rxOk` / `lastRxMs` — **do not** demote role.
- `rxBad` optional for magic/CRC fail if easy.
- `lastRxMs` for age display.

Format counters for OLED with compact suffix to fit ≤128px (e.g. raw until 9999, then `12k`-style or `9999+`). Prefer left-align Plain_10.

## OLED layout (Stationary) — verify with getStringWidth
Use `ArialMT_Plain_10`, `TEXT_ALIGN_LEFT`, `setColor(WHITE)`. About 4–5 rows, y≈0,12,24,36,48 (10px font). Example target (must measure widths ≤126):

```
LK +012.3"     // or "B  +012.3\"" when not locked; clear Locked: prefix "LK" or line "Locked" if width allows with inches
TX 1234        // successful TX only
RX 0           // validated RX peer frames
E0 st:0        // txFail + lastRadioStatus
91867Pa        // live Pa OR "rx --" / "rx 1.2s" age — pick what fits
```

Rules:
- Signed **1:1** local inches via existing `inchesDelta(livePa)` (not 6:1).
- Cap display inches magnitude for string (e.g. show `+/-999` max) without changing lock threshold logic (`>+24` still latches Locked).
- When locked, indication must remain obvious (`LK` prefix and/or `Locked` on a row).
- Call `display.getStringWidth(buf)` and if >126, shorten (drop Pa decimals, use compact counts).
- No Page/canvas required. Optional: write `/tmp/db_oled_layout.txt` with final row strings for representative states.

## Serial diagnostics (throttled)
Every ~1s while Stationary: `Serial.printf` one line: `diag TX=%lu RX=%lu E=%lu st=%d in=%.1f Pa=%.0f locked=%d` — helps no-reception debug. No spam.

## Build/flash/verify
```bash
cd /home/a/DirtFlatTop
~/.local/bin/arduino-cli compile --fqbn esp32:esp32:heltec_wifi_lora_32_V3 bmp585_monitor
# fix compile errors yourself
~/.local/bin/arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:heltec_wifi_lora_32_V3 bmp585_monitor
# capture ~12s serial after RTS reset; confirm BMP585/SX1262 OK and diag lines
```
Confirm chip MAC is `e8:3d:c1:e2:b0:f0` before/after flash (esptool read_mac). Never ACM0.

Update `HERMES_CONTEXT.md` with OLED layout rows + flash/serial outcome.

Print short summary when done.
