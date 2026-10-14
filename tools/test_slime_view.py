import types
import unittest

import slime_view as sv

DONGLE = 'F9F258A5111356F3'


class FakePort:
    def __init__(self, device, serial_number, vid=0x1209, pid=0x7690):
        self.device, self.serial_number, self.vid, self.pid = device, serial_number, vid, pid


class FakeSerial:
    """A receiver console: echoes the command, logs a line, then answers."""
    written = []
    answers = {}

    def __init__(self, port, baud, timeout):
        self.port, self.out = port, b''

    def read(self, n):
        out, self.out = self.out, b''
        return out

    def write(self, data):
        FakeSerial.written.append((self.port, data))
        cmd = data.decode().strip()
        self.out = (cmd + '\r\n' + '\x1b[1;33m<wrn> esb_event: something\x1b[0m\r\n'
                    + FakeSerial.answers.get(cmd, 'Unknown command') + '\r\n').encode()

    def close(self):
        pass


def fake_serial_module(ports):
    return types.SimpleNamespace(Serial=FakeSerial,
                                 list_ports=types.SimpleNamespace(comports=lambda: ports))


class ReceiverCommandTests(unittest.TestCase):
    def setUp(self):
        FakeSerial.written = []
        FakeSerial.answers = {'pair': 'Pairing on', 'unpair 3': 'Unpaired id 3'}
        self.ports = [FakePort('COM42', '53331020BA053EB0'), FakePort('COM29', DONGLE),
                      FakePort('COM16', '823B25D6352D1CF7', pid=0x7692)]
        self.module = fake_serial_module(self.ports)

    def run_command(self, command, replies):
        return sv.receiver_command(DONGLE, command, replies, timeout=0.5, serial_module=self.module)

    def test_answer_through_the_receivers_console(self):
        self.assertEqual(self.run_command('pair', ('Pairing on', 'Pairing off')), 'Pairing on')
        self.assertEqual(FakeSerial.written, [('COM29', b'pair\r\n')])  # found by serial number

    def test_unpair(self):
        self.assertEqual(self.run_command('unpair 3', ('Unpaired', 'Unpair:')), 'Unpaired id 3')

    def test_older_firmware(self):
        self.assertEqual(self.run_command('unpair 5', ('Unpaired', 'Unpair:')), 'Unknown command')

    def test_no_console(self):
        with self.assertRaises(OSError):
            sv.receiver_command('0000', 'pair', ('Pairing on',), serial_module=self.module)

    def test_pairable(self):
        self.assertTrue(sv.pairable(DONGLE))
        self.assertFalse(sv.pairable('53331020BA053EB0'))  # ESP32-S3 BLE receiver
        self.assertFalse(sv.pairable(None))


class FakeHid:
    def __init__(self):
        self.devices = []

    def enumerate(self, vid, pid):
        return self.devices

    def device(self):
        raise AssertionError('not opened in these tests')


class USBServerReceiverTests(unittest.TestCase):
    def test_trackers_know_their_receiver(self):
        server = sv.USBServer(FakeHid())
        try:
            server.serials['path1'] = DONGLE
            report = bytes((255, 3)) + bytes.fromhex('847BEBC50791')[::-1] + bytes(8) + bytes(48)
            server.handle(report, 'path1')
            (t,) = server.snapshot()
            self.assertEqual((t['id'], t['receiver'], t['mac']), (3, DONGLE, '847BEBC50791'))
            self.assertEqual(server.receivers(), [DONGLE])
            server.forget(('path1', 3))
            self.assertEqual(server.snapshot(), [])
        finally:
            server.close()


if __name__ == '__main__':
    unittest.main()
