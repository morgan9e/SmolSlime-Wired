"""Capture synthetic ESB diagnostics, 30 s TDMA / legacy / TDMA by default.

Requires diagnostic trackers and matching receiver (docs/RADIO_TEST.md).
Raw serial chunks and parsed per-interval counters are retained; no pose data.
"""
import argparse
import csv
import datetime
import json
from pathlib import Path
import re
import time

import serial
from serial.tools import list_ports

ANSI = re.compile(r'\x1b\[[0-9;]*m')
TEST = re.compile(r'rftest id (\d+): (.+)')
DIAG = re.compile(r'diag id (\d+): rx (\d+), lost (\d+) \(([\d.]+)%\), dup (\d+), late (\d+), max gap (\d+) ms, RSSI -(\d+) \(worst -(\d+)\)')


def tone(start):
    try:
        import winsound
        for hz in ([880, 1175] if start else [660, 440]):
            winsound.Beep(hz, 180)
    except ImportError:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--duration', type=float, default=30)
    ap.add_argument('--modes', nargs='+', choices=['tdma', 'legacy'], default=['tdma', 'legacy', 'tdma'])
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    if args.duration <= 0:
        ap.error('duration must be positive')
    stamp = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=9))).strftime('%Y%m%d-%H%M%S-KST')
    folder = args.output or Path('dist/diagnostics') / ('synthetic-' + stamp)
    folder.mkdir(parents=True, exist_ok=False)
    ports = [p.device for p in list_ports.comports() if p.vid == 0x1209 and p.pid == 0x7690]
    if len(ports) != 1:
        raise RuntimeError(f'Expected one receiver, found {ports}')
    s = serial.Serial(ports[0], 115200, timeout=0, write_timeout=2)
    s.dtr = True
    print('OUTPUT', folder, flush=True)
    raw = (folder/'raw.jsonl').open('w', encoding='utf8')
    origin = time.monotonic()
    stage = 'setup'
    def read():
        b = s.read(s.in_waiting or 1)
        if b:
            text = b.decode(errors='replace')
            raw.write(json.dumps(dict(t=time.monotonic()-origin, stage=stage, text=text))+'\n')
            return ANSI.sub('', text)
        return ''
    def command(line, wait=.6):
        s.write((line+'\n').encode())
        end = time.monotonic()+wait
        out = ''
        while time.monotonic() < end:
            out += read()
            time.sleep(.002)
        return out
    def mode(wanted):
        status = command('radio status')
        if f'mode={wanted} target={wanted} phase=idle' in status:
            return
        command('radio mode '+wanted)
        deadline = time.monotonic()+12
        while time.monotonic() < deadline:
            status = command('radio status')
            if 'phase=idle' in status:
                if f'mode={wanted} target={wanted} phase=idle' in status and 'result=success' in status:
                    return
                raise RuntimeError('Mode switch failed: '+status)
        raise TimeoutError('Mode switch did not finish')
    try:
        command('diag off')
        listing = command('list')
        names = {i:a[-2:] for i,a in enumerate(re.findall(r'^([0-9A-F]{12})\s*$', listing, re.M))}
        status = command('radio status')
        active = re.search(r'active=([0-9a-f]+)', status)
        if not active:
            raise RuntimeError('No active tracker snapshot')
        mask = int(active[1], 16)
        telemetry = command('tdma')
        ready = 0
        for line in telemetry.splitlines():
            m = TEST.search(line)
            if m:
                fields = dict(re.findall(r'(\w+)=(\w+)', m[2]))
                if fields.get('confirmed') == '1' and fields.get('imu') == '0' and fields.get('hz') == '100' and fields.get('kinds') == '1f' and int(fields['age_ms']) < 1500:
                    ready |= 1 << int(m[1])
        if not mask or mask & ~ready:
            raise RuntimeError(f'Active mask {mask:02x}, confirmed 100 Hz radio-only mask {ready:02x}; finish rollout/turn off other transmitters first')
        (folder/'setup.json').write_text(json.dumps(dict(active_mask=mask, names=names, modes=args.modes, duration=args.duration), indent=2))
        with (folder/'intervals.csv').open('w',newline='') as df, (folder/'counters.csv').open('w',newline='') as tf:
            dw=csv.writer(df);dw.writerow(['stage','t','tracker','rx','lost','loss_pct','duplicates','late','max_gap_ms','rssi','worst_rssi'])
            tw=csv.writer(tf);tw.writerow(['stage','t','tracker','generated','skipped','late','submitted','replaced','acked','failed','errors','hz','confirmed','imu','kinds','age_ms'])
            for index, wanted in enumerate(args.modes):
                stage = 'switch'
                mode(wanted)
                command('diag on', 1.3)
                stage = f'{index+1}-{wanted}'
                print('START',stage,flush=True);tone(True)
                start=time.monotonic();poll=0;progress=10;buf=''
                while time.monotonic()-start < args.duration:
                    elapsed=time.monotonic()-start
                    if elapsed >= poll:
                        s.write(b'tdma\n');poll+=1
                    buf += read()
                    while '\n' in buf:
                        line,buf=buf.split('\n',1)
                        m=DIAG.search(line)
                        if m:
                            v=list(m.groups());tid=int(v.pop(0))
                            v[-2:]=[-int(v[-2]),-int(v[-1])]
                            dw.writerow([stage,round(elapsed,3),names.get(tid,str(tid)),*v])
                        m=TEST.search(line)
                        if m:
                            v=dict(re.findall(r'(\w+)=(\w+)',m[2]))
                            tw.writerow([stage,round(elapsed,3),names.get(int(m[1]),m[1]),*[v.get(k,'') for k in ['generated','skipped','late','submitted','replaced','acked','failed','errors','hz','confirmed','imu','kinds','age_ms']]])
                    if elapsed >= progress:
                        print(stage,round(elapsed),'seconds',flush=True);progress+=10
                    time.sleep(.002)
                tone(False);print('END',stage,flush=True)
                stage='boundary';command('diag off')
        print('COMPLETE',folder,flush=True)
    finally:
        stage='cleanup'
        try:
            command('diag off');mode('tdma')
        finally:
            raw.close();s.close()


if __name__ == '__main__':
    main()
