#!/usr/bin/env python3
"""Slime View: a small stand-in for SlimeVR Server to check the hub's trackers.

Quit SlimeVR Server first so it does not compete for tracker data. In Wi-Fi mode the hub
finds this program on UDP 6969 and relays every tracker to it. Shows per tracker: 3D orientation, last
real data, status, battery, signal, temperature and packet rates.
For a simple skeleton, select a body part on each of six tracker cards (chest, hips,
thighs and lower legs). Click Calibrate, then stand upright with straight legs during
the three-second countdown. Then use Ski-pose mounting: keep feet parallel and flat,
bend knees, push hips back and lean the torso forward without turning. Hold during
the five-second countdown. Repeat both steps after remounting or tracker restarts.
Assignments, standing references and ski-pose corrections are saved automatically to
slime_view_config.json beside this script (override with --config PATH).
Calibration accepts the last valid rotation from inactive trackers; those samples
must still represent the requested pose. The status reports when older data is used.
The preview uses fixed proportions and a stationary hip position; arms and head
follow the chest, and feet follow the lower legs.

    python slime_view.py [--port 6969]
    python slime_view.py --usb

Wi-Fi needs only the standard library. USB needs: python -m pip install hidapi
Plug the nRF receiver into the PC for USB mode; no Wi-Fi or ESP32 hub is needed.
On Windows, allow Python through the firewall for private networks when asked.
"""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import struct
import threading
import tempfile
import time
import tkinter as tk
from tkinter import ttk

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
        self.description = f"Wi-Fi / UDP {self.sock.getsockname()[1]}"
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


def decode_hid_packet(t, p):
    """Decode receiver firmware's 16-byte record (see src/Hub/main/decoder.c)."""
    kind = p[0]
    if kind == 255:
        t.mac = p[2:8].hex().upper()
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
        self.error = None
        self.description = 'USB-HID: waiting for an nRF receiver'
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)

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
                    t.last_any = 0
                decode_hid_packet(t, p)

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
                        for path in found:
                            if path in devices:
                                continue
                            dev = self.hid.device()
                            try:
                                dev.open_path(path)
                                dev.set_nonblocking(1)
                                devices[path] = dev
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
                            self.handle(data, path.hex() if isinstance(path, bytes) else str(path))
                    except (OSError, ValueError) as e:
                        self.error = str(e)
                        devices.pop(path).close()
                self.description = (f'USB-HID: {len(devices)} receiver(s) connected'
                                    if devices else 'USB-HID: waiting for an nRF receiver')
                if now >= next_tick:
                    next_tick = now + 1
                    with self.lock:
                        for t in self.trackers.values():
                            t.real_rate, t.held_rate, t.real, t.held = t.real, t.held, 0, 0
                self.stop.wait(0.002)
        finally:
            for dev in devices.values():
                dev.close()


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


def draw_tracker(c, q, size, dim):
    c.delete("all")
    cx = cy = size / 2
    s = size * 0.28
    if q is None:
        c.create_text(cx, cy, text="no rotation", fill="#888")
        return
    hx, hy, hz = BOX
    verts = [q_rotate(q, (sx * hx, sy * hy, sz * hz))
             for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    pts = [to_screen(v, cx, cy, s) for v in verts]
    faces = sorted(FACES, key=lambda f: -sum(pts[i][2] for i in f[0]))
    for idx, col in faces:
        c.create_polygon([coord for i in idx for coord in pts[i][:2]],
                         fill="#555" if dim else col, outline="#111")
    for axis, col in (((1.6, 0, 0), "#ff5555"), ((0, 1.3, 0), "#55dd55"), ((0, 0, 1.0), "#5599ff")):
        x, y, _ = to_screen(q_rotate(q, axis), cx, cy, s)
        c.create_line(cx, cy, x, y, fill="#888" if dim else col, width=2, arrow=tk.LAST)


# ---------------------------------------------------------------- window

BODY_PARTS = ('Chest', 'Hips', 'Left thigh', 'Right thigh', 'Left lower leg', 'Right lower leg')
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


class Skeleton:
    """Six absolute segment rotations, with a fixed pelvis and Z-up geometry."""
    def __init__(self):
        self.assignments = {}  # body part -> ('mac', address), or legacy transport key
        self.reference = {}
        self.mounting = {}  # per-segment heading correction about world Z
        self.last_rotations = {}

    def save(self, path):
        path = Path(path)
        data = {'version': 3, 'assignments': self.assignments,
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
        if not isinstance(data, dict) or type(data.get('version')) is not int or data['version'] not in (1, 2, 3):
            raise ValueError('Unsupported config version')
        assignments, reference = data.get('assignments'), data.get('calibration')
        if not isinstance(assignments, dict) or not isinstance(reference, dict):
            raise ValueError('Assignments and calibration must be objects')
        parsed_assignments = {}
        for part, key in assignments.items():
            if part not in BODY_PARTS or not isinstance(key, list) or len(key) != 2:
                raise ValueError('Invalid tracker assignment')
            if data['version'] == 3 and key[0] == 'mac':
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
        if reference and (set(reference) != set(BODY_PARTS) or set(assignments) != set(BODY_PARTS)):
            raise ValueError('Calibration requires all six assignments')
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
        if mounting and (set(mounting) != set(BODY_PARTS) or not parsed_reference):
            raise ValueError('Mounting requires six standing references and corrections')
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
        missing = [p for p in BODY_PARTS if p not in samples]
        if missing:
            return 'Waiting for assigned rotation data: ' + ', '.join(missing)
        self.reference = samples
        self.mounting.clear()
        self.last_rotations = dict.fromkeys(BODY_PARTS, IDENTITY)
        return 'Full reset saved.' + self.calibration_notice(snapshot, now, samples)

    def calibrate_mounting(self, snapshot, now):
        if set(self.reference) != set(BODY_PARTS):
            return 'Waiting for standing reset. Complete step 1 first.'
        samples = self.samples(snapshot, now, require_live=False)
        missing = [p for p in BODY_PARTS if p not in samples]
        if missing:
            return 'Waiting for assigned rotation data: ' + ', '.join(missing)
        corrections, weak = {}, []
        for part in BODY_PARTS:
            delta = q_multiply(samples[part], q_inverse(self.reference[part]))
            up = q_rotate(delta, (0, 0, 1))
            tilt = math.atan2(math.hypot(up[0], up[1]), up[2])
            if not math.radians(10) <= tilt <= math.radians(85):
                weak.append(part)
                continue
            # Z-up, forward = -Y. In a squat, knee->hip points backward;
            # ankle->knee and the torso's upward axes lean forward.
            target = math.pi / 2 if part.endswith('thigh') else -math.pi / 2
            yaw = target - math.atan2(up[1], up[0])
            corrections[part] = (math.cos(yaw / 2), 0, 0, math.sin(yaw / 2))
        if weak:
            return 'Waiting for a clear ski bend (10-85 degrees): ' + ', '.join(weak)
        # Replace as a group; a failed attempt leaves the previous calibration intact.
        self.mounting = corrections
        self.last_rotations.clear()
        return 'Mount reset saved.' + self.calibration_notice(snapshot, now, samples)

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
    """Return (start, end, controlling part); dimensions are approximate metres."""
    segments = []

    def bone(start, offset, part):
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
        ankle = bone(knee, (0, 0, -0.43), side + ' lower leg')
        bone(ankle, (0, -0.18, -0.06), side + ' lower leg')
        shoulder = bone(neck, (sign * 0.20, 0, -0.04), 'Chest')
        elbow = bone(shoulder, (sign * 0.08, 0, -0.28), 'Chest')
        bone(elbow, (0, 0, -0.25), 'Chest')
    return segments


def draw_skeleton(canvas, segments, live):
    canvas.delete('all')
    width, height = max(300, canvas.winfo_width()), max(300, canvas.winfo_height())
    scale = min(width / 2.8, height / 2.5)

    def project(v):
        return to_screen((v[0], v[1], v[2] - 0.85), width / 2, height / 2, scale)

    for i in range(-2, 3):
        for a, b in (((i * 0.4, -0.8, 0), (i * 0.4, 0.8, 0)),
                     ((-0.8, i * 0.4, 0), (0.8, i * 0.4, 0))):
            canvas.create_line(*project(a)[:2], *project(b)[:2], fill='#29303c')
    projected = [(project(a), project(b), part) for a, b, part in segments]
    for a, b, part in sorted(projected, key=lambda segment: -(segment[0][2] + segment[1][2])):
        color = '#666666' if part not in live else (
            '#54baff' if part.startswith('Left') else '#ffac66' if part.startswith('Right') else '#c6d4ef')
        canvas.create_line(*a[:2], *b[:2], fill=color, width=5, capstyle=tk.ROUND)
        for x, y, _ in (a, b):
            canvas.create_oval(x-4, y-4, x+4, y+4, fill=color, outline='')

def age_text(t):
    if t is None:
        return "never"
    a = time.time() - t
    if a < 1:
        return f"{a * 1000:.0f} ms ago"
    if a < 120:
        return f"{a:.0f} s ago"
    return f"{a / 60:.0f} min ago"


class App:
    CARD_W, CANVAS = 250, 150

    def __init__(self, root, server, config_path=CONFIG_PATH):
        self.root, self.server = root, server
        self.config_path = Path(config_path)
        root.title("Slime View")
        root.configure(bg="#1e1e1e")
        top = tk.Frame(root, bg="#1e1e1e")
        top.pack(fill="x", padx=8, pady=6)
        self.header = tk.Label(top, text="", fg="#ddd", bg="#1e1e1e", anchor="w", font=("Segoe UI", 10))
        self.header.pack(side="left", fill="x", expand=True)
        tk.Button(top, text="Forget not seen", command=server.forget_stale).pack(side="right")
        self.skeleton = Skeleton()
        self.role_vars = {}
        self.tracker_keys = {}
        self.calibration_timer = None
        body = tk.Frame(root, bg='#1e1e1e')
        body.pack(fill='both', expand=True)
        preview = tk.Frame(body, bg='#1e1e1e', width=360)
        preview.pack(side='left', fill='y', padx=8, pady=4)
        tk.Label(preview, text='Skeleton', fg='#fff', bg='#1e1e1e', font=('Segoe UI', 14, 'bold')).pack()
        self.calibrate_button = tk.Button(preview, text='Full Reset', command=self.start_calibration)
        self.calibrate_button.pack(fill='x')
        self.mounting_button = tk.Button(preview, text='Mount Reset',
                                        command=lambda: self.start_calibration(5, mounting=True), state='disabled')
        self.mounting_button.pack(fill='x', pady=(4, 0))
        self.skeleton_status = tk.Label(preview, text='',
                                        fg='#ccc', bg='#1e1e1e', wraplength=340, height=3)
        self.skeleton_status.pack(fill='x')
        self.config_status = tk.Label(preview, text='', fg='#999', bg='#1e1e1e', wraplength=340)
        self.config_status.pack(fill='x', pady=(0, 6))
        try:
            self.skeleton.load(self.config_path)
        except FileNotFoundError:
            pass
        except (OSError, ValueError, OverflowError) as e:
            self.config_status.config(text=f'Could not load config: {e}', fg='#ffac66')
        else:
            self.config_status.config(text=f'Loaded {self.config_path.name}')
            self.skeleton_status.config(text=('Calibration loaded.' if self.skeleton.mounting else
                                             'Full reset loaded.' if self.skeleton.reference else ''))
        self.mounting_button.config(state='normal' if self.skeleton.reference else 'disabled')
        self.skeleton_canvas = tk.Canvas(preview, width=350, height=430, bg='#151515', highlightthickness=0)
        self.skeleton_canvas.pack(fill='both', expand=True)
        # Scroll the cards so all assignments remain accessible on smaller screens.
        card_area = tk.Frame(body, bg='#1e1e1e')
        card_area.pack(side='left', fill='both', expand=True)
        self.card_canvas = tk.Canvas(card_area, bg='#1e1e1e', highlightthickness=0)
        scrollbar = ttk.Scrollbar(card_area, orient='vertical', command=self.card_canvas.yview)
        scrollbar.pack(side='right', fill='y')
        self.card_canvas.pack(side='left', fill='both', expand=True)
        self.card_canvas.configure(yscrollcommand=scrollbar.set)
        self.grid = tk.Frame(self.card_canvas, bg='#1e1e1e')
        window = self.card_canvas.create_window(0, 0, window=self.grid, anchor='nw')
        self.card_canvas.bind('<Configure>', lambda e: self.card_canvas.itemconfigure(window, width=e.width))
        self.grid.bind('<Configure>', lambda e: self.card_canvas.configure(scrollregion=self.card_canvas.bbox('all')))
        self.cards = {}
        self.tick()

    def assign_role(self, key):
        self.skeleton.assign(self.tracker_keys.get(key, key), self.role_vars[key].get())
        self.sync_roles()
        self.cancel_calibration()
        self.skeleton_status.config(text='Assignments changed.')
        self.save_config()

    def sync_roles(self):
        for addr, var in self.role_vars.items():
            identity = self.tracker_keys.get(addr, addr)
            var.set(next((p for p, k in self.skeleton.assignments.items() if k == identity), 'Unassigned'))

    def save_config(self):
        try:
            self.skeleton.save(self.config_path)
        except (OSError, ValueError) as e:
            self.config_status.config(text=f'Could not save config: {e}', fg='#ffac66')
        else:
            self.config_status.config(text=f'Saved {self.config_path.name}', fg='#999')

    def cancel_calibration(self):
        if self.calibration_timer is not None:
            self.root.after_cancel(self.calibration_timer)
            self.calibration_timer = None
        self.calibrate_button.config(state='normal')
        self.mounting_button.config(state='normal' if self.skeleton.reference else 'disabled')

    def start_calibration(self, remaining=3, mounting=False):
        self.calibration_timer = None
        if remaining:
            self.calibrate_button.config(state='disabled')
            self.mounting_button.config(state='disabled')
            instruction = 'Mount Reset' if mounting else 'Full Reset'
            self.skeleton_status.config(text=f'{instruction}... {remaining}')
            self.calibration_timer = self.root.after(1000, lambda: self.start_calibration(remaining - 1, mounting))
        else:
            calibrate = self.skeleton.calibrate_mounting if mounting else self.skeleton.calibrate
            message = calibrate(self.server.snapshot(), time.time())
            self.skeleton_status.config(text=message)
            self.cancel_calibration()
            if not message.startswith('Waiting'):
                self.save_config()

    def card(self, key):
        if key not in self.cards:
            f = tk.Frame(self.grid, bg="#2a2a2a", padx=6, pady=6)
            title = tk.Label(f, fg="#fff", bg="#2a2a2a", font=("Segoe UI", 11, "bold"), anchor="w")
            title.pack(fill="x")
            identity = self.tracker_keys.get(key, key)
            assigned = next((p for p, k in self.skeleton.assignments.items() if k == identity), 'Unassigned')
            role = self.role_vars[key] = tk.StringVar(value=assigned)
            selection = ttk.Combobox(f, textvariable=role, values=('Unassigned', *BODY_PARTS), state='readonly')
            selection.pack(fill='x', pady=(5, 0))
            selection.bind('<<ComboboxSelected>>', lambda e, k=key: self.assign_role(k))
            c = tk.Canvas(f, width=self.CANVAS, height=self.CANVAS, bg="#151515", highlightthickness=0)
            c.pack(pady=4)
            info = tk.Label(f, fg="#ccc", bg="#2a2a2a", font=("Consolas", 9), justify="left", anchor="w")
            info.pack(fill="x")
            self.cards[key] = (f, title, c, info)
        return self.cards[key]

    def tick(self):
        snap = sorted(self.server.snapshot(), key=lambda t: (t["addr"][0], t["id"] if t["id"] is not None else 99))
        self.tracker_keys = {t['addr']: tracker_key(t) for t in snap}
        if self.skeleton.migrate_assignments(snap):
            self.save_config()
        keys = {t["addr"] for t in snap}
        for k in [k for k in self.cards if k not in keys]:
            self.cards.pop(k)[0].destroy()
            self.role_vars.pop(k)
            # Keep saved assignments when a tracker disappears or is forgotten.
        self.sync_roles()
        cols = max(1, self.card_canvas.winfo_width() // (self.CARD_W + 8))
        seen = 0
        for n, t in enumerate(snap):
            f, title, c, info = self.card(t["addr"])
            f.grid(row=n // cols, column=n % cols, padx=4, pady=4, sticky="n")
            fresh = t["last_real"] is not None and time.time() - t["last_real"] < STALE_S
            seen += fresh
            name = f"ID {t['id']}" if t["id"] is not None else f"port {t['addr'][1]}"
            title.config(text=f"{name}  {t['mac'] or '?'}", fg="#fff" if fresh else "#888")
            draw_tracker(c, t["q"], self.CANVAS, not fresh)
            lines = [
                f"seen    {age_text(t['last_real'])}",
                f"status  {STATUS.get(t['status'], '?') if t['status'] is not None else '?'}"
                + ("" if fresh else "  (not seen)"),
                "battery " + (f"{t['batt_pct']:.0f}%" if t["batt_pct"] is not None else "?")
                + (f"  {t['batt_v']:.2f} V" if t["batt_v"] is not None else ""),
                "signal  " + (f"{t['rssi']} dBm" if t["rssi"] is not None else "?")
                + ("   temp " + f"{t['temp']:.1f} C" if t["temp"] is not None else ""),
                f"rate    {t['real_rate']}/s real, {t['held_rate']}/s held",
            ]
            if t["q"] is not None:
                r, p, y = euler_deg(t["q"])
                lines.append(f"XYZ     {r:6.1f} {p:6.1f} {y:6.1f} deg")
            if t["fw"]:
                lines.append(f"fw      {t['fw']}")
            if isinstance(self.server, USBServer):
                receivers = sorted({s['addr'][0] for s in snap})
                lines.append(f"source  USB receiver {receivers.index(t['addr'][0]) + 1}")
            else:
                lines.append(f"hub     {t['addr'][0]}")
            info.config(text="\n".join(lines), fg="#ccc" if fresh else "#777")
        err = f"   last error: {self.server.error}" if self.server.error else ""
        self.header.config(text=f"{self.server.description} | {len(snap)} tracker(s), {seen} seen{err}")
        segments, live = self.skeleton.pose(snap, time.time())
        draw_skeleton(self.skeleton_canvas, segments, live)
        self.root.after(50, self.tick)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=6969)
    ap.add_argument("--usb", action="store_true", help="Read nRF receivers over USB-HID, without Wi-Fi")
    ap.add_argument('--config', type=Path, default=CONFIG_PATH, help='JSON assignments and calibration file')
    args = ap.parse_args()
    try:
        server = USBServer() if args.usb else Server(args.port)
    except RuntimeError as e:
        raise SystemExit(str(e))
    except OSError as e:
        raise SystemExit(f"Cannot open UDP {args.port} ({e}). Is SlimeVR Server still running?")
    try:
        root = tk.Tk()
        root.geometry("1180x820")
        App(root, server, args.config)
        root.mainloop()
    finally:
        server.close()


if __name__ == "__main__":
    main()
