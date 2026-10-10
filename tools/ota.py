#!/usr/bin/env python3
"""Update a tracker's firmware over the radio, through the nRF52840 Dongle (docs/OTA.md).

Needs the OTA builds: the receiver from this repository (make receiver) and trackers running an
MCUboot image (make tracker-ota, flashed once over USB as _OTA.uf2). The image to send is the
_OTA_app.bin of make tracker-ota.

    python ota.py ../dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker 5
    python ota.py <image> --tracker 5 --port COM29       # receiver port, found by itself otherwise

The receiver offers the image to that tracker at once; the tracker pauses tracking and fetches it
over the radio while this streams it into the receiver's 16 KB window (the receiver says in every
answer how far the tracker is, and this never runs further ahead). The tracker checks the image,
reboots, MCUboot swaps it in, and it confirms itself once the receiver hears it (a failed image is
rolled back on its next reboot). The new version then shows in slime_link.py.

Needs pyserial: python -m pip install pyserial
"""
import argparse
import re
import sys
import time
import zlib

RECEIVER_VID, RECEIVER_PID = 0x1209, 0x7690
CHUNK = 56      # bytes per console line: "ota d <offset> <112 hex digits>" stays under 128 characters
WINDOW = 16384  # the receiver's window (Receiver/src/ota.c)
IN_FLIGHT = 6   # console lines sent before their answers (the receiver's USB buffer holds 1 KB)
BOARDS = {  # file name part -> CONFIG_BOARD_TARGET of the image
    'ProMicro_StackedSPI': 'promicro_uf2/nrf52840/stackedspi',
    'XIAO_Sense': 'xiao_ble/nrf52840/sense',
}


class Receiver:
    def __init__(self, port):
        import serial
        self.ser = serial.Serial(port, 115200, timeout=0)
        self.ser.dtr = True
        self.buf = b''
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, line):
        self.ser.write((line + '\n').encode())

    def messages(self):
        """'ota:' messages and console errors received so far (without waiting)."""
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


def find_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == RECEIVER_VID and p.pid == RECEIVER_PID:
            return p.device
    sys.exit('No receiver found on USB (1209:7690); give --port')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='the _OTA_app.bin of make tracker-ota')
    ap.add_argument('--tracker', type=int, required=True, help='tracker id to update (the receiver\'s "list" order)')
    ap.add_argument('--board', help='CONFIG_BOARD_TARGET of the image (from the file name otherwise)')
    ap.add_argument('--port', help='receiver serial port')
    ap.add_argument('--timeout', type=float, default=300, help='seconds for the whole update')
    args = ap.parse_args(argv)
    try:
        import serial  # noqa: F401
    except ImportError:
        sys.exit('Needs pyserial: python -m pip install pyserial')

    image = open(args.image, 'rb').read()
    board = args.board or next((b for k, b in BOARDS.items() if k in args.image), None)
    if not board:
        sys.exit('Cannot tell the board from the file name; give --board')
    if image[:4] != b'\x3d\xb8\xf3\x96':
        sys.exit('Not an MCUboot image (use the _OTA_app.bin of make tracker-ota)')
    size, crc = len(image), zlib.crc32(image)
    rx = Receiver(args.port or find_port())
    print(f'{args.image}: {size} bytes, crc {crc:08x}, board {board}')

    rx.send(f'ota begin {size} {crc:08x} {zlib.crc32(board.encode()):08x} {args.tracker}')
    rx.wait_for('begin', 5)
    print(f'Offered to tracker {args.tracker} (on and paired; it pauses tracking while it receives)')

    sent = asked = 0  # bytes sent in console lines; offset the tracker asked for last
    in_flight = 0
    start = time.monotonic()
    accepted = None
    while time.monotonic() - start < args.timeout:
        for msg in rx.messages():
            m = re.match(r'(ok|full) (\d+) (\d+)', msg)
            if m:
                in_flight = max(0, in_flight - 1)
                asked = max(asked, int(m[3]))
                continue
            m = re.match(r'error: data at \d+, expected (\d+)', msg)
            if m:  # a line was lost or refused: go on from where the receiver is
                sent, in_flight = int(m[1]), 0
                continue
            if msg.startswith('error'):
                sys.exit(f'receiver: {msg}')
            print('   ', msg)
            if 'accepted' in msg:
                accepted = time.monotonic()
            if 'reboots into it' in msg:
                total = time.monotonic() - (accepted or start)
                print(f'Done in {total:.0f} s: the tracker swaps the image in and confirms it once the receiver hears it.')
                print('Check its version with slime_link.py (firmware column).')
                return
            if 'failed' in msg or 'stopped asking' in msg:
                sys.exit(1)
        while in_flight < IN_FLIGHT and sent < size and sent + CHUNK <= asked + WINDOW:
            part = image[sent:sent + CHUNK]
            rx.send(f'ota d {sent} {part.hex()}')
            sent += len(part)
            in_flight += 1
        time.sleep(0.001)
    rx.send('ota stop')
    sys.exit(f'No result within {args.timeout:.0f} s; offer stopped (tracker at {asked} of {size} bytes)')


if __name__ == '__main__':
    main()
