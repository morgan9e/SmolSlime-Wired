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

A setting is the same as on the tracker's console (list_config, write_config). Some take effect
at once (sensor_update_time_ms), others on the tracker's next start (radio_tx_power, sensor
rates). A setting that cuts the tracker off (e.g. a very low radio_tx_power) has to be undone over
USB.

Needs pyserial: python -m pip install pyserial
"""
import argparse
import re
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
    'sensor_update_time_ms',
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


def find_port(port):
    if port:
        return port
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if p.vid == RECEIVER_VID and p.pid == RECEIVER_PID]
    if not ports:
        sys.exit('No receiver found on USB (1209:7690); give --port')
    if len(ports) > 1:
        sys.exit('More than one receiver on USB: give --port')
    return ports[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tracker', type=int, required=True, help='tracker id (the receiver\'s "list" order)')
    ap.add_argument('--port', help='receiver console serial port')
    ap.add_argument('command', choices=('list', 'get', 'set', 'reset'))
    ap.add_argument('setting', nargs='?', help='setting name or id (list_config)')
    ap.add_argument('value', nargs='?', type=int)
    args = ap.parse_args(argv)
    if args.command != 'list' and args.setting is None:
        ap.error(f'{args.command} needs a setting')
    if args.command == 'set' and args.value is None:
        ap.error('set needs a value')
    try:
        import serial  # noqa: F401
    except ImportError:
        sys.exit('Needs pyserial: python -m pip install pyserial')

    con = Console(find_port(args.port))
    if args.command == 'list':
        failed = 0
        for sid, name in enumerate(SETTINGS):
            value, err = con.request(args.tracker, 'get', sid)
            print(f'{sid:3} {name:32} {value if err is None else "(" + err + ")"}')
            if err and 'did not answer' in err:
                sys.exit(1)
            failed += err is not None
        sys.exit(1 if failed else 0)
    sid = setting_id(args.setting)
    op = {'get': 'get', 'set': 'set', 'reset': 'reset'}[args.command]
    value, err = con.request(args.tracker, op, sid, args.value if args.command == 'set' else None)
    if err:
        sys.exit(f'tracker {args.tracker} {setting_name(sid)}: {err}')
    print(f'tracker {args.tracker} {setting_name(sid)} = {value}')


if __name__ == '__main__':
    main()
