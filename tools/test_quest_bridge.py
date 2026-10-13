import math
import socket
import struct
import unittest

from quest_bridge import Bridge, Device, HELLO, REPORT, envelope, motion, unpack


MAC = bytes.fromhex("628dcfc6318a")
PEER = ("192.0.2.1", 4567)


def address(mac=MAC, tracker=5):
    return bytes((255, tracker)) + mac[::-1] + bytes(8)


def info(tracker=5):
    p = bytearray(16)
    p[:7] = bytes((0, tracker, 80, 155, 128, 4, 0))
    p[8], p[15] = 13, 55
    return bytes(p)


def report(*records):
    return b"".join(records).ljust(64, b"\xff")


class FakeSocket:
    def close(self):
        pass


class FakeDevice:
    def __init__(self, mac, info, server):
        self.mac, self.info, self.sock = mac, info, FakeSocket()
        self.records = []

    def record(self, p, now):
        self.records.append(p)


class ProtocolTests(unittest.TestCase):
    def test_envelope_validation(self):
        valid = envelope(REPORT, 100, 0xffffffff, report(info()))
        self.assertEqual(unpack(valid)[2:4], (100, 0xffffffff))
        for data in (b"", valid[:-1], valid + b"x", b"BAD!" + valid[4:], envelope(REPORT, 1, 0, bytes(16))):
            with self.assertRaises(ValueError):
                unpack(data)

    def test_type7_offsets(self):
        p = bytearray(16)
        p[0], p[1], p[2] = 7, 5, 3
        struct.pack_into("<H", p, 3, 12345)
        struct.pack_into("<I", p, 5, 512 | (1024 << 10) | (1024 << 21))
        struct.pack_into("<3h", p, 9, 128, -256, 512)
        q, a = motion(p)
        self.assertEqual(q, (0, 0, 0, 1))
        self.assertEqual(a, (1, -2, 4))

    def test_q15_and_type4(self):
        p = bytearray(16)
        p[0] = 1
        struct.pack_into("<4h", p, 2, 0, 0, 23170, 23170)
        struct.pack_into("<3h", p, 10, 128, -128, 1256)
        q, a = motion(p)
        self.assertAlmostEqual(q[2], math.sqrt(0.5))
        self.assertEqual(a[:2], (1, -1))
        p[0] = 4
        self.assertIsNone(motion(p)[1])
        p[2:10] = bytes(8)
        self.assertEqual(motion(p), (None, None))

    def test_registration_after_data_and_unknown_type(self):
        b = Bridge(("127.0.0.1", 6969), factory=FakeDevice)
        b.receive(envelope(HELLO, 1, 0, b"receiver"), PEER, 0)
        b.receive(envelope(REPORT, 1, 1, report(info(), address())), PEER, 0.1)
        self.assertIn(MAC, b.devices)
        self.assertEqual(b.devices[MAC].info[8], 13)
        self.assertEqual(len(b.devices[MAC].records), 1)

    def test_restart_wrap_duplicates_and_disconnect(self):
        b = Bridge(("127.0.0.1", 6969), factory=FakeDevice)
        b.receive(envelope(HELLO, 1, 0xfffffffe, b"rx"), PEER, 0)
        data = report(info(), address())
        b.receive(envelope(REPORT, 1, 0, data), PEER, 0.1)
        self.assertEqual(b.lost, 1)
        b.receive(envelope(REPORT, 1, 0, data), PEER, 0.2)
        self.assertEqual(b.rejected, 1)
        b.receive(envelope(HELLO, 2, 0, b"rx"), PEER, 0.3)
        self.assertFalse(b.devices)
        b.receive(envelope(HELLO, 1, 1, b"rx"), PEER, 0.4)
        self.assertEqual(b.session, 2)
        b.receive(envelope(REPORT, 2, 1, data), PEER, 0.5)
        b.receive(envelope(HELLO, 2, 2, b"", False), PEER, 0.6)
        self.assertFalse(b.devices)

    def test_address_only_does_not_create_or_refresh_tracker(self):
        b = Bridge(("127.0.0.1", 6969), factory=FakeDevice)
        b.receive(envelope(HELLO, 1, 0, b"rx"), PEER, 0)
        b.receive(envelope(REPORT, 1, 1, report(address())), PEER, 1)
        self.assertFalse(b.devices)

    def test_id_reassignment_requires_fresh_info(self):
        b = Bridge(("127.0.0.1", 6969), factory=FakeDevice)
        b.receive(envelope(HELLO, 1, 0, b"rx"), PEER, 0)
        b.receive(envelope(REPORT, 1, 1, report(info(), address())), PEER, 0.1)
        other = bytes.fromhex("aabbccddeeff")
        b.receive(envelope(REPORT, 1, 2, report(address(other))), PEER, 0.2)
        self.assertFalse(b.devices)
        self.assertNotIn(5, b.infos)

    def test_peer_filter(self):
        b = Bridge(("127.0.0.1", 6969), quest_ip="192.0.2.2", factory=FakeDevice)
        b.receive(envelope(HELLO, 1, 0, b"rx"), PEER, 0)
        self.assertIsNone(b.peer)


class ServerIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.settimeout(0.05)
        self.device = Device(MAC, info(), self.server.getsockname())

    def tearDown(self):
        self.device.sock.close()
        self.server.close()

    def drain(self):
        result = []
        while True:
            try:
                result.append(self.server.recvfrom(2048))
            except socket.timeout:
                return result

    def test_handshake_motion_ping_and_silence(self):
        self.device.record(info(), 10)
        handshake, peer = self.server.recvfrom(2048)
        self.assertEqual(struct.unpack_from(">I", handshake)[0], 3)
        self.assertEqual(struct.unpack_from(">7i", handshake, 12), (4, 13, 0, 0, 0, 0, 22))
        length = handshake[40]
        self.assertEqual(handshake[41 + length - 1], 0)
        self.assertEqual(handshake[41 + length:], MAC)
        self.server.sendto(b"\x03Hey OVR =D 5".ljust(64, b"\0"), peer)
        self.device.tick(10.1)
        packets = self.drain()
        self.assertTrue(any(p[:4] == struct.pack(">I", 15) for p, _ in packets))
        p = bytearray(16); p[0], p[1] = 7, 5
        struct.pack_into("<I", p, 5, 512 | 1024 << 10 | 1024 << 21)
        struct.pack_into("<3h", p, 9, 128, -256, 512)
        self.device.record(p, 10.2)
        packets = [d for d, _ in self.drain()]
        self.assertEqual(struct.unpack_from(">BB4fB", packets[0], 12), (0, 1, 0, 0, 0, 1, 0))
        self.assertEqual(struct.unpack_from(">3fB", packets[1], 12), (1, -2, 4, 0))
        ping = struct.pack(">IQI", 10, 0, 12345)
        self.server.sendto(ping, peer)
        self.device.tick(10.3)
        pong, _ = self.server.recvfrom(2048)
        self.assertEqual(pong[12:], ping[12:])
        self.assertGreater(struct.unpack_from(">Q", pong, 4)[0], struct.unpack_from(">Q", packets[-1], 4)[0])
        self.server.sendto(ping, peer)
        self.device.tick(12)
        self.assertEqual(self.drain(), [])
        self.assertFalse(self.device.connected)

    def test_server_restart_retries_handshake(self):
        self.device.record(info(), 10)
        self.drain()
        self.device.record(info(), 11.1)
        packets = self.drain()
        self.assertTrue(any(p[:4] == struct.pack(">I", 3) for p, _ in packets))


if __name__ == "__main__":
    unittest.main()
