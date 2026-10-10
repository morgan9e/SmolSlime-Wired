#!/usr/bin/env python3
"""Slime Diag: live radio link diagnostics from the nRF receiver, relayed by the hub.

Turn them on once on the receiver's USB console with "diag hub on" (stored; "diag hub off"
stops them). Every second the receiver sends its statistics to the hub, and the hub forwards
them unchanged to UDP 6971 on the PC it streams to (SlimeVR Server or slime_view.py), so this
runs next to either of them. Standard library only.

    python slime_diag.py [--port 6971] [--log diag.csv] [--plain]

Per tracker: frames received, frames lost in the air (the tracker's sequence numbers that
never arrived), repeats (the tracker missed the receiver's ACK), the longest silence, signal
strength, and silences that lost frames. For the receiver: radio CRC good/bad, background
noise (median about -95 dBm on a clean channel), bit rate, and hub UART drops.
On Windows, allow Python through the firewall for private networks when asked.
"""
import argparse
import csv
import os
import socket
import struct
import sys
import time

DIAG_PORT = 6971
TYPE_RECEIVER, TYPE_TRACKER = 253, 254


def decode(p):
    """Decodes one 16-byte diagnostic packet (layout in src/Receiver/HUB.md), or returns None."""
    if len(p) != 16:
        return None
    if p[0] == TYPE_RECEIVER:
        ok, bad = struct.unpack_from('<HH', p, 2)
        report, dropped = struct.unpack_from('<HH', p, 12)
        return {'kind': 'receiver', 'crystal': bool(p[1] & 1), 'mbps': 1 if p[1] & 2 else 2,
                'counters': bool(p[1] & 4), 'crc_ok': ok, 'crc_bad': bad, 'noise_median': -p[6],
                'noise_quiet': -p[7], 'noise_loud': -p[8], 'noise_above_80_pct': p[9],
                'stored': p[10], 'report': report, 'uart_dropped': dropped}
    if p[0] == TYPE_TRACKER:
        rx, lost = struct.unpack_from('<HH', p, 2)
        max_gap, = struct.unpack_from('<H', p, 8)
        report, = struct.unpack_from('<H', p, 12)
        return {'kind': 'tracker', 'id': p[1], 'rx': rx, 'lost': lost, 'dup': p[6], 'late': p[7],
                'max_gap_ms': max_gap, 'rssi': -p[10] if rx else None, 'rssi_worst': -p[11] if rx else None,
                'report': report, 'gaps': p[14], 'outages': p[15]}
    return None


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


class Totals:
    """Running totals per tracker since start."""
    def __init__(self):
        self.rx = {}
        self.lost = {}

    def add(self, t):
        self.rx[t['id']] = self.rx.get(t['id'], 0) + t['rx']
        self.lost[t['id']] = self.lost.get(t['id'], 0) + t['lost']

    def loss(self, tid):
        return pct(self.lost.get(tid, 0), self.rx.get(tid, 0) + self.lost.get(tid, 0))


def format_report(receiver, trackers, totals, source):
    lines = [f'{time.strftime("%H:%M:%S")}  from hub {source}  (Ctrl+C to quit)', '']
    if receiver:
        r = receiver
        crc = (f'{pct(r["crc_bad"], r["crc_ok"] + r["crc_bad"]):4.1f}% bad CRC '
               f'({r["crc_bad"]} of {r["crc_ok"] + r["crc_bad"]})' if r['counters'] else 'no CRC counters')
        lines.append(f'receiver  {r["mbps"]} Mbps, {"crystal" if r["crystal"] else "NO CRYSTAL"}, {crc}, '
                     f'noise {r["noise_median"]} dBm (loudest 10% {r["noise_loud"]}, '
                     f'{r["noise_above_80_pct"]}% above -80), hub UART drops {r["uart_dropped"]}')
    else:
        lines.append('receiver  (no receiver packet in this report)')
    lines.append('')
    lines.append(' id   rx/s  lost/s  lost%  total%  dup  late  max gap  RSSI  worst  silences  outages')
    for t in sorted(trackers, key=lambda t: t['id']):
        rssi = f'{t["rssi"]:5d}  {t["rssi_worst"]:5d}' if t['rx'] else '    -      -'
        lines.append(f'{t["id"]:3d}  {t["rx"]:5d}  {t["lost"]:6d}  {pct(t["lost"], t["rx"] + t["lost"]):5.1f}  '
                     f'{totals.loss(t["id"]):6.1f}  {t["dup"]:3d}  {t["late"]:4d}  {t["max_gap_ms"]:5d} ms  '
                     f'{rssi}  {t["gaps"]:8d}  {t["outages"]:7d}')
    if not trackers:
        lines.append('  (no trackers heard)')
    return '\n'.join(lines)


CSV_FIELDS = ['time', 'report', 'kind', 'id', 'rx', 'lost', 'dup', 'late', 'max_gap_ms', 'rssi',
              'rssi_worst', 'gaps', 'outages', 'crc_ok', 'crc_bad', 'noise_median', 'noise_quiet',
              'noise_loud', 'noise_above_80_pct', 'mbps', 'crystal', 'counters', 'stored', 'uart_dropped']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=DIAG_PORT)
    ap.add_argument('--log', help='append every packet to this CSV file')
    ap.add_argument('--plain', action='store_true', help='print each report below the last instead of redrawing')
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', args.port))
    sock.settimeout(0.5)
    log_file = writer = None
    if args.log:
        new = not os.path.exists(args.log) or os.path.getsize(args.log) == 0
        log_file = open(args.log, 'a', newline='', encoding='utf-8')
        writer = csv.DictWriter(log_file, fieldnames=CSV_FIELDS, extrasaction='ignore')
        if new:
            writer.writeheader()
    if not args.plain and os.name == 'nt':
        os.system('')  # enable ANSI escapes in the Windows console
    print(f'Waiting for receiver diagnostics on UDP {args.port} '
          '("diag hub on" on the receiver console, hub streaming to this PC)...', flush=True)

    totals = Totals()
    report = None  # report number being collected
    receiver, trackers, source, last_rx = None, [], '?', 0.0

    def flush():
        nonlocal receiver, trackers
        if receiver is None and not trackers:
            return
        for t in trackers:
            totals.add(t)
        text = format_report(receiver, trackers, totals, source)
        if args.plain:
            print(text + '\n', flush=True)
        else:
            sys.stdout.write('\x1b[H\x1b[2J' + text + '\n')
            sys.stdout.flush()
        receiver, trackers = None, []

    try:
        while True:
            try:
                data, addr = sock.recvfrom(64)
            except socket.timeout:
                if time.time() - last_rx > 1.5:
                    flush()
                continue
            d = decode(data)
            if d is None:
                continue
            last_rx = time.time()
            source = addr[0]
            if report is not None and d['report'] != report:
                flush()
            report = d['report']
            if d['kind'] == 'receiver':
                receiver = d
            else:
                trackers.append(d)
            if writer:
                writer.writerow({'time': f'{last_rx:.3f}', **d})
                log_file.flush()
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        if log_file:
            log_file.close()


if __name__ == '__main__':
    main()
