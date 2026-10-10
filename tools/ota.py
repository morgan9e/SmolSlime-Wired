#!/usr/bin/env python3
"""Update a tracker's firmware over the radio, through the nRF52840 Dongle (docs/OTA.md).

Needs the OTA builds: the receiver from this repository (make receiver) and trackers running an
MCUboot image (make tracker-ota, flashed once over USB as _OTA.uf2). The image to send is the
_OTA_app.bin of make tracker-ota.

    python ota.py ../dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker 5
    python ota.py <image> --tracker 5 --port COM29       # receiver console, found by itself otherwise

The receiver offers the image to that tracker at once ("ota begin" on its console); the tracker
pauses tracking and fetches it over the radio while this streams it into the receiver's 16 KB
window as HID OUT reports, reading from "ota status" how far the tracker is. The tracker checks
the image, reboots, MCUboot swaps it in, and it confirms itself once the receiver hears it (a
failed image is rolled back on its next reboot). The new version then shows in slime_link.py.

Needs pyserial and hidapi: python -m pip install pyserial hidapi
"""
import argparse
import re
import sys
import time
import zlib

RECEIVER_VID, RECEIVER_PID = 0x1209, 0x7690
WINDOW = 16384        # the receiver's window (Receiver/src/ota.c)
HID_DATA = 0xF5       # OTA_HID_DATA (Receiver/src/ota.h): [type][offset u24][length][data]
HID_PAYLOAD = 59      # 64-byte report minus the header
AHEAD = 4096          # bytes sent before the receiver confirms them in its status
POLL_S = 0.05
BOARDS = {  # file name part -> CONFIG_BOARD_TARGET of the image
    'ProMicro_StackedSPI': 'promicro_uf2/nrf52840/stackedspi',
    'XIAO_Sense': 'xiao_ble/nrf52840/sense',
}
STATUS = re.compile(r'(offering|sending), tracker \d+, \d+ bytes, received (\d+), asked (\d+)')


class Console:
    def __init__(self, port):
        import serial
        self.ser = serial.Serial(port, 115200, timeout=0, write_timeout=2)
        self.ser.dtr = True
        self.buf = b''
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, line):
        self.ser.write((line + '\n').encode())

    def messages(self):
        """'ota:' messages received so far, without waiting."""
        self.buf += self.ser.read(self.ser.in_waiting or 0)
        *done, self.buf = self.buf.split(b'\n')
        for raw in done:
            text = re.sub(r'\x1b\[[0-9;]*m', '', raw.decode(errors='replace')).strip()
            if text.startswith('Too many parameters') or text.startswith('Invalid'):
                raise RuntimeError(f'receiver console: {text}')
            i = text.find('ota: ')
            if i >= 0:
                yield text[i + 5:]

    def wait_for(self, prefix, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for msg in self.messages():
                if msg.startswith('error'):
                    raise RuntimeError(f'receiver: {msg}')
                if msg.startswith(prefix):
                    return msg
            time.sleep(0.005)
        raise TimeoutError(f'no "ota: {prefix}" from the receiver')


def find_receiver(port):
    from serial.tools import list_ports
    import hid
    if not port:
        port = next((p.device for p in list_ports.comports()
                     if p.vid == RECEIVER_VID and p.pid == RECEIVER_PID), None)
    hid_paths = [d['path'] for d in hid.enumerate(RECEIVER_VID, RECEIVER_PID)]
    if not port or not hid_paths:
        sys.exit('No receiver found on USB (1209:7690); give --port')
    if len(hid_paths) > 1:
        sys.exit('More than one receiver on USB: unplug the others')
    dev = hid.device()
    dev.open_path(hid_paths[0])
    return port, dev


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='the _OTA_app.bin of make tracker-ota')
    ap.add_argument('--tracker', type=int, required=True, help='tracker id to update (the receiver\'s "list" order)')
    ap.add_argument('--board', help='CONFIG_BOARD_TARGET of the image (from the file name otherwise)')
    ap.add_argument('--port', help='receiver console serial port')
    ap.add_argument('--timeout', type=float, default=300, help='seconds for the whole update')
    args = ap.parse_args(argv)
    try:
        import serial  # noqa: F401
        import hid  # noqa: F401
    except ImportError:
        sys.exit('Needs pyserial and hidapi: python -m pip install pyserial hidapi')

    image = open(args.image, 'rb').read()
    board = args.board or next((b for k, b in BOARDS.items() if k in args.image), None)
    if not board:
        sys.exit('Cannot tell the board from the file name; give --board')
    if image[:4] != b'\x3d\xb8\xf3\x96':
        sys.exit('Not an MCUboot image (use the _OTA_app.bin of make tracker-ota)')
    size, crc = len(image), zlib.crc32(image)
    port, dev = find_receiver(args.port)
    con = Console(port)
    print(f'{args.image}: {size} bytes, crc {crc:08x}, board {board}')

    con.send(f'ota begin {size} {crc:08x} {zlib.crc32(board.encode()):08x} {args.tracker}')
    con.wait_for('begin', 5)
    print(f'Offered to tracker {args.tracker} (on and paired; it pauses tracking while it receives)')

    sent = received = asked = 0
    stuck_polls = 0
    start = time.monotonic()
    accepted = None
    next_poll = 0
    try:
        while time.monotonic() - start < args.timeout:
            now = time.monotonic()
            if now >= next_poll:
                con.send('ota status')
                next_poll = now + POLL_S
            for msg in con.messages():
                m = STATUS.match(msg)
                if m:
                    new_received, asked = int(m[2]), max(asked, int(m[3]))
                    # a report the receiver dropped: go on from where it is
                    stuck_polls = stuck_polls + 1 if new_received == received and sent > received else 0
                    received = new_received
                    if stuck_polls >= 3:
                        sent, stuck_polls = received, 0
                    continue
                if msg.startswith('error'):
                    sys.exit(f'receiver: {msg}')
                if msg.startswith('idle'):
                    continue
                print('   ', msg)
                if 'accepted' in msg:
                    accepted = time.monotonic()
                if 'reboots into it' in msg:
                    print(f'Done in {time.monotonic() - (accepted or start):.0f} s: the tracker swaps the image in '
                          'and confirms it once the receiver hears it.')
                    print('Check its version with slime_link.py (firmware column).')
                    return
                if 'failed' in msg or 'stopped asking' in msg:
                    sys.exit(1)
            while sent < size and sent - received < AHEAD and sent + HID_PAYLOAD <= asked + WINDOW:
                part = image[sent:sent + HID_PAYLOAD]
                report = bytes([HID_DATA, sent & 0xFF, (sent >> 8) & 0xFF, sent >> 16, len(part)]) + part
                dev.write(b'\x00' + report.ljust(64, b'\x00'))  # report id 0 (the receiver has none)
                sent += len(part)
            time.sleep(0.002)
        con.send('ota stop')
        sys.exit(f'No result within {args.timeout:.0f} s; offer stopped (tracker at {asked} of {size} bytes)')
    finally:
        dev.close()


if __name__ == '__main__':
    main()
