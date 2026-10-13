#!/usr/bin/env python3
"""Flash a UF2 image to every tracker plugged in over USB, one after the other.

    python flash_usb.py ../dist/SlimeNRF_ProMicro_StackedSPI_Tracker.uf2
    python flash_usb.py <image> --only 0,3         # these tracker ids only

Finds the tracker consoles (1209:7692), asks each for "info", and flashes those whose board
(Target) fits the image: "dfu" on the console, copy to the UF2 drive that appears, wait for the
tracker to come back and show its new version. The receiver and other boards are left alone.
Pairing and calibration are kept (the settings area is not in the image). Windows only (drive
letters).

Needs pyserial: python -m pip install pyserial
"""
import argparse
import os
import re
import shutil
import string
import sys
import time

VID, PID = 0x1209, 0x7692  # trackers (the receiver is 7690)
BOARDS = {  # file name part -> CONFIG_BOARD_TARGET of the image
    'ProMicro_StackedSPI': 'promicro_uf2/nrf52840/stackedspi',
    'ProMicro_StackedI2C': 'promicro_uf2/nrf52840/stackedi2c',
    'XIAO_Sense': 'xiao_ble/nrf52840/sense',
}


def consoles():
    from serial.tools import list_ports
    return {p.serial_number: p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID}


def ask(port, command, wait=1.0):
    import serial
    s = serial.Serial(port, 115200, timeout=0.1)
    s.dtr = True
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write((command + '\n').encode())
    buf, end = b'', time.monotonic() + wait
    while time.monotonic() < end:
        buf += s.read(4096)
    s.close()
    return re.sub(r'\x1b\[[0-9;]*m', '', buf.decode(errors='replace'))


def field(text, name):
    m = re.search(rf'^{name}: (.*)$', text, re.M)
    return m[1].strip() if m else None


def version(text):
    m = re.search(r'^SlimeVR-Tracker-nRF (\S+)', text, re.M)
    return m[1] if m else '?'


def uf2_drives():
    return {f'{d}:\\' for d in string.ascii_uppercase if os.path.exists(f'{d}:\\INFO_UF2.TXT')}


def wait_for(check, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        found = check()
        if found:
            return found
        time.sleep(0.3)
    return None


def flash(serial_number, port, image):
    before = uf2_drives()
    try:
        ask(port, 'dfu', wait=0.3)
    except Exception:
        pass  # the port goes away as it resets
    drive = wait_for(lambda: next(iter(uf2_drives() - before), None), 20)
    if not drive:
        return 'no UF2 drive appeared'
    try:
        shutil.copyfile(image, os.path.join(drive, os.path.basename(image)))
    except OSError:
        pass  # the drive disappears when the bootloader resets, sometimes before the copy returns
    port = wait_for(lambda: consoles().get(serial_number), 30)
    if not port:
        return 'did not come back on USB'
    time.sleep(1.5)
    return ask(port, 'info')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image', help='a tracker .uf2 from dist/')
    ap.add_argument('--board', help='CONFIG_BOARD_TARGET of the image (from the file name otherwise)')
    ap.add_argument('--only', help='comma-separated tracker ids')
    args = ap.parse_args(argv)
    try:
        import serial  # noqa: F401
    except ImportError:
        sys.exit('Needs pyserial: python -m pip install pyserial')
    board = args.board or next((b for k, b in BOARDS.items() if k in args.image), None)
    if not board:
        sys.exit('Cannot tell the board from the file name; give --board')
    only = {int(i) for i in args.only.split(',')} if args.only else None
    image = os.path.abspath(args.image)

    found = []
    for sn, port in sorted(consoles().items(), key=lambda x: x[1]):
        info = ask(port, 'info')
        target, tid = field(info, 'Target'), field(info, 'Tracker ID')
        print(f'{port:6} {target or "?":34} id {tid or "-":4} {version(info)}')
        if target == board and (only is None or (tid or '').isdigit() and int(tid) in only):
            found.append((sn, port, tid, version(info)))
    if not found:
        sys.exit(f'No {board} tracker on USB')

    print(f'\nFlashing {os.path.basename(image)} to {len(found)} tracker(s)')
    failed = 0
    for sn, port, tid, old in found:
        print(f'  id {tid} ({port}, {old}) ... ', end='', flush=True)
        info = flash(sn, port, image)
        if '\n' not in info:
            print(info)
            failed += 1
            continue
        print(f'{version(info)}, id {field(info, "Tracker ID")}, {field(info, "Image") or "no MCUboot"}')
    if failed:
        sys.exit(f'{failed} failed')


if __name__ == '__main__':
    main()
