#!/usr/bin/env python3
"""Turn an application .hex into a DFU package for Nordic's open bootloader (nRF52840 Dongle).

nrfutil-device programs the dongle only from such a package. This builds the same one nRF Connect
Programmer makes from a .hex: the application from its start address (0x1000 on the dongle),
padded to 4 bytes, and an unsigned init packet with fw_version 0xFFFFFFFF and is_debug set (no
version checks), hw_version 52, sd_req 0x00 (no SoftDevice) and the SHA-256 of the image, bytes
reversed as the bootloader compares them.

    python dfu_zip.py firmware.hex [firmware.zip]
    nrfutil device program --firmware firmware.zip --serial-number <dongle in its bootloader>

Standard library only.
"""
import hashlib
import json
import sys
import zipfile
from pathlib import Path


def read_hex(path):
    """Returns {address: byte} from an Intel HEX file."""
    data, base = {}, 0
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(':'):
            raise ValueError(f'line {n}: not an Intel HEX record')
        rec = bytes.fromhex(line[1:])
        if sum(rec) & 0xFF:
            raise ValueError(f'line {n}: bad checksum')
        count, addr, kind, body = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:4 + rec[0]]
        if len(body) != count:
            raise ValueError(f'line {n}: short record')
        if kind == 0:
            for i, b in enumerate(body):
                data[base + addr + i] = b
        elif kind == 1:
            break
        elif kind == 2:
            base = int.from_bytes(body, 'big') << 4
        elif kind == 4:
            base = int.from_bytes(body, 'big') << 16
        # 3 and 5 (start address) are not needed
    return data


def image(data):
    """The application as one contiguous image from its lowest address, padded to 4 bytes."""
    start, end = min(data), max(data) + 1
    if end - start > 1 << 20:
        raise ValueError(f'image spans 0x{start:X}-0x{end:X}: more than the application region')
    img = bytearray(b'\xff' * (end - start))
    for a, b in data.items():
        img[a - start] = b
    img += b'\xff' * (-len(img) % 4)
    return start, bytes(img)


# Protocol buffers, just enough for dfu-cc.proto
def varint(v):
    out = bytearray()
    while True:
        b, v = v & 0x7F, v >> 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def field_varint(num, v):
    return varint(num << 3) + varint(v)


def field_bytes(num, b):
    return varint(num << 3 | 2) + varint(len(b)) + b


def init_packet(img, hw_version=52, sd_req=(0,)):
    digest = hashlib.sha256(img).digest()[::-1]
    hash_msg = field_varint(1, 3) + field_bytes(2, digest)               # SHA256
    init = (field_varint(1, 0xFFFFFFFF)                                  # fw_version
            + field_varint(2, hw_version)
            + field_bytes(3, b''.join(varint(v) for v in sd_req))       # sd_req, packed
            + field_varint(4, 0)                                         # type APPLICATION
            + field_varint(5, 0) + field_varint(6, 0)                    # sd_size, bl_size
            + field_varint(7, len(img))                                  # app_size
            + field_bytes(8, hash_msg)
            + field_varint(9, 1))                                        # is_debug
    command = field_varint(1, 1) + field_bytes(2, init)                  # op_code INIT
    return field_bytes(1, command)                                       # Packet.command


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] in ('-h', '--help'):
        sys.exit(__doc__)
    src = Path(argv[0])
    dst = Path(argv[1]) if len(argv) > 1 else src.with_suffix('.zip')
    start, img = image(read_hex(src))
    if start != 0x1000:
        print(f'note: the application starts at 0x{start:X}; the dongle expects 0x1000', file=sys.stderr)
    manifest = {'manifest': {'application': {'bin_file': 'application.bin', 'dat_file': 'application.dat'}}}
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('application.bin', img)
        z.writestr('application.dat', init_packet(img))
        z.writestr('manifest.json', json.dumps(manifest))
    print(f'{dst}: {len(img)} bytes from 0x{start:X}, SHA-256 {hashlib.sha256(img).hexdigest()[:16]}...')


if __name__ == '__main__':
    main()
