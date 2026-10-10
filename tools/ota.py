#!/usr/bin/env python3
"""Update a tracker's firmware over the radio, through the nRF52840 Dongle (docs/OTA.md).

Needs the OTA builds: the receiver from this repository (make receiver) and trackers running an
MCUboot image (make tracker-ota, flashed once over USB as _OTA.uf2). The image to send is the
_OTA_app.bin of make tracker-ota.

    python ota.py ../dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker 5
    python ota.py <image> --tracker 5 --port COM29       # receiver port, found by itself otherwise
    python ota.py <image> --upload-only                  # store it on the receiver, send nothing

1. Uploads the image to the receiver over its console and checks the CRC there (skipped when the
   receiver already holds the same image).
2. "ota send <id>": the receiver offers it to that tracker, which pauses tracking, fetches it
   over the radio, checks it and reboots; MCUboot swaps it in and it confirms itself once the
   receiver hears it (a failed image is rolled back on its next reboot).
3. Follows the receiver's progress messages. The new version then shows in slime_link.py.

Needs pyserial: python -m pip install pyserial
"""
import argparse
import sys
import time
import zlib

RECEIVER_VID, RECEIVER_PID = 0x1209, 0x7690
CHUNK = 48  # bytes per console line: "ota d <offset> <96 hex digits>" stays under 128 characters
BOARDS = {  # file name part -> CONFIG_BOARD_TARGET of the image
    'ProMicro_StackedSPI': 'promicro_uf2/nrf52840/stackedspi',
    'XIAO_Sense': 'xiao_ble/nrf52840/sense',
}


def board_crc(board):
    return zlib.crc32(board.encode())


class Receiver:
    def __init__(self, port):
        import serial
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.ser.dtr = True
        self.buf = b''
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def lines(self):
        """Lines received so far."""
        self.buf += self.ser.read(4096)
        *done, self.buf = self.buf.split(b'\n')
        for raw in done:
            yield raw.decode(errors='replace').strip().replace('\x1b', '')

    def command(self, line, expect, timeout=5.0, show=False):
        """Send a console line and wait for an 'ota:' answer starting with one of expect."""
        self.ser.write((line + '\n').encode())
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for text in self.lines():
                if show and text:
                    print('   ', text)
                i = text.find('ota: ')
                if i < 0:
                    continue
                answer = text[i + 5:]
                if answer.startswith('error'):
                    raise RuntimeError(f'receiver: {answer}')
                if answer.startswith(expect):
                    return answer
        raise TimeoutError(f'no answer to "{line[:40]}"')


def find_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == RECEIVER_VID and p.pid == RECEIVER_PID:
            return p.device
    sys.exit('No receiver found on USB (1209:7690); give --port')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='the _OTA_app.bin of make tracker-ota')
    ap.add_argument('--tracker', type=int, help='tracker id to update (the receiver\'s "list" order)')
    ap.add_argument('--board', help='CONFIG_BOARD_TARGET of the image (from the file name otherwise)')
    ap.add_argument('--port', help='receiver serial port')
    ap.add_argument('--upload-only', action='store_true', help='store the image on the receiver only')
    ap.add_argument('--timeout', type=float, default=180, help='seconds to wait for the tracker')
    args = ap.parse_args(argv)
    if args.tracker is None and not args.upload_only:
        ap.error('give --tracker <id> (or --upload-only)')
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
    crc = zlib.crc32(image)
    rx = Receiver(args.port or find_port())
    print(f'{args.image}: {len(image)} bytes, crc {crc:08x}, board {board}')

    status = rx.command('ota status', '')
    if f'stored, {len(image)} of {len(image)} bytes, crc {crc:08x}' in status:
        print('The receiver already holds this image')
    else:
        print('Uploading to the receiver ...', end='', flush=True)
        t0 = time.monotonic()
        rx.command(f'ota begin {len(image)} {crc:08x} {board_crc(board):08x}', 'begin', timeout=20)
        for offset in range(0, len(image), CHUNK):
            part = image[offset:offset + CHUNK]
            for attempt in range(3):
                try:
                    rx.command(f'ota d {offset} {part.hex()}', 'ok', timeout=2)
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
            if offset % (CHUNK * 500) == 0:
                print('.', end='', flush=True)
        rx.command('ota end', 'stored', timeout=10)
        print(f' {time.monotonic() - t0:.0f} s')
    if args.upload_only:
        return

    print(f'Offering it to tracker {args.tracker} (it must be on and paired; it pauses tracking while it receives)')
    rx.command(f'ota send {args.tracker}', 'offering')
    end = time.monotonic() + args.timeout
    while time.monotonic() < end:
        for text in rx.lines():
            i = text.find('ota: ')
            if i < 0:
                continue
            msg = text[i + 5:]
            print('   ', msg)
            if 'reboots into it' in msg:
                print('Done: the tracker swaps the image in and confirms it once the receiver hears it.')
                print('Check its version with slime_link.py (firmware column).')
                return
            if 'failed' in msg or 'stopped asking' in msg:
                sys.exit(1)
        time.sleep(0.05)
    rx.command('ota stop', '')
    sys.exit(f'No result within {args.timeout:.0f} s; offer stopped')


if __name__ == '__main__':
    main()
