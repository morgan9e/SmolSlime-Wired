"""Quest raw-HID forwarder to stock SlimeVR UDP. Standard library only."""
import argparse
import logging
import math
import select
import socket
import struct
import time

LOG = logging.getLogger("quest")
HEADER = struct.Struct("<4sBBHQI")
REPORT, HELLO = 1, 2


def envelope(kind, session, sequence, payload, connected=True):
    return HEADER.pack(b"SNR2", kind, int(connected), len(payload), session, sequence) + payload


def unpack(data):
    if len(data) < HEADER.size:
        raise ValueError("short header")
    magic, kind, flags, size, session, seq = HEADER.unpack_from(data)
    payload = data[HEADER.size:]
    if (magic != b"SNR2" or flags not in (0, 1) or size != len(payload)
            or kind not in (REPORT, HELLO) or (kind == REPORT and (size != 64 or flags != 1))
            or (kind == HELLO and size > 128)):
        raise ValueError("invalid envelope")
    return kind, bool(flags), session, seq, payload


def motion(p):
    kind = p[0]
    if kind in (1, 4):
        q = tuple(v / 32768 for v in struct.unpack_from("<4h", p, 2))
    elif kind in (2, 7):
        bits, = struct.unpack_from("<I", p, 5)
        v = ((bits & 1023) / 512 - 1, ((bits >> 10) & 2047) / 1024 - 1,
             ((bits >> 21) & 2047) / 1024 - 1)
        d = sum(x * x for x in v)
        inv = 1 / math.sqrt(d + 1e-6)
        angle = math.pi / 2 * d * inv
        q = tuple(math.sin(angle) * inv * x for x in v) + (math.cos(angle),)
    else:
        return None, None
    norm = math.sqrt(sum(x * x for x in q))
    if not 0.5 < norm < 1.5:
        return None, None
    q = tuple(x / norm for x in q)
    a = None if kind == 4 else tuple(v / 128 for v in struct.unpack_from("<3h", p, 10 if kind == 1 else 9))
    return q, a


class Device:
    def __init__(self, mac, info, server):
        self.mac, self.info = mac, info
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.connect(server)
        self.sock.setblocking(False)
        self.number = 0
        self.seen = -math.inf
        self.last_handshake = self.last_sensor = self.last_reply = -math.inf
        self.connected = False
        self.rotations = 0

    def send(self, kind, payload=b""):
        self.number += 1
        try:
            self.sock.send(struct.pack(">IQ", kind, self.number) + payload)
        except OSError:
            self.connected = False

    def tick(self, now):
        active = now - self.seen < 1.0
        while True:
            try:
                data = self.sock.recv(2048)
            except (BlockingIOError, ConnectionResetError, ConnectionRefusedError):
                break
            if not active:
                continue
            if data.startswith(b"\x03Hey OVR =D 5"):
                self.connected = True
                self.last_reply = now
                self.last_sensor = -math.inf
            elif len(data) == 16 and data[:4] == struct.pack(">I", 10):
                # Echo the ping payload, with OUR monotonic packet number.
                self.send(10, data[12:])
                self.last_reply = now
            elif len(data) == 6 and data[:4] == struct.pack(">I", 15):
                self.last_reply = now
        if not active:
            self.connected = False
            return
        if now - self.last_reply > 3:
            self.connected = False
        if not self.connected and now - self.last_handshake >= 1:
            name = b"quest-bridge 0.1\0"
            self.send(3, struct.pack(">7iB", self.info[5], self.info[8], self.info[6],
                                    0, 0, 0, 22, len(name)) + name + self.mac)
            self.last_handshake = now
        if self.connected and now - self.last_sensor >= 1:
            self.send(15, bytes((0, 1, self.info[8])))
            self.last_sensor = now

    def record(self, p, now):
        self.seen = now
        if p[0] == 0:
            self.info = p
        self.tick(now)
        if not self.connected:
            return
        q, a = motion(p)
        if q is not None:
            self.send(17, struct.pack(">BB4fB", 0, 1, *q, 0))
            self.rotations += 1
        if a is not None:
            self.send(4, struct.pack(">3fB", *a, 0))
        if p[0] in (0, 2):
            if p[2] != 128:
                level = 1.0 if p[2] == 255 else min(100, p[2] & 127) / 100
                self.send(12, struct.pack(">ff", (p[3] + 245) / 100, level))
            if p[4]:
                self.send(20, struct.pack(">Bf", 0, p[4] / 2 - 39))
        if p[0] not in (1, 4) and 0 < p[15] < 127:
            self.send(19, struct.pack(">Bb", 0, -p[15]))


class Bridge:
    def __init__(self, server, quest_ip=None, factory=Device):
        self.server, self.quest_ip, self.factory = server, quest_ip, factory
        self.peer = self.session = self.sequence = None
        self.last_rx = -math.inf
        self.addresses, self.infos, self.devices = {}, {}, {}
        self.retired = set()
        self.reports = self.lost = self.rejected = 0

    def reset(self):
        for device in self.devices.values():
            device.sock.close()
        self.addresses.clear()
        self.infos.clear()
        self.devices.clear()

    def receive(self, data, peer, now):
        try:
            kind, connected, session, seq, payload = unpack(data)
        except ValueError:
            self.rejected += 1
            return
        if self.quest_ip and peer[0] != self.quest_ip:
            return
        key = (peer, session)
        if key != (self.peer, self.session):
            if kind != HELLO or key in self.retired:
                return
            if self.peer is not None and peer != self.peer and now - self.last_rx < 3:
                return
            if self.peer is not None:
                self.retired.add((self.peer, self.session))
            if len(self.retired) > 1024:
                # Bounded lifetime history; sessions are random 64-bit values.
                self.retired.pop()
            self.reset()
            self.peer, self.session, self.sequence = peer, session, None
            LOG.info("Quest %s receiver %s", peer[0], payload.decode("utf-8", "replace"))
        if self.sequence is not None:
            delta = (seq - self.sequence) & 0xffffffff
            if delta == 0 or delta >= 0x80000000:
                self.rejected += 1
                return
            self.lost += delta - 1
        self.sequence, self.last_rx = seq, now
        if not connected:
            self.reset()
            return
        if kind != REPORT:
            return
        self.reports += 1
        records = [payload[i:i + 16] for i in range(0, 64, 16)]
        # Registrations may follow data in the same USB report.
        for p in records:
            if p[0] == 255 and p[1] < 16 and p[2:8] not in (b"\0" * 6, b"\xff" * 6):
                mac = p[2:8][::-1]
                old = self.addresses.get(p[1])
                if old and old != mac:
                    self.infos.pop(p[1], None)
                    device = self.devices.pop(old, None)
                    if device:
                        device.sock.close()
                self.addresses[p[1]] = mac
        for p in records:
            if p[1] >= 16 or p[0] not in range(8):
                continue
            if p[0] == 0:
                self.infos[p[1]] = p
            mac, info = self.addresses.get(p[1]), self.infos.get(p[1])
            if mac is None or info is None:
                continue
            if mac not in self.devices:
                LOG.info("Tracker %s", mac.hex(":").upper())
                self.devices[mac] = self.factory(mac, info, self.server)
            self.devices[mac].record(p, now)

    def tick(self, now):
        for device in self.devices.values():
            device.tick(now)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen", default="0.0.0.0")
    parser.add_argument("--pc-port", type=int, default=6970)
    parser.add_argument("--quest-ip", help="Only accept this Quest IPv4 address")
    parser.add_argument("--server", default="127.0.0.1")
    parser.add_argument("--server-port", type=int, default=6969)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    bridge = Bridge((args.server, args.server_port), args.quest_ip)
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind((args.listen, args.pc_port))
    receiver.setblocking(False)
    LOG.info("Listening on %s:%d; SlimeVR %s:%d", args.listen, args.pc_port, args.server, args.server_port)
    report_at = time.monotonic()
    try:
        while True:
            ready, _, _ = select.select([receiver], [], [], 0.02)
            if ready:
                # Bound draining so server replies and timers are never starved.
                for _ in range(256):
                    try:
                        data, peer = receiver.recvfrom(2048)
                    except BlockingIOError:
                        break
                    bridge.receive(data, peer, time.monotonic())
            now = time.monotonic()
            bridge.tick(now)
            if now - report_at >= 5:
                rates = " ".join(f"{mac.hex()[-2:].upper()}:{d.rotations / (now - report_at):.0f}Hz"
                                 for mac, d in bridge.devices.items())
                LOG.info("Quest %s; reports=%d lost=%d rejected=%d %s",
                         "online" if now - bridge.last_rx < 3 else "offline",
                         bridge.reports, bridge.lost, bridge.rejected, rates)
                for device in bridge.devices.values():
                    device.rotations = 0
                report_at = now
    except KeyboardInterrupt:
        pass
    finally:
        bridge.reset()
        receiver.close()


if __name__ == "__main__":
    main()
