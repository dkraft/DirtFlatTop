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
