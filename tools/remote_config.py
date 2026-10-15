#!/usr/bin/env python3
"""Read and change a tracker's settings over the radio, through the nRF52840 Dongle (docs/OTA.md).

Needs this repository's receiver (make receiver) and trackers built with settings over the radio
(CONFIG_TRACKER_REMOTE_CONFIG, on by default; with or without MCUboot). The tracker must be on
and paired; no USB cable to it.

    python remote_config.py --tracker 3 list
    python remote_config.py --tracker 3 get sensor_update_time_ms
    python remote_config.py --tracker 3 set sensor_update_time_ms 10
    python remote_config.py --tracker 3 reset radio_tx_power
    python remote_config.py --tracker 3 set 29 10 --port COM29     # setting by id

A tracker in BLE mode (docs/BLE.md) is reached through tools/ble_bridge.py instead, which must be
running; name it by its BLE name without SlimeNRF-:

    python remote_config.py --ble 0791 get sensor_update_time_ms
    python remote_config.py --ble 0791 set connection_over_ble 0    # back to ESB (reboots)
    python remote_config.py --tracker 3 set connection_over_ble 1   # to BLE (reboots)

A setting is the same as on the tracker's console (list_config, write_config). Some take effect
at once (sensor_update_time_ms), others on the tracker's next start (radio_tx_power, sensor
rates); connection_over_ble restarts the tracker in the other mode. A setting that cuts the
tracker off (e.g. a very low radio_tx_power) has to be undone over USB.

Needs pyserial for the receiver: python -m pip install pyserial
"""
import argparse
import random
import re
import socket
import struct
import sys
import time

RECEIVER_VID, RECEIVER_PID = 0x1209, 0x7690
TIMEOUT_S = 6  # the receiver gives up after 3 s

# Tracker/src/config.c config_settings_names, in order: the index is the setting id. Keep in step
# with it (settings are only ever appended, so ids stay the same).
SETTINGS = [
    # 0: device, on/off
    'user_extra_actions', 'ignore_reset', 'user_shutdown', 'use_imu_wake_up', 'delay_sleep_on_status',
    'connection_over_hid',
    # 1: sensor, on/off
    'sensor_use_low_power_2', 'use_imu_timeout', 'use_active_timeout', 'sensor_use_mag', 'use_sensor_clock',
    'sensor_use_6_side_calibration',
    # 2: int16
    'led_default_color_r', 'led_default_color_g', 'led_default_color_b', 'active_timeout_mode',
    'sensor_accel_odr', 'sensor_gyro_odr', 'sensor_accel_fs', 'sensor_gyro_fs', 'sensor_fusion', 'radio_tx_power',
    # 3: int32
    'connection_timeout_delay', 'sensor_lp_timeout', 'imu_timeout_ramp_min', 'imu_timeout_ramp_max',
    'active_timeout_threshold', 'active_timeout_delay', 'battery_low_runtime_threshold',
    'sensor_update_time_ms', 'connection_over_ble', 'sensor_send_unchanged',
]

RESULT = re.compile(r'cfg: (?:idle, last: )?tracker (\d+) setting (\d+)(?: = (-?\d+)|: (.+))$')
FAILED = re.compile(r'cfg: (?:idle, last: )?tracker (\d+):? (did not answer.*|stopped.*)$')


def setting_id(name):
    if name.isdigit():
        return int(name)
    if name not in SETTINGS:
        sys.exit(f'Unknown setting {name!r}; one of: ' + ', '.join(SETTINGS))
    return SETTINGS.index(name)


def setting_name(i):
    return SETTINGS[i] if i < len(SETTINGS) else f'setting {i}'


class Console:
    def __init__(self, port):
        import serial
        self.ser = serial.Serial(port, 115200, timeout=0, write_timeout=2)
        self.ser.dtr = True
        self.buf = b''
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, line):
        self.ser.write((line + '\n').encode())

    def lines(self):
        self.buf += self.ser.read(self.ser.in_waiting or 0)
        *done, self.buf = self.buf.split(b'\n')
        for raw in done:
            text = re.sub(r'\x1b\[[0-9;]*m', '', raw.decode(errors='replace')).strip()
            if text.startswith('Unknown command'):
                raise RuntimeError('the receiver has no "cfg" command: flash this repository\'s receiver')
            i = text.rfind('cfg: ')  # log lines carry the module name too: '<inf> remote_config: cfg: ...'
            if i >= 0:
                yield text[i:]

    def request(self, tracker, op, sid, value=None):
        """(value, None) or (None, reason)."""
        self.send(f'cfg {tracker} {op} {sid}' + (f' {value}' if value is not None else ''))
        end = time.monotonic() + TIMEOUT_S
        next_poll = time.monotonic() + 1
        while time.monotonic() < end:
            for line in self.lines():
                if line.startswith('cfg: error'):
                    return None, line[5:]
                m = RESULT.match(line)
                if m and int(m[1]) == tracker and int(m[2]) == sid:
                    return (int(m[3]), None) if m[3] is not None else (None, m[4])
                m = FAILED.match(line)
                if m and int(m[1]) == tracker:
                    return None, m[2]
            if time.monotonic() >= next_poll:  # the result line may have been cut by an echo
                self.send('cfg status')
                next_poll = time.monotonic() + 1
            time.sleep(0.01)
        return None, 'no result from the receiver'


# Tracker/src/ota_proto.h CFG_MSG_*: [type][tracker id][seq][op|status][setting id u16][value i32]
CFG_MSG_REQ, CFG_MSG_RESP = 0xF6, 0xF7
CFG_MSG = struct.Struct('<BBBBHi')
CFG_OPS = {'get': 0, 'set': 1, 'reset': 2}
CFG_STATUS = ['ok', 'no such setting', 'value out of range', 'unknown operation', 'busy with a firmware update']
CFG_BUSY = 4


def cfg_request(seq, op, sid, value=0):
    return CFG_MSG.pack(CFG_MSG_REQ, 0, seq & 0xff, CFG_OPS[op], sid, value)


def cfg_answer(data):
    """(seq, status, setting id, value) of a tracker's answer, or None."""
    if len(data) != CFG_MSG.size or data[0] != CFG_MSG_RESP:
        return None
    _, _, seq, status, sid, value = CFG_MSG.unpack(data)
    return seq, status, sid, value


class BleBridge:
    """A tracker in BLE mode, through ble_bridge.py's control port."""

    def __init__(self, name, port, attempt_s=0.7):
        import ble_bridge
        self.bb = ble_bridge
        self.name = name.upper().removeprefix('SLIMENRF-')
        self.addr = ('127.0.0.1', port)
        self.attempt_s = attempt_s
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.seq = random.randrange(256)

    def request(self, tracker, op, sid, value=None):
        """(value, None) or (None, reason). The same seq is sent again until answered: the tracker
        carries a request out once and answers repeats from its last answer."""
        self.seq = (self.seq + 1) & 0xff
        message = self.bb.control_pack(self.name, cfg_request(self.seq, op, sid, value or 0))
        heard = False
        end = time.monotonic() + TIMEOUT_S
        while time.monotonic() < end:
            self.sock.sendto(message, self.addr)
            attempt_end = min(end, time.monotonic() + self.attempt_s)
            while time.monotonic() < attempt_end:
                self.sock.settimeout(max(0.01, attempt_end - time.monotonic()))
                try:
                    data = self.sock.recv(64)
                except socket.timeout:
                    break
                except ConnectionResetError:  # Windows: nothing listens on the control port
                    return None, 'ble_bridge.py is not running'
                try:
                    name, payload = self.bb.control_unpack(data)
                except ValueError:
                    continue
                if name != self.name:
                    continue
                heard = True
                if not payload:
                    return None, f'SlimeNRF-{self.name} is not connected to ble_bridge.py'
                answer = cfg_answer(payload)
                if not answer or answer[0] != self.seq or answer[2] != sid:
                    continue
                status, value = answer[1], answer[3]
                if status == CFG_BUSY:
                    break  # ask again
                if status:
                    return None, CFG_STATUS[status] if status < len(CFG_STATUS) else f'status {status}'
                return value, None
        return None, 'did not answer' if heard else 'no answer from ble_bridge.py (is it running?)'


def find_port(port):
    if port:
        return port
    from serial.tools import list_ports
    # Not the ESP32-S3 BLE receiver (same USB id, serial 5333...): it has no cfg command
    ports = [p.device for p in list_ports.comports() if p.vid == RECEIVER_VID and p.pid == RECEIVER_PID
             and not (p.serial_number or '').startswith('5333')]
    if not ports:
        sys.exit('No receiver found on USB (1209:7690); give --port')
    if len(ports) > 1:
        sys.exit('More than one receiver on USB: give --port')
    return ports[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    who = ap.add_mutually_exclusive_group(required=True)
    who.add_argument('--tracker', type=int, help='tracker id (the receiver\'s "list" order)')
    who.add_argument('--ble', metavar='NAME', help='tracker in BLE mode, e.g. 0791 for SlimeNRF-0791 (via ble_bridge.py)')
    ap.add_argument('--port', help='receiver console serial port')
    ap.add_argument('--bridge-port', type=int, default=6971, help='ble_bridge.py control port')
    ap.add_argument('command', choices=('list', 'get', 'set', 'reset'))
    ap.add_argument('setting', nargs='?', help='setting name or id (list_config)')
    ap.add_argument('value', nargs='?', type=int)
    args = ap.parse_args(argv)
    if args.command != 'list' and args.setting is None:
        ap.error(f'{args.command} needs a setting')
    if args.command == 'set' and args.value is None:
        ap.error('set needs a value')
    if args.ble:
        con = BleBridge(args.ble, args.bridge_port)
        label = f'SlimeNRF-{con.name}'
    else:
        try:
            import serial  # noqa: F401
        except ImportError:
            sys.exit('Needs pyserial: python -m pip install pyserial')
        con = Console(find_port(args.port))
        label = f'tracker {args.tracker}'
    if args.command == 'list':
        failed = 0
        for sid, name in enumerate(SETTINGS):
            value, err = con.request(args.tracker, 'get', sid)
            print(f'{sid:3} {name:32} {value if err is None else "(" + err + ")"}')
            if err and err not in CFG_STATUS:  # the link failed, not this setting: stop
                sys.exit(1)
            failed += err is not None
        sys.exit(1 if failed else 0)
    sid = setting_id(args.setting)
    value, err = con.request(args.tracker, args.command, sid, args.value if args.command == 'set' else None)
    if err:
        sys.exit(f'{label} {setting_name(sid)}: {err}')
    print(f'{label} {setting_name(sid)} = {value}')
    if setting_name(sid) == 'connection_over_ble' and args.command != 'get':
        print(f'{label} restarts in {"BLE" if value else "ESB"} mode if it was not in it')


if __name__ == '__main__':
    main()
