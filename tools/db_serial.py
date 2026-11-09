#!/usr/bin/python3
"""Bounded local DirtBeetle serial capture. Never flashes or probes MAC via reset."""
import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import termios
import time
from datetime import datetime
from zoneinfo import ZoneInfo

# Hermes may run python3 from its isolated managed environment, not system Python.
try:
    import serial
except ImportError:
    system = '/usr/bin/python3'
    if os.path.realpath(sys.executable) != os.path.realpath(system):
        print('# pyserial absent in '+sys.executable+'; re-executing '+system, file=sys.stderr, flush=True)
        os.execv(system, [system, str(Path(__file__).resolve()), *sys.argv[1:]])
    print('ERROR: system Python lacks pyserial. Ask before installing; use /usr/bin/python3 -m pip, not bare pip.', file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parents[1]
MAP = {
 'original': ('pci-0000:07:00.3-usb-0:1:1.0-port0', 'e8:3d:c1:e2:ae:f0', 'f0aee2c1'),
 'second': ('pci-0000:07:00.3-usb-0:2:1.0-port0', 'e8:3d:c1:e2:b0:f0', 'f0b0e2c1'),
}


def metadata(port):
    """Read sysfs only: no serial open, DTR pulse, esptool, or MAC claim."""
    target = Path(port).resolve()
    if not target.name.startswith('ttyUSB'):
        raise ValueError('Only CP2102 ttyUSB ports allowed; V4 ttyACM ports are prohibited')
    base = Path('/sys/class/tty')/target.name/'device'
    if not base.exists():
        raise ValueError(str(target)+' is missing')
    for p in (base.resolve(), *base.resolve().parents):
        if (p/'idVendor').exists():
            vid = (p/'idVendor').read_text().strip()
            pid = (p/'idProduct').read_text().strip()
            if (vid,pid) != ('10c4','ea60'):
                raise ValueError('Unexpected USB adapter '+vid+':'+pid)
            return str(target), vid+':'+pid
    raise ValueError('Cannot confirm CP2102 adapter from sysfs')


def owners(port):
    done = subprocess.run(['/usr/bin/fuser', port], capture_output=True, text=True, timeout=2)
    return done.stdout.strip()


def open_preserving_lines(port):
    # A driver can pulse lines on first open even before Python can preserve them.
    # Guard FD samples current control state; clearing HUPCL avoids close-time drop.
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        bits = bytearray(4)
        fcntl.ioctl(fd, termios.TIOCMGET, bits, True)
        bits = int.from_bytes(bits, sys.byteorder)
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        s = serial.Serial(port=None, baudrate=115200, timeout=0, exclusive=True,
                          xonxoff=False, rtscts=False, dsrdtr=False)
        s.dtr = bool(bits & termios.TIOCM_DTR)
        s.rts = bool(bits & termios.TIOCM_RTS)
        s.port = port
        try:
            s.open()
            # Prevent noncooperating readers (cat, monitors) until finally close.
            fcntl.ioctl(s.fileno(), termios.TIOCEXCL)
            return s
        except BaseException:
            s.close()
            raise
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', choices=['original','second','both'], default='both')
    ap.add_argument('--port', help='Explicit CP2102 port; overrides saved location mapping')
    ap.add_argument('--seconds', type=float, default=12, help='Capture duration, 1..120 seconds')
    ap.add_argument('--reset', action='append', choices=['original','second','explicit'], default=[],
                    help='Deliberately reset ONLY named selected device; default: no deliberate reset')
    ap.add_argument('--list', action='store_true', help='Metadata only; does not open serial ports')
    ap.add_argument('--output', help='JSONL log path; default: timestamped logs/serial_*.jsonl')
    a = ap.parse_args()
    if not 1 <= a.seconds <= 120:
        ap.error('--seconds must be 1..120')
    names = ['explicit'] if a.port else (list(MAP) if a.device=='both' else [a.device])
    if set(a.reset)-set(names):
        ap.error('--reset must name a selected device')
    selected=[]
    try:
        for name in names:
            path = a.port if name=='explicit' else '/dev/serial/by-path/'+MAP[name][0]
            port, usb = metadata(path)
            selected.append((name,port))
            print('# '+json.dumps({'device':name,'port':port,'usb':usb,
                'known_mac_at_location':MAP[name][1] if name in MAP else None,
                'identity':'location metadata only; MAC not queried','python':sys.executable}), flush=True)
        if len({p for _,p in selected}) != len(selected):
            raise ValueError('Two device names resolve to the same port')
        if a.list:
            return 0
        # Check ALL ports before opening any. Never kill an unknown port owner.
        for name,port in selected:
            pids=owners(port)
            if pids:
                raise ValueError(port+' busy; owner PID(s): '+pids+'. Stop the known monitor normally, then retry once.')
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        print('ERROR: '+str(e),file=sys.stderr);return 2
    out=Path(a.output).expanduser() if a.output else ROOT/'logs'/('serial_'+datetime.now(ZoneInfo('America/New_York')).strftime('%Y%m%d_%H%M%S_%Z')+'.jsonl')
    out.parent.mkdir(parents=True,exist_ok=True)
    handles={};buffers={};counts={};boots={};log=None
    def emit(name,port,line):
        stamp=datetime.now(ZoneInfo('America/New_York'))
        row={'time':stamp.isoformat(timespec='milliseconds'),
             'zone':stamp.tzname(),'device':name,'port':port,'line':line}
        print(row['time']+' '+row['zone']+' '+name+' '+line,flush=True)
        log.write(json.dumps(row)+'\n');log.flush()
        counts[name]+=1
        if 'ESP-ROM:' in line:boots[name]+=1
    try:
        log=out.open('x',buffering=1)
        for name,port in selected:
            s=open_preserving_lines(port);handles[s]=(name,port);buffers[s]=b'';counts[name]=0;boots[name]=0
        # All selected ports stay open together; resetting Monitor never resets Base.
        for s,(name,port) in handles.items():
            if name in a.reset:
                print('# deliberate RESET '+name,flush=True)
                s.dtr=False;s.rts=True;time.sleep(.1);s.rts=False;time.sleep(.05)
        end=time.monotonic()+a.seconds
        while time.monotonic()<end:
            for s in select.select(list(handles),[],[],min(.2,max(0,end-time.monotonic())))[0]:
                data=s.read(4096)
                if not data:continue
                buffers[s]+=data
                while b'\n' in buffers[s]:
                    line,buffers[s]=buffers[s].split(b'\n',1)
                    emit(*handles[s],line.decode(errors='replace').rstrip('\r'))
                if len(buffers[s])>16384:
                    emit(*handles[s],buffers[s].decode(errors='replace')+' [unterminated chunk]');buffers[s]=b''
        for s,b in buffers.items():
            if b:emit(*handles[s],b.decode(errors='replace')+' [partial]')
        print('# SUMMARY '+json.dumps({'lines':counts,'boot_markers':boots,'log':str(out),
              'reset_requested':a.reset,'complete':True}),flush=True)
        return 0 if all(counts.values()) else 3
    except KeyboardInterrupt:
        print('Interrupted; closing ports',file=sys.stderr);return 130
    except (OSError,ValueError,serial.SerialException) as e:
        print('ERROR: '+str(e)+'; no automatic retry',file=sys.stderr);return 2
    finally:
        for s in handles:
            try:fcntl.ioctl(s.fileno(),termios.TIOCNXCL)
            except OSError:pass
            s.close()
        if log:log.close()

if __name__=='__main__':
    sys.exit(main())
