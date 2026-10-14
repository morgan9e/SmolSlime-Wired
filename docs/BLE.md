# Tracker BLE mode

Status (2026-10-14): built for every tracker image; PC bridge unit-tested. On hardware (ProMicro
D235, standard MPSL clocks): ESB on the dongle 28.9 packets/s with no gaps; BLE from a Linux laptop
(BlueZ) connects 3/3 in under 1.5 s and streams ~30 packets/s with 0 lost; settings over BLE work.
This Windows PC (Realtek adapter, Bluetooth headphones on it) mostly fails to start connections:
run the bridge on another machine with `--server <PC address>`. Not measured: six trackers,
latency, range, current.

An earlier build used `MPSL_USE_EXTERNAL_CLOCK_CONTROL` (experimental on nRF52): BLE connections
failed silently and USB stopped enumerating. Tracker DEE8 still runs a build with it; its USB
does not come up, so it needs its bootloader (reset) to be reflashed.

## Use

On the tracker's USB console (`tools/serial_monitor.py`):

    ble          # Connection: ESB|BLE, and in BLE mode its name, link and notification counts
    ble on       # store connection_over_ble=1 and reboot into BLE mode
    ble off      # back to ESB (the receiver pairing is kept)

The setting is also `write_config connection_over_ble 0|1` on the console (takes effect on the
next boot). Over the radio the tracker switches at once (it reboots after answering):

    python tools/remote_config.py --tracker 3 set connection_over_ble 1   # ESB -> BLE, via the receiver
    python tools/remote_config.py --ble 0791 set connection_over_ble 0    # BLE -> ESB, via ble_bridge.py

`info` shows the mode too.

On the PC (Windows, Linux or macOS with a BLE adapter), with SlimeVR Server running:

    python -m pip install bleak
    python tools/ble_bridge.py              # every SlimeNRF tracker it finds
    python tools/ble_bridge.py --only 0791  # SlimeNRF-0791 only

A tracker advertises as `SlimeNRF-XXXX`, the last four hex digits of its device address (`info`).
The bridge connects to each, then registers it with the server as a UDP tracker under its device
address, like `quest_bridge.py`. It logs per tracker every 5 s: rotations/s sent to SlimeVR,
packets/s received, and packets lost (from the sequence number).

Settings over BLE, while the bridge runs: `remote_config.py --ble 0791 list|get|set|reset ...`,
the same commands and setting names as through the receiver. A tracker takes one BLE connection,
so they go through the bridge's control port (UDP 127.0.0.1:6971, local only, no
authentication; `--control-port` / `--bridge-port` to change it).

LED: short blink every second while advertising (as when pairing), four flashes on connect.
With `user_shutdown` on, a tracker that stays unconnected for `connection_timeout_delay` powers
off, as it does without a receiver.

## What it is

- One image has both: `CONFIG_TRACKER_BLE` (default on for nRF52840) builds the Zephyr BLE host
  and the SoftDevice Controller in beside ESB. About +116 KB flash, +15 KB RAM (ProMicro SPI: 353 KB / 58 KB). The OTA slots
  still fit: ProMicro 365 of 444 KB, XIAO 349 of 368 KB (the tightest).
- The radio runs one of them per boot. MPSL (the controller's scheduler) keeps its default clock
  control and stays initialized in both modes; in ESB mode it is idle and ESB connects its own
  radio interrupt over MPSL's (`ESB_DYNAMIC_INTERRUPTS`, `MPSL_DYNAMIC_INTERRUPTS`). Flash writes
  go through MPSL timeslots in both modes.
- USB self-check (`usb.c`): USB power present but the host has not configured the device in 15 s
  → reboot, up to 3 times per plug-in, so a stalled USB stack recovers without a hand on it.
- No BLE no-connection power-off while on USB power (a plugged-in tracker could not wake again).
- BLE notifications are sent from their own thread (`ble_tx_thread`, 1.5 KB stack); the
  connection thread's 512 B stack overflowed in the BLE send path.
- GATT service `6e5c0001-7a3b-4c8e-9f1d-2b4a5c6d7e8f`:
  - `…0002` notify: the 16-byte packet `connection.c` would send over ESB (types 0-7), plus a
    sequence byte (17 bytes, fits the default MTU). The tracker id byte is 0.
  - `…0003` read: the device address, 6 bytes little endian.
  - `…0004` write + notify: settings, the receiver path's 10-byte `CFG_REQ` / `CFG_RESP`
    (`Tracker/src/ota_proto.h`), handled by the same `remote_config.c`.
- At most two notifications wait in the stack; while they are full only the newest packet waits
  (ESB flushes its queue the same way). Advertising 20-30 ms, only while unconnected. Preferred connection interval 7.5-15 ms, requested 5 s
  after connecting; the central decides (`ble` shows what it got). +8 dBm. No pairing/bonding.
- A new MCUboot image confirms itself after 200 delivered notifications in BLE mode, as after 200
  receiver acks in ESB mode.

## Changed for ESB mode too

- RC clock calibration is on (MPSL, every 4 s; BLE needs 500 ppm); upstream had it off for a
  faster boot. The calibration briefly runs the crystal now and then.
- MPSL owns the clocks and stays initialized; ESB takes the radio interrupt over at `esb_init`.
- System workqueue stack 256 → 1024 B.

## Open

- Hardware: six trackers on one adapter, latency, range, and current draw in both modes. OTA and
  flash writes in ESB mode with MPSL idle (timeslots) are not checked yet.
- Windows PC central: connection starts fail (Realtek adapter with headphone audio on it).
- Firmware updates over BLE are not implemented (ESB or USB only). This repository's own OTA
  protocol over GATT would fit; mcumgr would not fit the XIAO's slot (about 27 KB free).
- The bridge keeps scanning after its trackers are connected, which costs throughput on Windows.
- Reconnect after a dropout waits for the 4 s supervision timeout and connects one tracker at a
  time.
- Always on: `prj.conf` now has `USE_IMU_WAKE_UP=n` and `USE_ACTIVE_TIMEOUT=n` (no sleep when
  still). The no-receiver / no-BLE-connection shutdown is a full power-off; boards without a
  button (7A93, 74DA) wake from it only by USB power or reset.
