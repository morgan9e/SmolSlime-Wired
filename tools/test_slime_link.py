"""Tests for slime_link.py with synthetic receiver HID reports: python -m unittest test_slime_link"""
import unittest

from slime_link import Monitor


def packet(kind, tid, rssi=0, batt=None, addr=None):
    p = bytearray(16)
    p[0], p[1] = kind, tid
    if addr is not None:
        p[2:8] = bytes.fromhex(addr)
    if batt is not None:
        p[2], p[3] = batt, 125  # 125 + 245 = 3.70 V
    if kind not in (1, 4, 255):
        p[15] = rssi
    return bytes(p)


def report(*packets):
    data = b''.join(packets)
    return data + bytes(64 - len(data))


class MonitorTest(unittest.TestCase):
    def test_rate_rssi_battery_and_address(self):
        m = Monitor()
        m.report(0, report(packet(255, 3, addr='DEE876FA4759')), 0.0)
        for i in range(50):
            m.report(0, report(packet(2, 3, rssi=60 + i % 10, batt=80)), i * 0.02)
        m.tick(1.0)
        link = m.links[(0, 3)]
        self.assertEqual(link.addr, 'DEE876FA4759')
        self.assertEqual(link.rate, 50)
        self.assertEqual(link.rssi_min, -69)
        self.assertAlmostEqual(link.rssi_avg, -64.5)
        self.assertEqual((link.batt_pct, link.batt_v), (80, 3.70))
        self.assertEqual(link.gaps, 0)

    def test_rotation_packets_do_not_carry_rssi(self):
        m = Monitor()
        m.report(0, report(packet(1, 0), packet(4, 0)), 0.0)
        self.assertIsNone(m.links[(0, 0)].rssi)
        self.assertEqual(m.links[(0, 0)].count, 2)

    def test_gaps_and_long_gaps(self):
        m = Monitor(gap=0.1, offline_s=2)
        for t in (0.0, 0.05, 0.3, 0.32, 1.0, 1.01):
            m.report(0, report(packet(1, 1)), t)
        link = m.links[(0, 1)]
        self.assertEqual(link.gaps, 2)          # 250 ms and 680 ms
        self.assertEqual(link.long_gaps, 1)     # 680 ms
        self.assertAlmostEqual(link.max_gap, 0.68)

    def test_offline_counted_once_not_as_gap(self):
        m = Monitor(gap=0.1, offline_s=2)
        m.report(0, report(packet(1, 2)), 0.0)
        self.assertEqual(m.tick(1.0), [])
        went = m.tick(2.5)
        self.assertEqual([l.id for l in went], [2])
        self.assertEqual(m.tick(3.5), [])       # still off: not counted again
        m.report(0, report(packet(1, 2)), 5.0)  # back after 5 s
        link = m.links[(0, 2)]
        self.assertEqual((link.offline, link.gaps, link.is_off), (1, 0, False))

    def test_min_rate_ignores_start_and_offline_seconds(self):
        m = Monitor(offline_s=2)
        m.report(0, report(packet(1, 5)), 0.5)
        m.tick(1.0)                             # first second: not counted
        for i in range(10):
            m.report(0, report(packet(1, 5)), 1.0 + i * 0.1)
        m.tick(2.0)
        self.assertEqual(m.links[(0, 5)].min_rate, 10)
        m.tick(5.0)                             # offline: not a low second
        self.assertEqual(m.links[(0, 5)].min_rate, 10)

    def test_leading_report_id_and_receivers_kept_apart(self):
        m = Monitor()
        m.report(0, b'\x00' + report(packet(1, 0)), 0.0)
        m.report(1, report(packet(1, 0)), 0.0)
        self.assertEqual(sorted(m.links), [(0, 0), (1, 0)])


if __name__ == '__main__':
    unittest.main()
