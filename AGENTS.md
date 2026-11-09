# DirtBeetle / DirtFlatTop project instructions

## Start here; retain proven procedures
Read `SERIAL_RUNBOOK.md` before any serial/USB work. Read the newest sections of
`HERMES_CONTEXT.md` and `LORA_REPAIR_NOTES.md` before firmware work; older milestone
claims are historical and may be superseded. `HERMES_CONTEXT.md` is a referenced
runbook, not an automatically discovered Hermes context filename.

## Serial command — do not relearn this each turn
```sh
/home/a/DirtFlatTop/tools/db_serial.py --device both --seconds 12
```
The executable uses `/usr/bin/python3` (pyserial3.5 installed), not Hermes's
managed Python. It captures both V3s together, timestamps/flushed output, logs
JSONL, and closes handles at its deadline. Set terminal tool timeout to25 seconds
for a12-second capture. No separate cat/stty/miniterm loop, bare pip install,
esptool MAC probe, reflash, or board reset is needed to read ongoing diagnostics.
Exit0 is a completed capture; exit3 means a selected port was silent, NOT an RF
hardware diagnosis. Stop after one useful capture and summarize. On error inspect
that error once; do not repeat identical commands or enter unattended loops.

`--list` checks USB location metadata without opening ports. The saved mapping is
original ae:f0 at physical USB port1, second b0:f0 at port2; generic CP2102 serial0001
is duplicated. Location metadata is not a fresh MAC measurement. Recheck identity
before flashing, particularly after cable swaps. Never open/reset/flash V4 ttyACM0.
`--reset second` deliberately resets only Monitor while Base stays open/running;
use only when startup diagnostics are actually needed. Default preserves control
line states and does not deliberately reset; USB drivers can still pulse on open.
Busy ports: helper reports owners and exits. Do not kill unknown processes.

## Hermes persistence
This AGENTS.md is auto-discovered for a FRESH session started in this directory.
Use `/home/a/DirtFlatTop/tools/hermes_db` to start there. Non-git projects do not
inherit AGENTS.md from parent directories. Existing sessions cache context: do not
assume editing a file changes their prompt. Never resume the old runaway session
local-172502720a3611096e5bd3b225058a665da6bf8f5a0959ed4c1be48908a5b577.
Do not spawn a new model task merely to run serial capture.
