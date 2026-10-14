#!/usr/bin/env python3
"""Slime View: a small stand-in for SlimeVR Server to check the hub's trackers.

By default it picks one source by itself: an nRF receiver on USB when one is plugged in, else
the ESP32 hub over Wi-Fi (the hub finds this program on UDP 6969 and relays every tracker to it).
It checks every 2 s and switches when a receiver is plugged in or unplugged. Over Wi-Fi, quit
SlimeVR Server first so it does not compete for tracker data. Shows per tracker: 3D orientation, last
real data, status, battery, signal, temperature and packet rates.
With an nRF receiver on USB: Pair (status bar) puts it into pairing mode until clicked again,
and Remove on a tracker card unpairs that tracker (the others keep their ids). Both use the
receiver's console (needs pyserial and a receiver firmware with `unpair`).
For a simple skeleton, select a body part on each tracker card: chest, hips, thighs and
ankles, optionally feet, upper arms and lower arms. Unassigned parts move with their parent.
The resets follow SlimeVR Server:
  Full reset   stand upright, legs straight, arms at your sides (3 s).
  Mount reset  ski pose: feet parallel and flat, knees bent, hips back, torso forward
               without turning; upper arms back, forearms forward (5 s).
  Feet reset   on tiptoes, both feet pointing forward (3 s); the ski pose keeps feet flat.
Repeat them after remounting or tracker restarts.
Assignments, standing references and mounting corrections are saved automatically to
slime_view_config.json beside this script (override with --config PATH).
Body parts are also taken from SlimeVR Server's own assignments (vrconfig.yml or the
settings profile in its config folder) while "Follow SlimeVR" is ticked;
picking a part by hand unticks it. Use --slimevr-config PATH or --no-slimevr to change this.
Calibration accepts the last valid rotation from inactive trackers; those samples
must still represent the requested pose. The status reports when older data is used.
The preview uses fixed proportions and a stationary hip position; the head follows
the chest.

    python slime_view.py              USB if a receiver is plugged in, else Wi-Fi
    python slime_view.py --usb        USB receivers only
    python slime_view.py --wifi [--port 6969]   Wi-Fi only

Needs PySide6: python -m pip install PySide6. USB also needs: python -m pip install hidapi
Plug the nRF receiver into the PC for USB; no Wi-Fi or ESP32 hub is needed.
On Windows, allow Python through the firewall for private networks when asked.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import struct
import threading
import tempfile
import time
import sys

try:
    from PySide6.QtCore import QPointF, Qt, QTimer
    from PySide6.QtGui import QColor, QPainter, QPalette, QPen, QPolygonF
    from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFrame, QGridLayout, QGroupBox,
                                   QHBoxLayout, QLabel, QMainWindow, QMessageBox, QPushButton, QScrollArea,
                                   QStyle, QVBoxLayout, QWidget)
except ImportError as e:
    raise SystemExit('Slime View needs PySide6. Run: python -m pip install PySide6') from e

HANDSHAKE_REPLY = b"\x03Hey OVR =D 5"
PORT_BASE = 6980  # the hub's local port for tracker id 0 (CONFIG_HUB_LOCAL_PORT_BASE)

# Hub -> server packet types (SlimeVR UDP protocol, big-endian)
PKT_HEARTBEAT, PKT_HANDSHAKE, PKT_BATTERY, PKT_SENSOR_INFO = 0, 3, 12, 15
PKT_ROTATION, PKT_PING, PKT_SIGNAL, PKT_TEMPERATURE, PKT_ROT_ACCEL = 17, 10, 19, 20, 23
SRV_HEARTBEAT, SRV_SENSOR_INFO = 1, 15

STALE_S = 2.0  # no real data for this long: tracker shown as not seen
STATUS = {1: "OK", 2: "Error"}


class Tracker:
    def __init__(self, addr):
        self.addr = addr  # (ip, port) of the hub socket for this tracker
        self.id = addr[1] - PORT_BASE if 0 <= addr[1] - PORT_BASE < 64 else None
        self.source = 'wifi'  # or 'usb'
        self.receiver = None  # USB: the receiver's USB serial number (its console has the same)
        self.mac = None
        self.fw = None
        self.status = None
        self.q = None  # w, x, y, z
        self.accel = None
        self.batt_pct = self.batt_v = self.rssi = self.temp = None
        self.last_real = None  # time of the last rotation+accel packet
        self.last_any = time.time()
        self.real = self.held = 0  # counts in the current second
        self.real_rate = self.held_rate = 0


class Server:
    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.sock.settimeout(0.2)
        self.description = f"Wi-Fi, UDP {self.sock.getsockname()[1]}"
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.trackers = {}
        self.hb_num = 0
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        self.sock.close()

    def send(self, addr, data):
        try:
            self.sock.sendto(data, addr)
        except OSError as e:
            self.error = str(e)

    def run(self):
        next_tick = time.time()
        while not self.stop.is_set():
            try:
                data, addr = self.sock.recvfrom(2048)
                self.handle(data, addr)
            except socket.timeout:
                pass
            except OSError as e:  # e.g. ICMP port unreachable reported on Windows
                self.error = str(e)
            now = time.time()
            if now >= next_tick:  # once a second: heartbeats and packet rates
                next_tick = now + 1.0
                with self.lock:
                    for t in self.trackers.values():
                        t.real_rate, t.held_rate, t.real, t.held = t.real, t.held, 0, 0
                    addrs = list(self.trackers)
                for a in addrs:
                    self.hb_num += 1
                    self.send(a, struct.pack(">IQ", SRV_HEARTBEAT, self.hb_num))

    def handle(self, b, addr):
        if len(b) < 4:
            return
        kind = struct.unpack_from(">I", b)[0]
        if kind == PKT_HANDSHAKE:
            self.send(addr, HANDSHAKE_REPLY)
        with self.lock:
            t = self.trackers.get(addr)
            if t is None:
                t = self.trackers[addr] = Tracker(addr)
            t.last_any = time.time()
            p = b[12:]  # after type and packet number
            try:
                self.decode(t, kind, p)
            except (struct.error, IndexError):
                pass  # short packet

    def decode(self, t, kind, p):
        if kind == PKT_HANDSHAKE:
            n = p[28]
            t.fw = p[29:29 + n].decode("ascii", "replace")
            t.mac = p[29 + n:35 + n].hex().upper()
        elif kind == PKT_SENSOR_INFO:
            t.status = p[1]
            self.send(t.addr, struct.pack(">IBB", SRV_SENSOR_INFO, p[0], p[1]))  # ack
        elif kind == PKT_ROT_ACCEL:  # real sample: Q15 quaternion x y z w, Q7 accel
            x, y, z, w, ax, ay, az = struct.unpack_from(">7h", p, 1)
            t.q = (w / 32768, x / 32768, y / 32768, z / 32768)
            t.accel = (ax / 128, ay / 128, az / 128)
            t.last_real = time.time()
            t.real += 1
        elif kind == PKT_ROTATION:  # float quaternion x y z w; held pose during radio gaps
            x, y, z, w = struct.unpack_from(">4f", p, 2)
            t.q = (w, x, y, z)
            t.held += 1
        elif kind == PKT_BATTERY:
            t.batt_v, pct = struct.unpack_from(">2f", p)
            t.batt_pct = pct * 100
        elif kind == PKT_SIGNAL:
            t.rssi = struct.unpack_from(">b", p, 1)[0]
        elif kind == PKT_TEMPERATURE:
            t.temp = struct.unpack_from(">f", p, 1)[0]
        elif kind == PKT_PING:
            pass

    def snapshot(self):
        with self.lock:
            return [vars(t).copy() for t in self.trackers.values()]

    def forget_stale(self):
        now = time.time()
        with self.lock:
            for a in [a for a, t in self.trackers.items() if now - t.last_any > STALE_S]:
                del self.trackers[a]

    def forget(self, addr):
        with self.lock:
            self.trackers.pop(addr, None)

    def receivers(self):
        return []  # the Wi-Fi hub has no pairing here


# The receiver's console (its USB CDC interface, same USB serial number as its HID interface):
# pairing and unpairing. The ESP32-S3 BLE receiver (BleReceiver/, serial 5333...) has the same USB
# id but another console.
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def pairable(receiver_serial):
    return bool(receiver_serial) and not receiver_serial.startswith('5333')


def receiver_command(receiver_serial, command, replies, timeout=2.0, serial_module=None):
    """Runs one console command on the receiver with this USB serial number. Returns the first
    reply line starting with one of `replies`, 'Unknown command' (older firmware), or None."""
    if serial_module is None:
        import serial as serial_module
        from serial.tools import list_ports
    else:
        list_ports = serial_module.list_ports
    port = next((p.device for p in list_ports.comports()
                 if (p.vid, p.pid) == (0x1209, 0x7690) and p.serial_number == receiver_serial), None)
    if port is None:
        raise OSError('its console port was not found')
    s = serial_module.Serial(port, 115200, timeout=0.1)
    try:
        s.dtr = True
        s.read(65536)  # whatever was waiting
        s.write((command + '\r\n').encode('ascii'))
        buf, end = b'', time.monotonic() + timeout
        while time.monotonic() < end:
            buf += s.read(4096)
            *lines, buf = buf.split(b'\n')
            for raw in lines:
                line = ANSI.sub('', raw.decode('utf-8', 'replace')).strip()
                if line.startswith(tuple(replies)) or line == 'Unknown command':
                    return line
        return None
    finally:
        s.close()


def decode_hid_packet(t, p):
    """Decode receiver firmware's 16-byte record (see src/Hub/main/decoder.c)."""
    kind = p[0]
    if kind == 255:
        # little-endian in the packet; reversed it matches the hub's Wi-Fi handshake and the
        # receiver's "list", so saved assignments work over both
        t.mac = p[2:8][::-1].hex().upper()
        return  # receiver address announcements are not evidence of a live tracker
    t.last_any = time.time()
    if kind in (0, 2):
        t.batt_pct = None if p[2] == 128 else min(100, p[2] & 127)
        t.batt_v = (p[3] + 245) / 100
        t.temp = p[4] / 2 - 39 if p[4] else None
    if kind == 0:
        t.fw = '.'.join(str(v) for v in p[12:15])
    if kind == 3:
        t.status = 2 if p[2] == 3 else 1
    if kind not in (1, 4):
        t.rssi = -p[15]
    if kind not in (1, 2, 4, 7):
        return
    if kind in (1, 4):
        x, y, z, w = (v / 32768 for v in struct.unpack_from('<4h', p, 2))
    else:
        bits = struct.unpack_from('<I', p, 5)[0]
        v = ((bits & 1023) / 512 - 1,
             ((bits >> 10) & 2047) / 1024 - 1,
             ((bits >> 21) & 2047) / 1024 - 1)
        d = sum(a * a for a in v)
        inv = 1 / math.sqrt(d + 1e-6)
        angle = math.pi / 2 * d * inv
        x, y, z = (math.sin(angle) * inv * a for a in v)
        w = math.cos(angle)
    norm = w*w + x*x + y*y + z*z
    if not 0.25 < norm < 4:
        return
    t.q = tuple(a / math.sqrt(norm) for a in (w, x, y, z))
    if kind != 4:
        offset = 10 if kind == 1 else 9
        t.accel = tuple(a / 128 for a in struct.unpack_from('<3h', p, offset))
    t.last_real = time.time()
    t.real += 1
    if t.status is None:
        t.status = 1


class USBServer:
    """Read all matching nRF receivers. Only the worker owns device handles."""
    snapshot = Server.snapshot
    forget_stale = Server.forget_stale
    forget = Server.forget

    def __init__(self, hid_module=None):
        if hid_module is None:
            try:
                import hid as hid_module
            except ImportError as e:
                raise RuntimeError('USB mode needs hidapi. Run: python -m pip install hidapi') from e
        if not hasattr(hid_module, 'device'):
            raise RuntimeError('Install the hidapi package (not the different package named hid).')
        self.hid = hid_module
        self.lock = threading.Lock()
        self.trackers = {}
        self.serials = {}  # receiver (HID path) -> its USB serial number, while open
        self.error = None
        self.description = 'USB, no receiver'
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)

    @staticmethod
    def receiver_key(path):
        return path.hex() if isinstance(path, bytes) else str(path)

    def handle(self, data, receiver):
        b = bytes(data)
        # The firmware has no report ID; tolerate a platform's leading zero ID.
        if len(b) == 65 and b[0] == 0:
            b = b[1:]
        if len(b) != 64:
            return
        with self.lock:
            for offset in range(0, 64, 16):
                p = b[offset:offset + 16]
                if not any(p) or p[0] not in (*range(8), 255):
                    continue
                key = (receiver, p[1])
                t = self.trackers.get(key)
                if t is None:
                    t = self.trackers[key] = Tracker(key)
                    t.id = p[1]
                    t.source = 'usb'
                    t.last_any = 0
                t.receiver = self.serials.get(receiver)
                decode_hid_packet(t, p)

    def receivers(self):
        """USB serial numbers of the open receivers, in the order their trackers are listed."""
        with self.lock:
            return [self.serials[k] for k in sorted(self.serials) if self.serials[k]]

    def run(self):
        devices = {}
        next_scan = next_tick = 0
        try:
            while not self.stop.is_set():
                now = time.monotonic()
                if now >= next_scan:
                    next_scan = now + 1
                    errors = []
                    try:
                        found = {d['path']: d for d in self.hid.enumerate(0x1209, 0x7690)}
                        for path in list(devices):
                            if path not in found:
                                devices.pop(path).close()
                                with self.lock:
                                    self.serials.pop(self.receiver_key(path), None)
                        for path in found:
                            if path in devices:
                                continue
                            dev = self.hid.device()
                            try:
                                dev.open_path(path)
                                dev.set_nonblocking(1)
                                devices[path] = dev
                                with self.lock:
                                    self.serials[self.receiver_key(path)] = found[path].get('serial_number')
                            except (OSError, ValueError) as e:
                                dev.close()
                                errors.append(str(e))
                    except (OSError, ValueError) as e:
                        errors.append(str(e))
                    self.error = '; '.join(errors) or None
                for path, dev in list(devices.items()):
                    try:
                        # Drain bursts, but let scanning, rate updates and shutdown run.
                        for _ in range(64):
                            data = dev.read(65)
                            if not data:
                                break
                            self.handle(data, self.receiver_key(path))
                    except (OSError, ValueError) as e:
                        self.error = str(e)
                        devices.pop(path).close()
                self.description = (f'USB, {len(devices)} receiver' + ('s' if len(devices) > 1 else '')
                                    if devices else 'USB, no receiver')
                if now >= next_tick:
                    next_tick = now + 1
                    with self.lock:
                        for t in self.trackers.values():
                            t.real_rate, t.held_rate, t.real, t.held = t.real, t.held, 0, 0
                self.stop.wait(0.002)
        finally:
            for dev in devices.values():
                dev.close()


class AutoServer:
    """One source at a time: an nRF receiver on USB when one is plugged in, else the Wi-Fi hub.
    Checks every 2 s and switches when a receiver is plugged in or unplugged; only the active
    source is open, so in USB mode UDP 6969 stays free."""

    def __init__(self, port):
        self.port = port
        try:
            import hid
            self.hid = hid if hasattr(hid, 'device') else None
        except ImportError:
            self.hid = None
        self.lock = threading.Lock()
        self.active = self.kind = None
        self.note = None if self.hid else 'USB off: python -m pip install hidapi'
        self.switch()
        if self.active is None:
            raise RuntimeError(self.note)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.watch, daemon=True)
        self.thread.start()

    def usb_present(self):
        try:
            return bool(self.hid and self.hid.enumerate(0x1209, 0x7690))
        except (OSError, ValueError):
            return False

    def switch(self):
        want = 'usb' if self.usb_present() else 'wifi'
        with self.lock:
            if want == self.kind:
                return
            try:
                new = USBServer(self.hid) if want == 'usb' else Server(self.port)
            except (OSError, RuntimeError) as e:
                self.note = (f'Wi-Fi: UDP {self.port} busy ({getattr(e, "strerror", None) or e}); '
                             'SlimeVR Server running?') if want == 'wifi' else f'USB: {e}'
                return  # keep the current source; try again on the next check
            old, self.active, self.kind = self.active, new, want
            self.note = None if self.hid else 'USB off: python -m pip install hidapi'
        if old is not None:
            old.close()

    def watch(self):
        while not self.stop.wait(2):
            self.switch()

    @property
    def description(self):
        a = self.active
        return a.description + (f'   ·   {self.note}' if self.note else '')

    @property
    def error(self):
        return self.active.error

    def snapshot(self):
        return self.active.snapshot()

    def forget_stale(self):
        self.active.forget_stale()

    def forget(self, addr):
        self.active.forget(addr)

    def receivers(self):
        return self.active.receivers()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)
        with self.lock:
            self.active.close()


# ---------------------------------------------------------------- 3D drawing

def q_rotate(q, v):
    w, x, y, z = q
    vx, vy, vz = v
    # v' = q v q*
    tx = 2 * (y * vz - z * vy)
    ty = 2 * (z * vx - x * vz)
    tz = 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty,
            vy + w * ty + z * tx - x * tz,
            vz + w * tz + x * ty - y * tx)


def euler_deg(q):
    w, x, y, z = q
    roll = math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y))
    pitch = math.asin(max(-1.0, min(1.0, 2 * (w * y - z * x))))
    yaw = math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))
    return tuple(math.degrees(a) for a in (roll, pitch, yaw))


VIEW_YAW, VIEW_PITCH = math.radians(-35), math.radians(25)
BOX = (1.0, 0.7, 0.3)  # half sizes x, y, z: a flat tracker
FACES = [  # vertex indices, colour; +Z face highlighted
    ((4, 5, 7, 6), "#4f8cff"), ((0, 2, 3, 1), "#2b3a55"),
    ((1, 3, 7, 5), "#c0504d"), ((0, 4, 6, 2), "#6b2b2a"),
    ((2, 6, 7, 3), "#4e9a4e"), ((0, 1, 5, 4), "#2a542a"),
]


def to_screen(v, cx, cy, s):
    x, y, z = v
    # world (z up) -> camera: yaw about z, then tilt about x
    cyw, syw = math.cos(VIEW_YAW), math.sin(VIEW_YAW)
    x, y = x * cyw - y * syw, x * syw + y * cyw
    cp, sp = math.cos(VIEW_PITCH), math.sin(VIEW_PITCH)
    y, z = y * cp - z * sp, y * sp + z * cp
    return cx + x * s, cy - z * s, y  # depth: larger y is further away


# ---------------------------------------------------------------- window

BODY_PARTS = ('Chest', 'Hips', 'Left thigh', 'Right thigh', 'Left ankle', 'Right ankle',
              'Left foot', 'Right foot', 'Left upper arm', 'Right upper arm', 'Left lower arm', 'Right lower arm')
FEET = ('Left foot', 'Right foot')
# An unassigned part moves with its parent; the hips are the root.
PARENT = {'Chest': 'Hips'}
for _side in ('Left', 'Right'):
    PARENT.update({f'{_side} thigh': 'Hips', f'{_side} ankle': f'{_side} thigh',
                   f'{_side} foot': f'{_side} ankle', f'{_side} upper arm': 'Chest',
                   f'{_side} lower arm': f'{_side} upper arm'})
LEGACY_PARTS = {'Left lower leg': 'Left ankle', 'Right lower leg': 'Right ankle'}  # config versions 1-3
IDENTITY = (1, 0, 0, 0)
CONFIG_PATH = Path(__file__).resolve().with_name('slime_view_config.json')


def unit_quaternion(q):
    if q is None or not all(math.isfinite(a) for a in q):
        return None
    norm = math.hypot(*q)
    return tuple(a / norm for a in q) if math.isfinite(norm) and norm > 1e-6 else None


def q_multiply(a, b):
    w, x, y, z = a
    v, i, j, k = b
    return (w*v - x*i - y*j - z*k, w*i + x*v + y*k - z*j,
            w*j - x*k + y*v + z*i, w*k + x*j - y*i + z*v)


def q_inverse(q):
    """Inverse of a unit quaternion."""
    return (q[0], -q[1], -q[2], -q[3])


def normalized_mac(value):
    if not isinstance(value, str):
        return None
    value = value.replace(':', '').replace('-', '').upper()
    if (len(value) == 12 and all(c in '0123456789ABCDEF' for c in value)
            and value not in ('000000000000', 'FFFFFFFFFFFF')):
        return value
    return None


def tracker_key(tracker):
    mac = normalized_mac(tracker.get('mac'))
    return ('mac', mac) if mac else tracker['addr']


# ---------------------------------------------------------------- SlimeVR Server assignments
#
# SlimeVR Server keeps its tracker assignments in its config directory
# (Windows %APPDATA%, macOS ~/Library/Application Support, Linux $XDG_CONFIG_HOME or ~/.config,
# each + /dev.slimevr.SlimeVR; or vrconfig.yml beside the server if it has a config/ folder):
#   vrconfig.yml (server main branch), YAML "trackers:" map, one entry per tracker name:
#     udp://AA:BB:CC:DD:EE:FF/0  -> designation: "body:chest"   (Wi-Fi, sensor 0)
#     AABBCCDDEEFF/0             -> designation: "body:chest"   (nRF receiver over HID)
#   settings/<profile>.json (server-rewrite; profile from global.json selectedSettingsProfile):
#     "trackers": {"AA:BB:CC:DD:EE:FF:0": {"bodyPart": "CHEST"}, "AABBCCDDEEFF": {...}}
# One MAC can have a Wi-Fi and a HID entry with different parts; the entry for the transport the
# tracker currently uses wins. Between the two files, the more recently written one wins.

SLIMEVR_ID = 'dev.slimevr.SlimeVR'
# SlimeVR body part -> (our part, preference when several trackers map to the same part)
SLIMEVR_PARTS = {
    'CHEST': ('Chest', 0), 'UPPER_CHEST': ('Chest', 1),
    'HIP': ('Hips', 0), 'WAIST': ('Hips', 1),
    'LEFT_UPPER_LEG': ('Left thigh', 0), 'RIGHT_UPPER_LEG': ('Right thigh', 0),
    'LEFT_LOWER_LEG': ('Left ankle', 0), 'RIGHT_LOWER_LEG': ('Right ankle', 0),
    'LEFT_FOOT': ('Left foot', 0), 'RIGHT_FOOT': ('Right foot', 0),
    'LEFT_UPPER_ARM': ('Left upper arm', 0), 'RIGHT_UPPER_ARM': ('Right upper arm', 0),
    'LEFT_LOWER_ARM': ('Left lower arm', 0), 'RIGHT_LOWER_ARM': ('Right lower arm', 0),
}


def slimevr_config_dir():
    if os.name == 'nt':
        base = os.environ.get('APPDATA')
        return Path(base, SLIMEVR_ID) if base else None
    if os.uname().sysname == 'Darwin':
        return Path.home() / 'Library' / 'Application Support' / SLIMEVR_ID
    return Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config', SLIMEVR_ID)


def slimevr_files(path):
    """The vrconfig.yml and settings profile to read for a directory or a single file."""
    if path is None:
        return []
    path = Path(path)
    if path.is_file() or path.suffix in ('.yml', '.yaml', '.json'):
        return [path]
    profile = 'default'
    try:
        profile = json.loads((path / 'global.json').read_text(encoding='utf-8')).get(
            'selectedSettingsProfile') or profile
    except (OSError, ValueError, AttributeError):
        pass
    return [path / 'vrconfig.yml', path / 'settings' / f'{profile}.json']


def parse_slimevr_tracker_name(name):
    """('usb' | 'wifi', MAC) for sensor 0 of a SlimeVR tracker name, else None."""
    if not isinstance(name, str):
        return None
    transport = 'usb'
    if name.startswith('udp://'):  # legacy Wi-Fi: udp://AA:BB:CC:DD:EE:FF/0
        transport, name = 'wifi', name[6:]
    base, sep, sensor = name.rpartition('/')
    if not sep:  # rewrite: AA:BB:CC:DD:EE:FF:0 (Wi-Fi) or AABBCCDDEEFF (HID)
        base, sensor = name, '0'
        if name.count(':') == 6:
            base, _, sensor = name.rpartition(':')
            transport = 'wifi'
    if sensor != '0':
        return None  # extension sensors share the MAC; this viewer shows one tracker per MAC
    mac = normalized_mac(base)
    return (transport, mac) if mac else None


def yaml_scalar(text):
    text = text.strip()
    if text.startswith('"'):
        return json.loads(text)  # Jackson writes JSON-compatible double-quoted strings
    if text.startswith("'"):
        return text[1:-1].replace("''", "'")
    return None if text in ('null', '~', '') else text


def read_vrconfig_designations(text):
    """{tracker name: designation} from vrconfig.yml without needing a YAML library."""
    result, name, in_trackers = {}, None, False
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        indent = len(line) - len(line.lstrip(' '))
        if indent == 0:
            in_trackers = line.rstrip() == 'trackers:'
            name = None
        elif in_trackers and indent == 2 and line.rstrip().endswith(':'):
            name = yaml_scalar(line.strip()[:-1])
        elif in_trackers and indent == 4 and name is not None and line.strip().startswith('designation:'):
            result[name] = yaml_scalar(line.strip()[len('designation:'):])
    return result


def read_slimevr_assignments(files):
    """{(transport, MAC): SlimeVR body part or None}; newer files override older ones."""
    entries = []
    for f in files:
        try:
            mtime, text = f.stat().st_mtime, f.read_text(encoding='utf-8')
            if f.suffix == '.json':
                trackers = json.loads(text).get('trackers') or {}
                parts = {n: c.get('bodyPart') for n, c in trackers.items() if isinstance(c, dict)}
            else:
                parts = read_vrconfig_designations(text)
        except (OSError, ValueError, AttributeError):
            continue
        entries.append((mtime, parts))
    result = {}
    for _, parts in sorted(entries, key=lambda e: e[0]):
        for name, part in parts.items():
            key = parse_slimevr_tracker_name(name)
            if key:
                part = part.split(':', 1)[1] if isinstance(part, str) and part.startswith('body:') else part
                result[key] = part.upper() if isinstance(part, str) else None
    return result


def resolve_slimevr_assignments(saved, snapshot):
    """{our body part: ('mac', MAC)} for the trackers SlimeVR Server has assigned.

    For each MAC, use the entry for the transport the tracker is on now (or this viewer's
    current source when the tracker is not visible). Prefer visible trackers per body part."""
    seen = {}
    for t in snapshot:
        mac = normalized_mac(t.get('mac'))
        if mac:
            seen[mac] = t['source']
    sources = set(seen.values())
    current = sources.pop() if len(sources) == 1 else None
    by_mac = {}
    for (transport, mac), part in saved.items():
        by_mac.setdefault(mac, {})[transport] = part
    candidates = {}
    for mac, parts in by_mac.items():
        transport = seen.get(mac, current)
        # No entry for this transport: use the other one, unless the two entries disagree.
        part = parts[transport] if transport in parts else (
            next(iter(parts.values())) if len(set(parts.values())) == 1 else None)
        if part in SLIMEVR_PARTS:
            ours, preference = SLIMEVR_PARTS[part]
            candidates.setdefault(ours, []).append((mac not in seen, preference, mac))
    return {part: ('mac', min(options)[2]) for part, options in candidates.items()}


class Skeleton:
    """Absolute segment rotations for the assigned parts, with a fixed pelvis and Z-up geometry."""
    def __init__(self):
        self.assignments = {}  # body part -> ('mac', address), or legacy transport key
        self.reference = {}
        self.mounting = {}  # per-segment heading correction about world Z
        self.last_rotations = {}

    def save(self, path):
        path = Path(path)
        data = {'version': 4, 'assignments': self.assignments,
                'calibration': self.reference, 'mounting': self.mounting}
        # Replace only after a complete write, so a failed save preserves the old file.
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                             prefix=path.name + '.', suffix='.tmp', delete=False) as f:
                temporary = Path(f.name)
                json.dump(data, f, indent=2, allow_nan=False)
                f.write('\n')
                f.flush()
                os.fsync(f.fileno())
            temporary.replace(path)
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()

    def load(self, path):
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        if not isinstance(data, dict) or type(data.get('version')) is not int or data['version'] not in (1, 2, 3, 4):
            raise ValueError('Unsupported config version')
        assignments, reference = data.get('assignments'), data.get('calibration')
        if not isinstance(assignments, dict) or not isinstance(reference, dict):
            raise ValueError('Assignments and calibration must be objects')
        if data['version'] < 4:
            assignments = {LEGACY_PARTS.get(p, p): k for p, k in assignments.items()}
            reference = {LEGACY_PARTS.get(p, p): q for p, q in reference.items()}
        parsed_assignments = {}
        for part, key in assignments.items():
            if part not in BODY_PARTS or not isinstance(key, list) or len(key) != 2:
                raise ValueError('Invalid tracker assignment')
            if data['version'] >= 3 and key[0] == 'mac':
                mac = normalized_mac(key[1])
                if mac is None:
                    raise ValueError('Invalid tracker MAC')
                key = ['mac', mac]
            elif (not isinstance(key[0], str) or not key[0]
                  or type(key[1]) is not int or not 0 <= key[1] <= 65535):
                raise ValueError('Invalid tracker assignment')
            parsed_assignments[part] = tuple(key)
        if len(set(parsed_assignments.values())) != len(parsed_assignments):
            raise ValueError('A tracker cannot have multiple body parts')
        if not set(reference) <= set(parsed_assignments):
            raise ValueError('Calibration for an unassigned body part')
        parsed_reference = {}
        for part, q in reference.items():
            if (not isinstance(q, list) or len(q) != 4
                    or any(type(a) not in (int, float) for a in q)):
                raise ValueError('Invalid calibration quaternion')
            normalized = unit_quaternion(q)
            if normalized is None:
                raise ValueError('Invalid calibration quaternion')
            parsed_reference[part] = normalized
        mounting = data.get('mounting') if data['version'] >= 2 else {}
        if not isinstance(mounting, dict):
            raise ValueError('Mounting must be an object')
        if data['version'] < 4:
            mounting = {LEGACY_PARTS.get(p, p): q for p, q in mounting.items()}
        if not set(mounting) <= set(parsed_reference):
            raise ValueError('Mounting correction without a standing reference')
        parsed_mounting = {}
        for part, q in mounting.items():
            if (not isinstance(q, list) or len(q) != 4
                    or any(type(a) not in (int, float) for a in q)):
                raise ValueError('Invalid mounting quaternion')
            normalized = unit_quaternion(q)
            if normalized is None or abs(normalized[1]) > 1e-6 or abs(normalized[2]) > 1e-6:
                raise ValueError('Mounting correction must rotate about Z')
            parsed_mounting[part] = normalized
        # Apply only once the entire document has been validated.
        self.assignments = parsed_assignments
        self.reference = parsed_reference
        self.mounting = parsed_mounting
        self.last_rotations = dict.fromkeys(parsed_reference, IDENTITY)

    def assign(self, key, part):
        updated = {p: k for p, k in self.assignments.items() if k != key and p != part}
        if part in BODY_PARTS:
            updated[part] = key
        if updated != self.assignments:
            self.assignments = updated
            self.reference.clear()
            self.mounting.clear()
            self.last_rotations.clear()

    def apply_assignments(self, assigned):
        """Take these body parts; keep others unless their tracker now has another part."""
        updated = {p: k for p, k in self.assignments.items()
                   if p not in assigned and k not in assigned.values()}
        updated.update(assigned)
        if updated == self.assignments:
            return False
        self.assignments = updated
        self.reference.clear()
        self.mounting.clear()
        self.last_rotations.clear()
        return True

    def migrate_assignments(self, snapshot):
        """Promote known transport keys when their MAC arrives, keeping calibration."""
        identities = {t['addr']: tracker_key(t) for t in snapshot}
        changed = False
        for part, key in list(self.assignments.items()):
            identity = identities.get(key, key)
            if identity != key and identity not in self.assignments.values():
                self.assignments[part] = identity
                changed = True
        return changed

    def samples(self, snapshot, now, require_live=True):
        trackers = {}
        for t in snapshot:
            if unit_quaternion(t['q']) is None:
                continue
            # If the same tracker arrives twice, use its newest valid sample.
            for key in (t['addr'], tracker_key(t)):
                previous = trackers.get(key)
                if previous is None or (t['last_real'] or 0) > (previous['last_real'] or 0):
                    trackers[key] = t
        result = {}
        for part, key in self.assignments.items():
            t = trackers.get(key)
            if t and (not require_live or
                      (t['last_real'] is not None and now - t['last_real'] < STALE_S)):
                q = unit_quaternion(t['q'])
                if q is not None:
                    result[part] = q
        return result

    def calibration_notice(self, snapshot, now, samples):
        inactive = set(samples) - self.samples(snapshot, now).keys()
        return f' Used last received data for {len(inactive)} inactive tracker(s).' if inactive else ''

    def calibrate(self, snapshot, now):
        samples = self.samples(snapshot, now, require_live=False)
        if not self.assignments:
            return 'Waiting for assignments.'
        missing = [p for p in BODY_PARTS if p in self.assignments and p not in samples]
        if missing:
            return 'Waiting for assigned rotation data: ' + ', '.join(missing)
        self.reference = samples
        self.mounting.clear()
        self.last_rotations = dict.fromkeys(samples, IDENTITY)
        return 'Full reset saved.' + self.calibration_notice(snapshot, now, samples)

    def calibrate_mounting(self, snapshot, now, feet=False):
        """Ski pose (feet=False) or tiptoes (feet=True), as in SlimeVR Server.

        Feet stay flat in the ski pose, so they get their own reset. Each part's upward axis
        must tilt; its direction gives the tracker's heading on the body."""
        if not self.reference:
            return 'Waiting for standing reset. Complete step 1 first.'
        parts = [p for p in BODY_PARTS if p in self.assignments and (p in FEET) == feet]
        if not parts:
            return 'Waiting for assigned feet.' if feet else 'Waiting for assignments.'
        unreset = [p for p in parts if p not in self.reference]
        if unreset:
            return 'Waiting for standing reset: ' + ', '.join(unreset)
        samples = self.samples(snapshot, now, require_live=False)
        missing = [p for p in parts if p not in samples]
        if missing:
            return 'Waiting for assigned rotation data: ' + ', '.join(missing)
        corrections, weak = {}, []
        for part in parts:
            delta = q_multiply(samples[part], q_inverse(self.reference[part]))
            up = q_rotate(delta, (0, 0, 1))
            tilt = math.atan2(math.hypot(up[0], up[1]), up[2])
            if not math.radians(10) <= tilt <= math.radians(85):
                weak.append(part)
                continue
            # Z-up, forward = -Y. In a ski pose, knee->hip and hand->elbow point backward;
            # the torso, ankles and upper arms (elbows back) lean forward. On tiptoes the
            # feet's upward axes lean forward too.
            backward = part.endswith(('thigh', 'lower arm'))
            target = math.pi / 2 if backward else -math.pi / 2
            yaw = target - math.atan2(up[1], up[0])
            corrections[part] = (math.cos(yaw / 2), 0, 0, math.sin(yaw / 2))
        if weak:
            bend = 'tiptoe' if feet else 'ski bend'
            return f'Waiting for a clear {bend} (10-85 degrees): ' + ', '.join(weak)
        # Replace as a group; a failed attempt leaves the previous calibration intact.
        self.mounting = {p: q for p, q in self.mounting.items() if (p in FEET) != feet} | corrections
        self.last_rotations.clear()
        next_step = '' if feet or not any(p in self.assignments for p in FEET) else ' Now Feet reset.'
        return ('Feet reset saved.' if feet else 'Mount reset saved.' + next_step) + self.calibration_notice(
            snapshot, now, {p: samples[p] for p in parts})

    def pose(self, snapshot, now):
        samples = self.samples(snapshot, now)
        live = set()
        for part, initial in self.reference.items():
            if part in samples:
                # Absolute world-space change cancels a fixed sensor mounting rotation.
                delta = q_multiply(samples[part], q_inverse(initial))
                correction = self.mounting.get(part, IDENTITY)
                # Change the world heading frame, preserving the standing identity.
                self.last_rotations[part] = q_multiply(q_multiply(correction, delta), q_inverse(correction))
                live.add(part)
        return skeleton_segments(self.last_rotations), live


def skeleton_segments(rotations):
    """Return (start, end, controlling part); dimensions are approximate metres.

    A part without a rotation follows its parent (PARENT); the segment reports that part."""
    segments = []

    def controller(part):
        while part not in rotations and part in PARENT:
            part = PARENT[part]
        return part

    def bone(start, offset, part):
        part = controller(part)
        delta = q_rotate(rotations.get(part, IDENTITY), offset)
        end = tuple(a + b for a, b in zip(start, delta))
        segments.append((start, end, part))
        return end

    pelvis = (0, 0, 0.92)
    waist = bone(pelvis, (0, 0, 0.16), 'Hips')
    neck = bone(waist, (0, 0, 0.36), 'Chest')
    bone(neck, (0, 0, 0.22), 'Chest')
    # With Z up and -Y forward, the wearer's left is +X.
    for side, sign in (('Left', 1), ('Right', -1)):
        hip = bone(pelvis, (sign * 0.14, 0, 0), 'Hips')
        knee = bone(hip, (0, 0, -0.43), side + ' thigh')
        ankle = bone(knee, (0, 0, -0.43), side + ' ankle')
        bone(ankle, (0, -0.18, -0.06), side + ' foot')
        shoulder = bone(neck, (sign * 0.20, 0, -0.04), 'Chest')
        elbow = bone(shoulder, (sign * 0.08, 0, -0.28), side + ' upper arm')
        bone(elbow, (0, 0, -0.25), side + ' lower arm')
    return segments


def age_text(t):
    if t is None:
        return "never"
    a = time.time() - t
    if a < 1:
        return f"{a * 1000:.0f} ms ago"
    if a < 120:
        return f"{a:.0f} s ago"
    return f"{a / 60:.0f} min ago"


# ---------------------------------------------------------------- Qt window

LEFT, RIGHT, ERROR = QColor('#3d8fd9'), QColor('#e8873a'), QColor('#d9443b')
COLUMNS, ROWS = 4, 2  # the window fits eight trackers; more scroll


def draw_arrow(painter, start, end, color):
    painter.setPen(QPen(color, 2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
    painter.drawLine(start, end)
    dx, dy = end.x() - start.x(), end.y() - start.y()
    length = math.hypot(dx, dy)
    if length < 4:
        return
    ux, uy, size = dx / length, dy / length, 6
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(color)
    painter.drawPolygon(QPolygonF([
        end, QPointF(end.x() - ux * size - uy * size * 0.5, end.y() - uy * size + ux * size * 0.5),
        QPointF(end.x() - ux * size + uy * size * 0.5, end.y() - uy * size - ux * size * 0.5)]))


class TrackerView(QWidget):
    """The tracker as a flat box with its X (red), Y (green) and Z (blue) axes."""
    SIZE = 130

    def __init__(self):
        super().__init__()
        self.setFixedSize(self.SIZE, self.SIZE)
        self.q, self.dim = None, False

    def set_rotation(self, q, dim):
        if (q, dim) != (self.q, self.dim):
            self.q, self.dim = q, dim
            self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.fillRect(self.rect(), self.palette().color(QPalette.ColorRole.Base))
        if self.q is None:
            p.setPen(self.palette().color(QPalette.ColorRole.PlaceholderText))
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, 'no data')
            return
        cx = cy = self.SIZE / 2
        s = self.SIZE * 0.27
        hx, hy, hz = BOX
        verts = [q_rotate(self.q, (sx * hx, sy * hy, sz * hz))
                 for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        pts = [to_screen(v, cx, cy, s) for v in verts]
        p.setPen(QPen(QColor('#111'), 1))
        for idx, col in sorted(FACES, key=lambda f: -sum(pts[i][2] for i in f[0])):
            color = QColor(col)
            if self.dim:
                grey = color.lightness() // 2 + 60
                color = QColor(grey, grey, grey)
            p.setBrush(color)
            p.drawPolygon(QPolygonF([QPointF(*pts[i][:2]) for i in idx]))
        for axis, col in (((1.6, 0, 0), '#e04040'), ((0, 1.3, 0), '#3aa83a'), ((0, 0, 1.0), '#3d7fe0')):
            x, y, _ = to_screen(q_rotate(self.q, axis), cx, cy, s)
            draw_arrow(p, QPointF(cx, cy), QPointF(x, y), QColor('#888' if self.dim else col))


class SkeletonView(QWidget):
    """Fixed-proportion preview; bones without live data are grey."""

    def __init__(self):
        super().__init__()
        self.setMinimumSize(240, 320)
        self.segments, self.live = [], set()

    def set_pose(self, segments, live):
        self.segments, self.live = segments, live
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        palette = self.palette()
        p.fillRect(self.rect(), palette.color(QPalette.ColorRole.Base))
        width, height = self.width(), self.height()
        scale = min(width / 2.8, height / 2.5)

        def project(v):
            x, y, depth = to_screen((v[0], v[1], v[2] - 0.85), width / 2, height / 2, scale)
            return QPointF(x, y), depth

        p.setPen(QPen(palette.color(QPalette.ColorRole.Midlight), 1))
        for i in range(-2, 3):
            for a, b in (((i * 0.4, -0.8, 0), (i * 0.4, 0.8, 0)),
                         ((-0.8, i * 0.4, 0), (0.8, i * 0.4, 0))):
                p.drawLine(project(a)[0], project(b)[0])
        idle = palette.color(QPalette.ColorRole.PlaceholderText)
        centre = palette.color(QPalette.ColorRole.Text)
        projected = [(project(a), project(b), part) for a, b, part in self.segments]
        for (a, _), (b, _), part in sorted(projected, key=lambda s: -(s[0][1] + s[1][1])):
            color = (idle if part not in self.live else LEFT if part.startswith('Left')
                     else RIGHT if part.startswith('Right') else centre)
            p.setPen(QPen(color, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawLine(a, b)


class TrackerCard(QGroupBox):
    WIDTH = 200

    def __init__(self, on_assign, on_remove=None):
        super().__init__()
        self.setFixedWidth(self.WIDTH)
        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        self.role = QComboBox()
        self.role.addItems(('Unassigned', *BODY_PARTS))
        self.role.activated.connect(lambda i: on_assign(self.role.itemText(i)))  # user choices only
        layout.addWidget(self.role)
        self.view = TrackerView()
        layout.addWidget(self.view, alignment=Qt.AlignmentFlag.AlignHCenter)
        self.state = QLabel()
        self.details = QLabel()
        self.details.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.state)
        layout.addWidget(self.details)
        # Unpair from its receiver (USB trackers on an nRF receiver); the card keeps its height
        self.remove = QPushButton('Remove')
        self.remove.setToolTip("Unpair this tracker from its receiver; the others keep their ids")
        if on_remove:
            self.remove.clicked.connect(on_remove)
        policy = self.remove.sizePolicy()
        policy.setRetainSizeWhenHidden(True)
        self.remove.setSizePolicy(policy)
        layout.addWidget(self.remove)

    def set_role(self, part):
        if self.role.currentText() != part:
            self.role.setCurrentText(part)

    def show_tracker(self, t, fresh, receiver, removable=False, busy=False):
        self.remove.setVisible(removable)
        self.remove.setEnabled(not busy)
        name = f"ID {t['id']}" if t['id'] is not None else f"Port {t['addr'][1]}"
        self.setTitle(f"{name}   {t['mac'] or '?'}")
        self.view.set_rotation(t['q'], not fresh)
        if t['status'] == 2:
            state, color = 'Error', ERROR
        elif fresh:
            state, color = 'Live', None
        else:
            state, color = f"Not seen, {age_text(t['last_real'])}", self.palette().color(QPalette.ColorRole.PlaceholderText)
        self.state.setText(state + (f'  ·  {receiver}' if receiver else ''))
        self.state.setStyleSheet(f'color: {color.name()};' if color else '')
        battery = ' '.join(filter(None, (
            f"{t['batt_pct']:.0f}%" if t['batt_pct'] is not None else None,
            f"{t['batt_v']:.2f} V" if t['batt_v'] is not None else None))) or '?'
        rate = f"{t['real_rate']}/s" + (f" (+{t['held_rate']} held)" if t['held_rate'] else '')
        self.details.setText('\n'.join((
            f"{battery}   {t['rssi']} dBm" if t['rssi'] is not None else battery,
            rate + (f"   {t['temp']:.0f} °C" if t['temp'] is not None else ''),
            f"fw {t['fw'] or '?'}")))


class App(QMainWindow):
    def __init__(self, server, config_path=CONFIG_PATH, slimevr_path=None):
        super().__init__()
        self.server = server
        self.config_path = Path(config_path)
        self.slimevr_files = slimevr_files(slimevr_path)
        self.slimevr_state = None  # (file times, visible trackers) at the last sync
        self.slimevr_next_check = 0
        self.skeleton = Skeleton()
        self.tracker_keys = {}
        self.cards = {}
        self.order = None  # tracker order currently placed in the grid
        self.countdown = None  # (seconds left, reset kind) while a reset counts down
        self.setWindowTitle('Slime View')

        root = QWidget()
        body = QHBoxLayout(root)
        self.setCentralWidget(root)

        side = QVBoxLayout()
        self.skeleton_view = SkeletonView()
        side.addWidget(self.skeleton_view, 1)
        buttons = QHBoxLayout()
        self.calibrate_button = QPushButton('Full reset')
        self.calibrate_button.clicked.connect(lambda: self.start_calibration(3, 'full'))
        self.mounting_button = QPushButton('Mount reset')
        self.mounting_button.clicked.connect(lambda: self.start_calibration(5, 'mount'))
        self.feet_button = QPushButton('Feet reset')
        self.feet_button.clicked.connect(lambda: self.start_calibration(3, 'feet'))
        buttons.addWidget(self.calibrate_button)
        buttons.addWidget(self.mounting_button)
        buttons.addWidget(self.feet_button)
        side.addLayout(buttons)
        self.follow_slimevr = QCheckBox('Follow SlimeVR')
        self.follow_slimevr.setChecked(bool(self.slimevr_files))
        self.follow_slimevr.setEnabled(bool(self.slimevr_files))
        self.follow_slimevr.setToolTip('\n'.join(str(f) for f in self.slimevr_files))
        self.follow_slimevr.toggled.connect(self.slimevr_toggled)
        side.addWidget(self.follow_slimevr)
        self.message = QLabel()
        self.message.setWordWrap(True)
        self.message.setFixedWidth(240)
        self.message.setMinimumHeight(self.message.fontMetrics().lineSpacing() * 3)
        self.message.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        side.addWidget(self.message)
        body.addLayout(side)

        cards = QWidget()
        self.grid = QGridLayout(cards)
        self.grid.setContentsMargins(0, 0, 0, 0)
        self.grid.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        self.empty = QLabel('No trackers')
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.empty.setEnabled(False)
        self.scroll = QScrollArea()
        self.scroll.setWidget(cards)
        self.scroll.setWidgetResizable(True)
        self.scroll.setFrameShape(QFrame.Shape.NoFrame)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        # Size for exactly COLUMNS x ROWS cards, keeping room for the scroll bar.
        probe = TrackerCard(lambda part: None)
        probe.show_tracker(dict(Tracker(('', 0)).__dict__, last_real=time.time(), batt_pct=0,
                                batt_v=0, rssi=0, temp=0, fw='0.0.0', mac='0' * 12), True, '')
        gap = self.grid.spacing()
        scrollbar = self.style().pixelMetric(QStyle.PixelMetric.PM_ScrollBarExtent)
        self.scroll.setFixedSize(COLUMNS * TrackerCard.WIDTH + (COLUMNS - 1) * gap + scrollbar + gap,
                                 ROWS * probe.sizeHint().height() + (ROWS - 1) * gap)
        probe.deleteLater()
        body.addWidget(self.scroll)

        self.status = QLabel()
        self.statusBar().setSizeGripEnabled(False)  # the window has a fixed size
        self.statusBar().addWidget(self.status, 1)
        # Pairing on an nRF receiver (its console: pair / exit); which one, when there are several
        try:
            import serial  # noqa: F401  (pyserial, for the receiver's console)
            self.console_note = None
        except ImportError:
            self.console_note = 'Pair and Remove need pyserial: python -m pip install pyserial'
        self.console_results = []  # (callback, reply or exception) from console threads
        self.console_busy = False
        self.pairing = set()  # receivers (USB serial numbers) in pairing mode
        self.snap = {}  # the last snapshot by tracker address
        self.pair_receiver = QComboBox()
        self.pair_receiver.setToolTip('Receiver to pair with')
        self.pair_receiver.hide()
        self.pair_receiver.currentIndexChanged.connect(lambda i: self.update_pair_button())
        self.statusBar().addPermanentWidget(self.pair_receiver)
        self.pair_button = QPushButton('Pair')
        self.pair_button.setCheckable(True)
        self.pair_button.setToolTip(self.console_note or
                                    'Pairing mode on the receiver: trackers asking to pair are added '
                                    '(in BLE mode: trackers in BLE mode not paired yet). Click again to stop.')
        self.pair_button.clicked.connect(self.pair_clicked)
        self.pair_button.hide()  # until an nRF receiver is on USB
        self.statusBar().addPermanentWidget(self.pair_button)
        forget = QPushButton('Forget stale')
        forget.clicked.connect(server.forget_stale)
        self.statusBar().addPermanentWidget(forget)

        self.calibration_timer = QTimer(self)
        self.calibration_timer.setSingleShot(True)
        self.calibration_timer.setInterval(1000)
        self.calibration_timer.timeout.connect(self.countdown_step)
        try:
            self.skeleton.load(self.config_path)
        except FileNotFoundError:
            pass
        except (OSError, ValueError, OverflowError) as e:
            self.show_message(f'Could not load {self.config_path.name}: {e}', warn=True)
        self.update_buttons()
        self.tick()
        self.setFixedSize(self.sizeHint())
        self.timer = QTimer(self)
        self.timer.setInterval(50)
        self.timer.timeout.connect(self.tick)
        self.timer.start()

    def show_message(self, text, warn=False):
        self.message.setText(text)
        self.message.setStyleSheet(f'color: {ERROR.name()};' if warn else '')

    # ---- receiver console: pairing and unpairing ----

    def run_console(self, receiver, command, replies, done):
        """The command on a worker thread (the console answers within ~2 s); done(reply or
        exception) runs on the next tick."""
        self.console_busy = True

        def work():
            try:
                result = receiver_command(receiver, command, replies)
            except Exception as e:  # serial errors vary by platform
                result = e
            self.console_results.append((done, result))

        threading.Thread(target=work, daemon=True).start()

    def console_failed(self, result, what):
        if isinstance(result, Exception):
            self.show_message(f'{what}: {result}', warn=True)
        elif result == 'Unknown command':
            self.show_message(f'{what}: this receiver firmware cannot do it (update the receiver).', warn=True)
        elif result is None:
            self.show_message(f'{what}: no answer from the receiver (older receiver firmware does not '
                              'confirm; update it).', warn=True)
        else:
            return False
        return True

    def pair_target(self):
        receivers = self.pair_receiver.property('serials') or []
        i = self.pair_receiver.currentIndex()
        return receivers[i] if 0 <= i < len(receivers) else None

    def update_pair_button(self):
        target = self.pair_target()
        self.pair_button.setEnabled(bool(target) and not self.console_busy and not self.console_note)
        self.pair_button.setChecked(target in self.pairing)
        self.pair_button.setText('Stop pairing' if target in self.pairing else 'Pair')

    def pair_clicked(self):
        target = self.pair_target()
        if not target:
            return
        on = target not in self.pairing

        def done(result):
            if not self.console_failed(result, 'Pairing'):
                if result.startswith('Pairing on'):
                    self.pairing.add(target)
                    self.show_message('Pairing: put a tracker into pairing mode (or switch it to BLE); '
                                      'it appears here when added. Click Stop pairing when done.')
                else:
                    self.pairing.discard(target)
                    self.show_message('Pairing stopped.')

        self.run_console(target, 'pair' if on else 'exit', ('Pairing on', 'Pairing off'), done)
        self.update_pair_button()

    def remove_tracker(self, addr):
        t = self.snap.get(addr)
        if not t or t['id'] is None or not pairable(t['receiver']):
            return
        name = f"ID {t['id']} ({t['mac'] or '?'})"
        answer = QMessageBox.question(
            self, 'Remove tracker',
            f'Unpair {name} from its receiver?\n\nIts data is ignored from now on; it comes back only '
            'by pairing again. The other trackers keep their ids.')
        if answer != QMessageBox.StandardButton.Yes:
            return

        def done(result):
            if not self.console_failed(result, f'Remove {name}'):
                if result.startswith('Unpaired'):
                    self.server.forget(addr)
                    self.show_message(f'Removed {name}.')
                else:
                    self.show_message(f'Remove {name}: {result}', warn=True)

        self.run_console(t['receiver'], f"unpair {t['id']}", ('Unpaired', 'Unpair:', 'Usage: unpair'), done)

    def closeEvent(self, event):
        for receiver in list(self.pairing):  # leave no receiver in pairing mode
            try:
                receiver_command(receiver, 'exit', ('Pairing off',), timeout=1.0)
            except Exception:
                pass
        super().closeEvent(event)

    def update_buttons(self):
        idle = self.countdown is None
        self.calibrate_button.setEnabled(idle)
        self.mounting_button.setEnabled(idle and bool(self.skeleton.reference))
        self.feet_button.setEnabled(idle and any(p in self.skeleton.reference for p in FEET))

    def assign_role(self, key, part):
        self.skeleton.assign(self.tracker_keys.get(key, key), part)
        self.sync_roles()
        self.cancel_calibration()
        # A manual choice would be overwritten by the next sync, so stop following.
        self.follow_slimevr.setChecked(False)
        self.show_message('Reassigned. Reset again.')
        self.save_config()

    def slimevr_toggled(self):
        self.slimevr_state = None  # sync on the next tick
        self.slimevr_next_check = 0

    def sync_slimevr(self, snap):
        """Copy SlimeVR Server's assignments when its config or the visible trackers change."""
        now = time.monotonic()
        if not self.follow_slimevr.isChecked() or now < self.slimevr_next_check:
            return
        self.slimevr_next_check = now + 1
        times = []
        for f in self.slimevr_files:
            try:
                times.append(f.stat().st_mtime)
            except OSError:
                times.append(None)
        state = (tuple(times), frozenset((normalized_mac(t['mac']), t['source']) for t in snap))
        if state == self.slimevr_state:
            return
        self.slimevr_state = state
        if not any(times):
            self.show_message('SlimeVR config not found.', warn=True)
            return
        assigned = resolve_slimevr_assignments(read_slimevr_assignments(self.slimevr_files), snap)
        if assigned and self.skeleton.apply_assignments(assigned):
            self.sync_roles()
            self.cancel_calibration()
            self.show_message('Assigned from SlimeVR. Reset again.')
            self.save_config()

    def sync_roles(self):
        for addr, card in self.cards.items():
            identity = self.tracker_keys.get(addr, addr)
            card.set_role(next((p for p, k in self.skeleton.assignments.items() if k == identity), 'Unassigned'))

    def save_config(self):
        try:
            self.skeleton.save(self.config_path)
        except (OSError, ValueError) as e:
            self.show_message(f'Could not save {self.config_path.name}: {e}', warn=True)

    def cancel_calibration(self):
        self.calibration_timer.stop()
        self.countdown = None
        self.update_buttons()

    def start_calibration(self, seconds, kind):
        self.countdown = (seconds, kind)
        self.update_buttons()
        self.countdown_step()

    def countdown_step(self):
        remaining, kind = self.countdown
        if remaining:
            pose = {'full': 'Stand straight', 'mount': 'Ski pose, arms bent', 'feet': 'On tiptoes'}[kind]
            self.show_message(f'{pose}… {remaining}')
            self.countdown = (remaining - 1, kind)
            self.calibration_timer.start()
            return
        snapshot, now = self.server.snapshot(), time.time()
        if kind == 'full':
            message = self.skeleton.calibrate(snapshot, now)
        else:
            message = self.skeleton.calibrate_mounting(snapshot, now, feet=kind == 'feet')
        self.cancel_calibration()
        failed = message.startswith('Waiting')
        self.show_message(message, warn=failed)
        if not failed:
            self.save_config()

    def place_cards(self, order):
        if order == self.order:
            return
        self.order = order
        self.grid.removeWidget(self.empty)
        for n, key in enumerate(order):
            self.grid.addWidget(self.cards[key], n // COLUMNS, n % COLUMNS)
        if order:
            self.empty.hide()
        else:
            self.grid.addWidget(self.empty, 0, 0, ROWS, COLUMNS)
            self.empty.show()

    def tick_console(self):
        while self.console_results:
            done, result = self.console_results.pop(0)
            self.console_busy = False
            done(result)
        receivers = [r for r in self.server.receivers() if pairable(r)]
        self.pairing &= set(receivers)  # unplugged
        if receivers != (self.pair_receiver.property('serials') or []):
            current = self.pair_target()
            self.pair_receiver.blockSignals(True)
            self.pair_receiver.clear()
            self.pair_receiver.addItems([f'R{n + 1} …{r[-4:]}' for n, r in enumerate(receivers)])
            self.pair_receiver.setProperty('serials', receivers)
            if current in receivers:
                self.pair_receiver.setCurrentIndex(receivers.index(current))
            self.pair_receiver.blockSignals(False)
            self.pair_receiver.setVisible(len(receivers) > 1)
            self.pair_button.setVisible(bool(receivers))
        self.update_pair_button()

    def tick(self):
        self.tick_console()
        snap = sorted(self.server.snapshot(),
                      key=lambda t: (t['source'], t["addr"][0], t["id"] if t["id"] is not None else 99))
        self.snap = {t['addr']: t for t in snap}
        self.tracker_keys = {t['addr']: tracker_key(t) for t in snap}
        if self.skeleton.migrate_assignments(snap):
            self.save_config()
        keys = {t["addr"] for t in snap}
        for k in [k for k in self.cards if k not in keys]:
            card = self.cards.pop(k)  # saved assignments stay when a tracker disappears
            self.grid.removeWidget(card)
            card.deleteLater()
        for t in snap:
            if t['addr'] not in self.cards:
                self.cards[t['addr']] = TrackerCard(lambda part, k=t['addr']: self.assign_role(k, part),
                                                    lambda checked=False, k=t['addr']: self.remove_tracker(k))
        self.sync_slimevr(snap)
        self.sync_roles()
        self.place_cards(tuple(t['addr'] for t in snap))
        receivers = sorted({s['addr'][0] for s in snap if s['source'] == 'usb'})
        now, live = time.time(), 0
        for t in snap:
            fresh = t["last_real"] is not None and now - t["last_real"] < STALE_S
            live += fresh
            receiver = (f"R{receivers.index(t['addr'][0]) + 1}" if t['source'] == 'usb' and len(receivers) > 1
                        else '')
            removable = t['source'] == 'usb' and t['id'] is not None and pairable(t['receiver']) and not self.console_note
            self.cards[t['addr']].show_tracker(t, fresh, receiver, removable, self.console_busy)
        error = self.server.error
        self.status.setText(f'{self.server.description}   ·   {live}/{len(snap)} live'
                            + (f'   ·   {error}' if error else ''))
        self.skeleton_view.set_pose(*self.skeleton.pose(snap, now))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=6969, help="UDP port for the Wi-Fi hub (default 6969)")
    only = ap.add_mutually_exclusive_group()
    only.add_argument("--usb", action="store_true", help="Only nRF receivers over USB-HID")
    only.add_argument("--wifi", action="store_true", help="Only the ESP32 hub over Wi-Fi")
    ap.add_argument('--config', type=Path, default=CONFIG_PATH, help='JSON assignments and calibration file')
    slimevr = ap.add_mutually_exclusive_group()
    slimevr.add_argument('--slimevr-config', type=Path, default=slimevr_config_dir(), metavar='PATH',
                         help='SlimeVR Server config folder, vrconfig.yml or settings profile .json '
                              f'(default {slimevr_config_dir()})')
    slimevr.add_argument('--no-slimevr', action='store_true',
                         help="Do not take body part assignments from SlimeVR Server's config")
    args = ap.parse_args()
    try:
        if args.usb:
            server = USBServer()
        elif args.wifi:
            server = Server(args.port)
        else:
            server = AutoServer(args.port)
    except RuntimeError as e:
        raise SystemExit(str(e))
    except OSError as e:
        raise SystemExit(f"Cannot open UDP {args.port} ({e}). Is SlimeVR Server still running?")
    try:
        app = QApplication(sys.argv[:1])
        window = App(server, args.config, None if args.no_slimevr else args.slimevr_config)
        window.show()
        app.exec()
    finally:
        server.close()


if __name__ == "__main__":
    main()
