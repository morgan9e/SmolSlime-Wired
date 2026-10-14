import socket
import struct
import threading
import unittest

import remote_config as rc
from ble_bridge import Link, control_pack, control_unpack, split, tracker_mac, wanted

MAC = bytes.fromhex("847bebc50791")


def info():
    p = bytearray(16)
    p[:7] = bytes((0, 0, 80, 155, 128, 4, 0))
    p[8] = 13
    return bytes(p)


def quat():
    p = bytearray(16)
    p[0] = 1
    struct.pack_into("<4h", p, 2, 0, 0, 0, 32767)
    return bytes(p)


def notification(p, seq):
    return p + bytes((seq,))


class FakeSocket:
    def close(self):
        pass


class FakeDevice:
    def __init__(self, mac, info, server):
        self.mac, self.info, self.sock = mac, info, FakeSocket()
        self.records = []

    def record(self, p, now):
        self.records.append(p)

    def tick(self, now):
        pass


class PacketTests(unittest.TestCase):
    def test_split(self):
        self.assertEqual(split(notification(info(), 7)), (info(), 7))
        for data in (b"", info(), notification(info(), 1) + b"x", bytes((9,)) + bytes(16)):
            with self.assertRaises(ValueError):
                split(data)

    def test_mac_is_reversed_device_address(self):
        # The tracker sends FICR DEVICEADDR little endian; SlimeVR gets it as printed (847BEBC50791)
        self.assertEqual(tracker_mac(MAC[::-1]), MAC)
        with self.assertRaises(ValueError):
            tracker_mac(bytes(5))

    def test_wanted(self):
        self.assertTrue(wanted("SlimeNRF-0791", set()))
        self.assertTrue(wanted("SlimeNRF-0791", {"0791"}))
        self.assertFalse(wanted("SlimeNRF-0791", {"B8C8"}))
        self.assertFalse(wanted("Other", set()))
        self.assertFalse(wanted(None, set()))


class LinkTests(unittest.TestCase):
    def test_waits_for_device_info(self):
        link = Link(MAC, None, FakeDevice)
        link.receive(notification(quat(), 0), 0)
        self.assertIsNone(link.device)
        link.receive(notification(info(), 1), 0)
        link.receive(notification(quat(), 2), 0)
        self.assertEqual(link.device.records, [info(), quat()])
        self.assertEqual(link.device.mac, MAC)

    def test_sequence(self):
        link = Link(MAC, None, FakeDevice)
        link.receive(notification(info(), 254), 0)
        link.receive(notification(quat(), 255), 0)
        link.receive(notification(quat(), 255), 0)  # repeated: dropped
        link.receive(notification(quat(), 2), 0)  # wraps; 0 and 1 lost
        self.assertEqual((link.packets, link.lost), (3, 2))
        self.assertEqual(len(link.device.records), 3)
        link.disconnected()
        link.receive(notification(quat(), 100), 0)  # a new connection: no loss counted
        self.assertEqual(link.lost, 2)

    def test_rejects_garbage(self):
        link = Link(MAC, None, FakeDevice)
        link.receive(b"\x00" * 5, 0)
        self.assertEqual((link.rejected, link.packets), (1, 0))

    def test_slimevr_handshake(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        server.bind(("127.0.0.1", 0))
        server.settimeout(2)
        link = Link(MAC, server.getsockname())
        try:
            link.receive(notification(info(), 0), 10.0)
            data = server.recv(2048)
            self.assertEqual(struct.unpack_from(">I", data)[0], 3)  # handshake
            self.assertTrue(data.endswith(MAC))
        finally:
            link.close()
            server.close()


class ControlTests(unittest.TestCase):
    def test_framing(self):
        req = rc.cfg_request(7, "set", 29, 10)
        self.assertEqual(control_unpack(control_pack("0791", req)), ("0791", req))
        self.assertEqual(control_unpack(control_pack("b8c8")), ("B8C8", b""))
        for data in (b"", b"SNCF", b"XXXX" + bytes(8), control_pack("0791") + b"x"):
            with self.assertRaises(ValueError):
                control_unpack(data)

    def test_cfg_messages(self):
        req = rc.cfg_request(300, "reset", 30)
        self.assertEqual(req, bytes((0xF6, 0, 44, 2, 30, 0, 0, 0, 0, 0)))
        answer = struct.pack("<BBBBHi", 0xF7, 0, 44, 0, 30, -4)
        self.assertEqual(rc.cfg_answer(answer), (44, 0, 30, -4))
        self.assertIsNone(rc.cfg_answer(req))


class FakeBridge(threading.Thread):
    """ble_bridge.py's control port, answering for SlimeNRF-0791 like a tracker would."""

    def __init__(self, answers):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(3)
        self.port = self.sock.getsockname()[1]
        self.answers = answers  # per request: None (stay silent), "busy", "nc" or a value
        self.requests = []

    def run(self):
        for answer in self.answers:
            try:
                data, peer = self.sock.recvfrom(64)
            except socket.timeout:
                return
            name, req = control_unpack(data)
            self.requests.append(req)
            if answer is None:
                continue
            if answer == "nc":
                self.sock.sendto(control_pack(name), peer)
                continue
            _, _, seq, op, sid, _ = struct.unpack("<BBBBHi", req)
            status, value = (4, 0) if answer == "busy" else (0, answer)
            self.sock.sendto(control_pack(name, struct.pack("<BBBBHi", 0xF7, 0, seq, status, sid, value)), peer)


class BleRemoteConfigTests(unittest.TestCase):
    def run_bridge(self, answers, op="get", value=None):
        bridge = FakeBridge(answers)
        bridge.start()
        con = rc.BleBridge("SlimeNRF-0791", bridge.port, attempt_s=0.2)
        try:
            return con.request(None, op, 29, value), bridge.requests
        finally:
            con.sock.close()
            bridge.join(3)
            bridge.sock.close()

    def test_answer(self):
        (result, requests) = self.run_bridge([10], "set", 10)
        self.assertEqual(result, (10, None))
        self.assertEqual(struct.unpack("<BBBBHi", requests[0])[3:], (1, 29, 10))

    def test_retries_same_seq(self):
        (result, requests) = self.run_bridge([None, "busy", 12])
        self.assertEqual(result, (12, None))
        self.assertEqual(len({r[2] for r in requests}), 1)  # one seq: carried out once
        self.assertEqual(len(requests), 3)

    def test_not_connected(self):
        (result, _) = self.run_bridge(["nc"])
        self.assertEqual(result, (None, "SlimeNRF-0791 is not connected to ble_bridge.py"))


if __name__ == "__main__":
    unittest.main()
