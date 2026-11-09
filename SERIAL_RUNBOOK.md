# DirtBeetle serial runbook — tested 2026-11-07 EST

## One command; no model run required
```sh
/home/a/DirtFlatTop/tools/db_serial.py --device both --seconds 12
```
In Hermes terminal set `timeout: 25` for that 12-second capture (duration + startup
margin). Do not wrap this in `timeout ... cat`: the helper has its own monotonic
deadline. It flushes complete lines immediately, labels devices/times, writes a
JSONL file in `logs/`, prints a summary, and closes all serial handles in `finally`.
Exit 0 = bounded capture complete with data from every selected device. Exit 3 =
one or more ports were silent; this is not proof of a hardware fault. Exit 2 =
configuration, missing device/dependency, port busy, or I/O error; inspect the error
and correct it once, not an unattended retry loop. Ctrl-C returns 130 and closes.

```sh
# USB/sysfs metadata only; does NOT open serial or reset anything:
/home/a/DirtFlatTop/tools/db_serial.py --list
# Capture just one existing running device:
/home/a/DirtFlatTop/tools/db_serial.py --device second --seconds 8
# Only when boot diagnostics are wanted: preserve Base, reset Monitor explicitly:
/home/a/DirtFlatTop/tools/db_serial.py --device both --seconds 15 --reset second
# Explicit CP2102 port if cables/locations changed (check identity before flashing):
/home/a/DirtFlatTop/tools/db_serial.py --port /dev/ttyUSB1 --seconds 8
```
Default capture makes NO deliberate reset. Before pyserial opens it samples modem
line state using a guard FD, configures DTR/RTS to match, and clears HUPCL to avoid
close-time line drops. A kernel/USB driver may still pulse lines on first open;
zero-reset cannot be guaranteed. The tested no-reset capture saw ZERO ESP-ROM boot
markers and continued high sequence numbers on both boards. The helper never
writes serial payloads, flashes, reads MAC via esptool, or opens ttyACM devices.
An explicit reset pulses RTS/EN on only the named selected device. Reset-option
sequence is present but was intentionally not exercised in this serial-only task.

## Interpreter, ports, and contention
- Executable shebang `/usr/bin/python3`; verified pyserial3.5 installed for that
  interpreter. If accidentally run under Hermes's Python lacking pyserial, it
  re-execs `/usr/bin/python3` once. It never installs dependencies automatically.
- 115200 baud, 8N1, flow control off. Bounded nonblocking/select reads, unbuffered
  per-line output, no interactive terminal/miniterm required.
- Physical USB port1 by-path `pci-0000:07:00.3-usb-0:1:1.0-port0` currently original
  ae:f0 / ttyUSB0, UID f0aee2c1. Port2 `...usb-0:2:1.0-port0` currently second
  b0:f0 / ttyUSB1, UID f0b0e2c1. Both adapters are CP2102 10c4:ea60, serial0001.
  Duplicate by-id serial is NOT a unique board identity. Mapping comes from the
  earlier MAC-verified repair and unchanged sysfs locations, NOT a new MAC query.
  If boards/cables move, location mapping can become stale. `--list` says this.
- V4 meshbot ttyACM0 is prohibited by the helper and remained untouched.
- `fuser` preflight reports existing owner PIDs and refuses BEFORE opening any
  selected port. Exclusive advisory lock + TIOCEXCL block competing readers while
  capturing; locks are released on close. Never upload while a capture owns a port.
  Do not kill unknown owners. A race remains possible with another process opening
  between preflight and exclusive lock; treat the resulting error as contention.
- Capture BOTH ports in one process so observing Monitor does not repeatedly
  reopen/reset Base. Stagger boot Base first, then Monitor when actually needed.

## Why Hermes wasted time — observed, not guesses
Evidence: `logs/serial_failure_evidence.json` contains selected redacted command /
result pairs from the SINGLE exported overnight session (not a claim that every
failure happened in a fresh conversation).

1. **Wrong Python vs pip.** `python3 -c 'import serial...'` returned
   `ModuleNotFoundError: No module named 'serial'`. `pip install pyserial` replied
   `Requirement already satisfied ... /home/a/.local/lib/python3.10/site-packages`.
   A subsequent miniterm attempt named Hermes's managed venv Python, which still
   lacked serial. Bare pip and python3 targeted different environments. The helper
   fixes this without changing Hermes's managed install.
2. **Timeout misinterpretation and repetitive tooling.** `timeout 15 cat
   /dev/ttyUSB0` returned exit124 even while output contained many valid `diag
   TX=...` lines. GNU timeout124 means its deadline was reached, not serial failure.
   Export includes470 such records,125 with nonempty output. The model kept
   retrying; a correct capture should stop and summarize. Invalid long stty flags
   also failed (`stty: invalid argument '-iextc'`). Current helper returns0 on
   successful bounded capture, eliminating that misleading status.
3. **esptool detours were not needed to read UART.** `esptool.py ... chip_id`
   returned `command not found`; direct execution of a PlatformIO esptool.py path
   returned `Permission denied`. That denial was SCRIPT execution, not tty access.
   We found no evidence here of a tty group/baud problem; current user is in dialout,
   both ports readable/writable, and115200 produces correct text.
4. **Contention was real at least once.** Upload reported `Could not exclusively
   lock port ... [Errno11] Resource temporarily unavailable` while another agent
   was capturing the same port. This proves contention in that attempt, not that
   all overnight failures were due to port ownership.
5. **Persistence/discovery mismatch.** Exported session CWD was `/home/a/.hermes`.
   No AGENTS.md existed in project, home, or Hermes home. `HERMES_CONTEXT.md` was
   manually read earlier, but is NOT a supported auto-discovery filename. Shell
   `cd /home/a/DirtFlatTop && upload...` does not retroactively rebuild an existing
   session's startup prompt. Stored prompt had no HERMES_CONTEXT.md/pyserial command.
   Local docs say context and memory snapshots are frozen per session; retaining
   a long history is not the same as loading a durable procedure each fresh run.
6. **Runaway rather than bounded diagnosis.** The single exported session reached
   2,352 API calls and thousands of messages. Repeating a failed import or a working
   timed capture did not add evidence. Old task remains canceled; no new model
   loop was started for this fix. AGENTS.md now directs one useful capture and stop.

Not established from these excerpts: bad baud, denied tty permissions, killed
background captures, container isolation, or a universal reset/missed-startup root
cause. Previous repair did observe reopening serial could reset boards; this
helper explicitly controls that instead of assuming opens are passive.

## What persists / how Hermes discovers it
- `AGENTS.md`: concise auto-discovered project procedure, ports, safety, exact
  command and pointers to this runbook/current repair context.
- `tools/hermes_db`: fresh-session launcher with `hermes --in /home/a/DirtFlatTop`.
  ```sh
  /home/a/DirtFlatTop/tools/hermes_db
  ```
  No old-session resume flag is injected. Do not resume runaway session
  local-172502720a3611096e5bd3b225058a665da6bf8f5a0959ed4c1be48908a5b577.
- `HERMES_CONTEXT.md`: detailed progress history, explicitly linked by AGENTS.md.
- `tools/db_serial.py`: deterministic reusable operation; not dependent on model
  recollection or a new pip install.

Verified against installed Hermes docs:
`website/docs/user-guide/features/context-files.md` (supported names, priority,
non-git CWD-only discovery), `user-guide/which-file-does-what.md` (frozen snapshots),
`user-guide/features/memory.md` (session boundaries). Source manifest check using
`agent.context_file_sources.list_context_file_sources(..., skip_soul=True)` found
project AGENTS.md status=loaded, and NO project instruction at old CWD ~/.hermes.
Offline `tools/hermes_db prompt-size --json` tests the launcher without API calls.
A fresh project session gets AGENTS.md automatically. An already-running session,
or one launched elsewhere, is NOT guaranteed to get it; start a fresh session in
this directory. No unrelated global memory, model services or settings changed.

## Tests / actual evidence
- 12-second dual capture: exit0, original12 lines / second62 lines, boot markers0
  each. Base TX1942 and Monitor RX1874 continued, rxErr0/reject0. Raw log:
  `logs/serial_20261107_055704_EST.jsonl`; console `logs/serial_helper_test_console.log`.
- A simultaneous second helper refused with owner PID, without opening either port.
- Running helper `--list` under the exact failed Hermes managed Python re-executed
  /usr/bin/python3 and succeeded; metadata only, no resets.
- `--port /dev/ttyACM0 --list` refuses the V4 before opening it.
- Syntax check and offline project instruction loading pass. No firmware changes,
  reflashes, intentional resets, model loops, or lingering serial owners.

### Final helper validation (2026-11-07 05:59 EST)
Final-version five-second capture: original5 lines / second26, zero boot markers,
exit0; expected busy-port refusal exit2; V4 refusal exit2 before open; exact failed
Hermes interpreter fallback exit0; no remaining port owners. Test results:
`logs/serial_helper_tests.json`; auto-discovery proof:
`logs/hermes_context_discovery.json`; offline launcher budget:
`logs/hermes_project_prompt_size.json`. No model calls.
