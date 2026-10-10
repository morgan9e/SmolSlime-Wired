#!/usr/bin/env python3
"""Slime Link: radio link statistics per tracker, read from an nRF receiver over USB-HID.

Works with the stock SlimeVR receiver firmware (nRF52840 Dongle or a ProMicro receiver) and runs
alongside SlimeVR Server: every program that opens the receiver gets every report, so nothing
needs to be closed. It never writes to the receiver.

The receiver passes on what it decodes and drops the frame sequence numbers, so lost frames
cannot be counted exactly. What it shows is what a dropout looks like from the PC:

    rate      tracker packets in the last second (all types), and the lowest second so far
    RSSI      signal of the last packet that carries it, and average / worst since the start
    gaps      pauses between two packets longer than --gap (default 250 ms) and longer than 500 ms,
              and the longest pause; a tracker silent for --offline seconds (default 2) is "off"
              and its silence is counted as an offline event, not as a gap. A still tracker skips
              unchanged data and sends only every 100-200 ms, hence the default; while testing with
              moving trackers (which send every few ms), --gap 50 shows short dropouts too
    battery   percent and voltage, when the tracker sends it

    python slime_link.py                 live table, refreshed every second; Ctrl+C prints a summary
    python slime_link.py --csv link.csv  also write one row per tracker per second

Needs hidapi: python -m pip install hidapi
"""
import argparse
import csv
import sys
import time

VID, PID = 0x1209, 0x7690  # SlimeVR nRF receiver (stock and this repository's fork)
TRACKER_TYPES = range(8)    # tracker packets; 255 is the receiver's id -> address announcement


class Link:
    """Statistics of one tracker id on one receiver."""

    def __init__(self, rid, tid, now):
        self.rid, self.id = rid, tid
        self.addr = None
        self.start = now
        self.last = None            # time of the last tracker packet
        self.count = 0              # packets since the start
        self.second = 0             # packets in the current second
        self.rate = None            # packets in the last complete second
        self.min_rate = None        # lowest complete second while online
        self.partial = True         # the current second started mid-way (new, or back from offline)
        self.rssi = None
        self.rssi_sum = self.rssi_n = 0
        self.rssi_min = None
        self.gaps = self.long_gaps = 0
        self.max_gap = 0.0
        self.offline = 0            # silences longer than --offline
        self.is_off = False
        self.batt_pct = self.batt_v = None

    def packet(self, p, now, gap, offline_s):
        if self.last is not None:
            dt = now - self.last
            if dt >= offline_s:
                pass  # already counted as offline when the silence started
            elif dt > gap:
                self.gaps += 1
                if dt > 0.5:
                    self.long_gaps += 1
                self.max_gap = max(self.max_gap, dt)
        if self.is_off:
            self.partial = True
        self.last = now
        self.is_off = False
        self.count += 1
        self.second += 1
        kind = p[0]
        if kind not in (1, 4):  # byte 15 is the RSSI the receiver measured, as -dBm
            r = -p[15]
            if r < 0:
                self.rssi = r
                self.rssi_sum += r
                self.rssi_n += 1
                self.rssi_min = r if self.rssi_min is None else min(self.rssi_min, r)
        if kind in (0, 2):
            self.batt_pct = None if p[2] == 128 else min(100, p[2] & 127)
            self.batt_v = (p[3] + 245) / 100

    def tick(self, now, offline_s):
        """Close one second. Returns True when the tracker just went offline."""
        went_off = False
        if self.last is not None and not self.is_off and now - self.last >= offline_s:
            self.is_off = True
            self.offline += 1
            went_off = True
        self.rate = self.second
        if not self.is_off and not self.partial:
            self.min_rate = self.rate if self.min_rate is None else min(self.min_rate, self.rate)
        self.partial = False
        self.second = 0
        return went_off

    @property
    def rssi_avg(self):
        return self.rssi_sum / self.rssi_n if self.rssi_n else None


class Monitor:
    def __init__(self, gap=0.25, offline_s=2.0):
        self.gap, self.offline_s = gap, offline_s
        self.links = {}
        self.addrs = {}  # (receiver, id) -> address, from the receiver's 255 packets

    def report(self, rid, data, now):
        """One HID input report: four 16-byte packets (no report id)."""
        b = bytes(data)
        if len(b) == 65 and b[0] == 0:
            b = b[1:]
        if len(b) != 64:
            return
        for o in range(0, 64, 16):
            p = b[o:o + 16]
            if not any(p):
                continue
            kind, tid = p[0], p[1]
            if kind == 255:
                # little-endian on the wire; reversed, it reads as the receiver's "list" prints it
                self.addrs[(rid, tid)] = p[2:8][::-1].hex().upper()
                if (rid, tid) in self.links:
                    self.links[(rid, tid)].addr = self.addrs[(rid, tid)]
                continue
            if kind not in TRACKER_TYPES:
                continue
            key = (rid, tid)
            link = self.links.get(key)
            if link is None:
                link = self.links[key] = Link(rid, tid, now)
                link.addr = self.addrs.get(key)
            link.packet(p, now, self.gap, self.offline_s)

    def tick(self, now):
        return [l for l in self.links.values() if l.tick(now, self.offline_s)]

    def sorted(self):
        return sorted(self.links.values(), key=lambda l: (l.rid, l.id))


def fmt(v, spec, none='-'):
    return none if v is None else format(v, spec)


def table(mon, now, receivers):
    multi = len({l.rid for l in mon.links.values()}) > 1
    lines = [f'Slime Link  {time.strftime("%H:%M:%S")}  {receivers} receiver(s)  '
             f'gap > {mon.gap * 1000:.0f} ms, offline after {mon.offline_s:g} s    Ctrl+C: summary',
             '',
             f'{"id":>3} {"address":12} {"rate":>5} {"min":>4}  {"RSSI":>4} {"avg":>5} {"worst":>5}'
             f'  {"gaps":>5} {">500ms":>6} {"max gap":>8} {"off":>4}  {"battery":>11}  state']
    for l in mon.sorted():
        state = 'OFF' if l.is_off else ('no data' if l.last is None else
                                         f'{(now - l.last) * 1000:.0f} ms ago')
        batt = '-' if l.batt_pct is None else f'{l.batt_pct}% {l.batt_v:.2f}V'
        tid = f'{l.rid}:{l.id}' if multi else str(l.id)
        lines.append(f'{tid:>3} {l.addr or "?":12} {fmt(l.rate, "d"):>5} {fmt(l.min_rate, "d"):>4}'
                     f'  {fmt(l.rssi, "d"):>4} {fmt(l.rssi_avg, ".0f"):>5} {fmt(l.rssi_min, "d"):>5}'
                     f'  {l.gaps:>5} {l.long_gaps:>6} {l.max_gap * 1000:>6.0f}ms {l.offline:>4}'
                     f'  {batt:>11}  {state}')
    if not mon.links:
        lines.append('  no tracker packets yet')
    return '\n'.join(lines)


def summary(mon, now):
    out = ['', 'Summary']
    for l in mon.sorted():
        dur = max(now - l.start, 1e-9)
        out.append(f'  id {l.id} {l.addr or "?"}: {l.count} packets in {dur:.0f} s '
                   f'({l.count / dur:.1f}/s, lowest second {fmt(l.min_rate, "d")}), '
                   f'RSSI avg {fmt(l.rssi_avg, ".0f")} worst {fmt(l.rssi_min, "d")} dBm, '
                   f'{l.gaps} gaps > {mon.gap * 1000:.0f} ms ({l.long_gaps} > 500 ms, '
                   f'longest {l.max_gap * 1000:.0f} ms), offline {l.offline} times')
    return '\n'.join(out)


def open_receivers(hid, devices):
    for d in hid.enumerate(VID, PID):
        path = d['path']
        if path in devices:
            continue
        dev = hid.device()
        try:
            dev.open_path(path)
            dev.set_nonblocking(1)
            devices[path] = dev
        except (OSError, ValueError):
            dev.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gap', type=float, default=250, help='gap threshold in ms (default 250; 50 for moving trackers)')
    ap.add_argument('--offline', type=float, default=2, help='seconds of silence that count as offline')
    ap.add_argument('--csv', help='write one row per tracker per second to this file')
    ap.add_argument('--duration', type=float, help='stop after this many seconds')
    args = ap.parse_args(argv)
    try:
        import hid
    except ImportError:
        sys.exit('Needs hidapi: python -m pip install hidapi')
    if not hasattr(hid, 'device'):
        sys.exit('Install the hidapi package (not the different package named hid).')

    mon = Monitor(args.gap / 1000, args.offline)
    devices = {}
    rids = {}
    writer = None
    csv_file = open(args.csv, 'w', newline='') if args.csv else None
    if csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(['time', 'receiver', 'id', 'address', 'rate', 'rssi', 'rssi_avg', 'rssi_min',
                         'gaps', 'gaps_500ms', 'max_gap_ms', 'offline', 'battery_pct', 'battery_v'])
    t0 = time.monotonic()
    next_scan = next_tick = t0
    tty = sys.stdout.isatty()
    if tty:
        sys.stdout.write('\x1b[?25l')  # hide the cursor while redrawing
    try:
        while args.duration is None or time.monotonic() - t0 < args.duration:
            now = time.monotonic()
            if now >= next_scan:
                next_scan = now + 1
                open_receivers(hid, devices)
            for path, dev in list(devices.items()):
                rid = rids.setdefault(path, len(rids))
                try:
                    for _ in range(64):
                        data = dev.read(65)
                        if not data:
                            break
                        mon.report(rid, data, time.monotonic())
                except (OSError, ValueError):
                    devices.pop(path).close()
            if now >= next_tick:
                next_tick += 1
                mon.tick(now)
                if writer:
                    stamp = time.strftime('%Y-%m-%d %H:%M:%S')
                    for l in mon.sorted():
                        writer.writerow([stamp, l.rid, l.id, l.addr or '', l.rate, l.rssi,
                                         fmt(l.rssi_avg, '.1f', ''), l.rssi_min, l.gaps, l.long_gaps,
                                         round(l.max_gap * 1000), l.offline, l.batt_pct, l.batt_v])
                    csv_file.flush()
                text = table(mon, now, len(devices))
                if tty:
                    sys.stdout.write('\x1b[H\x1b[J' + text + '\n')
                else:
                    print(text + '\n')
                sys.stdout.flush()
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    finally:
        if tty:
            sys.stdout.write('\x1b[?25h')
        for dev in devices.values():
            dev.close()
        if csv_file:
            csv_file.close()
    print(summary(mon, time.monotonic()))


if __name__ == '__main__':
    main()
