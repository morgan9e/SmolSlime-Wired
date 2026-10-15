# Tracker BLE mode

**Recommended receiver: the nRF52840 Dongle in BLE mode** (receiver console `ble on`, below).
SlimeVR Server reads it as always (USB HID), trackers keep their ids, settings work through it.
Tested 2026-10-14 with eight trackers in BLE mode (seven ProMicro, a XIAO): all connected within
15 s of `ble on`, 12.5 ms interval, 2M PHY; still ~22-30 packets/s each, 0 gaps; all eight moving at
once 122-136 packets/s each (ESB gave 100), no gap over 50 ms, 0.0-0.3% of their packets dropped
at the trackers (newest kept). AB6C reached only ~63/s: its IMU interrupt line does not work, it
polls.

Ten trackers (2026-10-15, the eight plus two ABRJ Chrysalis, `RECEIVER_BLE_MAX_TRACKERS=10`), all
sending at full rate on a desk (`sensor_send_unchanged`, [TRACKER.md](TRACKER.md)), 30 s each:

| | 2M, 16.25 ms | 1M (`RECEIVER_BLE_PHY_1M`), 20 ms |
|---|---|---|
| Per tracker | ~120/s (102-138) | ~97/s (83-99) |
| All ten | 1070/s | 874/s |
| Lost over the air | 0 (receiver count = tracker sent count) | 0 |
| Dropped at the trackers (made ~139/s) | 12% | 28% |

Worn in play (2026-10-14, eight trackers, 2M): nothing went offline, longest silence 188 ms (ESB
with the same trackers: 1.4 s and 3.2 s); see [Measured](#measured).

Status (2026-10-14): built for every tracker image; PC bridge unit-tested. On hardware (ProMicro
D235, standard MPSL clocks): ESB on the dongle 28.9 packets/s with no gaps; BLE from a Linux laptop
(BlueZ) connects 3/3 in under 1.5 s and streams ~30 packets/s with 0 lost; settings over BLE work.
This Windows PC (Realtek adapter, Bluetooth headphones on it) mostly fails to start connections:
use the ESP32-C6 receiver below, or run the bridge on another machine with `--server <PC address>`.

With the ESP32-C6 receiver (ProMicro DEE8): connects within 3 s of the tracker's boot, 7.5 ms
interval; still ~23 packets/s, moving ~139 packets/s, 0 lost over 30 s each (sequence numbers),
and the tracker's own dropped count unchanged; settings over BLE work; after a forced disconnect it
is back within 1 s. Five trackers at once (DEE8, 04A6, 0791, 8DAA, FBB1, still): all connect
within seconds of their reboot, all at 7.5 ms, ~22 packets/s each; four lose nothing (2 in 30 s),
DEE8 at -92 dBm loses 15-20% (the tracker drops packets while retransmissions hold its two
notifications in flight). The ESP32-C6 board heard them at -78 to -94 dBm where the dongle hears
-43 to -72: keep it near the trackers. Not measured: six trackers, moving with several, latency,
range, current.

An earlier build used `MPSL_USE_EXTERNAL_CLOCK_CONTROL` (experimental on nRF52): BLE connections
failed silently and USB stopped enumerating (Windows: "Device Descriptor Request Failed"). DEE8 ran
it until 2026-10-14; the build from 00a25ce (standard clocks) fixed both. Every MCUboot and I2C
image in `dist/` built before then has it: rebuild before flashing.

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

### nRF52840 Dongle receiver (BLE mode)

The receiver firmware (`make receiver`, the nRF52840 Dongle) has a BLE central beside ESB
(`Receiver/src/connection/ble.c`, `CONFIG_RECEIVER_BLE`). On its USB console:

    ble          # Connection: ESB|BLE; in BLE mode each tracker: id, address, RSSI, interval, PHY, packets
    ble on       # store receiver_ble=1 and reboot into BLE mode
    ble off      # back to ESB

- One radio, one mode per boot, as on the tracker; the USB HID output is the same in both.
- Paired trackers are found by the service UUID and connected automatically. A tracker's id is
  its place in the paired list: the address characteristic holds the device address it sends
  when pairing over ESB, so trackers paired over ESB keep their ids.
- New trackers, as over ESB, only while pairing: console `pair` (or Pair in `slime_view.py`), then
  `exit`. Meanwhile a tracker in BLE mode not in the list is added (the first free id) when it
  connects; otherwise it is disconnected and left alone until pairing starts.
- `unpair <id>` (or Remove on a card in `slime_view.py`) removes one tracker without renumbering
  the others: its id is empty (`list` shows it as free, its data is dropped) and the next new
  tracker takes it. In both modes. `remove` (the last one) and `clear` as before; in BLE mode they
  disconnect the trackers so the ids follow.
- Settings: `cfg` on the console and `remote_config.py --tracker <id>` go to the tracker's settings
  characteristic (no ACKs to ride on). Firmware updates (`ota`) need ESB.
- Timing: up to 8 trackers in a 12.5 ms interval, each 1.5 ms of it, two notification exchanges
  (`BT_CTLR_SDC_MAX_CONN_EVENT_LEN_DEFAULT`): up to 160 packets/s per tracker; a moving tracker
  sends about 167/s, ~130/s get through. 1 s supervision timeout. More trackers: build option
  `CONFIG_RECEIVER_BLE_MAX_TRACKERS` (default 8; 10: 16.25 ms, ~120/s each, tested). One interval for any count:
  10 ms for up to six, switched to 12.5 ms when a seventh connected, left one or two of eight
  moving trackers at half rate (their events overlapped after the switch).
  Measured on the way there (six moving): 7.5 ms with 0.9 ms each, and 10 ms with 1.2 ms each,
  allowed one exchange per event (the controller budgets 27-byte packets both ways, ~0.6 ms on
  2M) and the trackers dropped ~17-30%. Scanning stays at 30 ms a second while any tracker is
  connected: 30 of 60 ms took 27-35% from some moving trackers.
- PHY: 2M by default. `CONFIG_RECEIVER_BLE_PHY_1M` takes the controller's 2M support out, so every
  link stays on 1M: about 3 dB more sensitivity (≈ -95 against -92 dBm) for body-blocked trackers,
  twice the air time; each tracker gets 1.9 ms and the interval follows (8: 16.25 ms, 10: 20 ms).
- A connected tracker that sends nothing for 5 s is disconnected and reconnects (`SILENT_MS`): a
  tracker whose firmware stopped sending kept its link alive with empty packets and froze in
  SlimeVR.
- LF clock: calibrated RC with BLE built in (upstream synthesizes it from the HF crystal; the
  Dongle has no 32 kHz crystal). MPSL keeps its default clock control in both modes.
- Flash: console `dfu`, then `nrfutil device program --firmware dist/SlimeNRF_Nordic_Dongle_Receiver.zip
  --serial-number <bootloader>` (the paired list and the mode setting are kept).

### ESP32-C6 receiver

Instead of the PC's Bluetooth, an ESP32-C6 board on USB can be the central (`BleReceiver/`, ESP-IDF
v5.5, NimBLE). It finds trackers by their service UUID, connects up to 10 at once, subscribes, and
passes the notifications to the PC as text lines on its USB Serial/JTAG port; the bridge forwards
them to SlimeVR as usual, and settings work the same way:

    make ble-receiver                                                        # WSL: dist/SlimeNRF_ESP32C6_BleReceiver.bin
    python -m esptool --chip esp32c6 -p COM23 write-flash 0x0 dist/SlimeNRF_ESP32C6_BleReceiver.bin
    python tools/ble_bridge.py --serial      # port found by USB id (303A:1001), or --serial COM23
    python tools/remote_config.py --ble 4759 get sensor_update_time_ms

- It sets 7.5 ms and a 1 s supervision timeout itself, after the tracker's own request (7.5-15 ms,
  4 s) comes in: at 15 ms, one notification per event carries 66 packets/s, short of the moving
  rate, and a 4 s timeout keeps a tracker sending into the void that long after the receiver resets.
- Seeed XIAO ESP32-C6: its radio goes through an RF switch (GPIO3 low powers it, GPIO14 high picks
  the U.FL connector). The build switches to U.FL at boot (`CONFIG_SLIME_XIAO_C6_ANTENNA`); console
  `N 0` / `N 1` picks onboard / U.FL. Without it an antenna on the U.FL connector does nothing.
- Measured (2026-10-15): U.FL with a 2.4 GHz antenna beat the onboard antenna by 0-11 dB per
  tracker and was about equal to the nRF dongle (±7 dB). Trackers: seven moving at 7.5 ms lost
  ≤0.5%; nine moving lost up to 45% on one (interval not tuned past seven). Ten connections
  configured (`sdkconfig.defaults.esp32c6`); its controller took eight at 7.5 ms without refusing one.
- **Opening and closing its port can reset it**: the USB Serial/JTAG resets the chip on some
  DTR/RTS changes (pyserial asserts both on open; Windows drops DTR first on close), and every
  tracker then waits out its supervision timeout. `ble_bridge.py` opens it with both off
  (`open_receiver`); a serial monitor that toggles them resets it.
- Its lines (`BleReceiver/main/main.c` has the full list): `C <slot> <address>` connected,
  `D <slot> <hex>` a data notification, `X <slot> <reason>` disconnected, `E ...` a failed step,
  `# ...` status every 5 s (per tracker: packets/s, RSSI, interval). `O 0791,B8C8` limits it to
  those trackers, `K <slot>` disconnects one.

### ESP32-S3 receiver (USB HID)

The same firmware on a XIAO ESP32-S3 (`make ble-receiver-s3`) is a USB device like the nRF
receiver: VID/PID 1209:7690, a CDC console and the receiver's HID interface and reports
(`BleReceiver/main/link_usb.c`), so SlimeVR Server reads it directly, no bridge. Each tracker gets
a stable id in NVS (console `L` lists, `Z` forgets them). `ble_bridge.py --serial` still carries
settings (it finds the S3 by its serial number `5333…` and forwards no data).

- Flashing: its USB is TinyUSB's. Console `B` (or BOOT held while plugging in) reboots it into the
  ROM download mode; then `python -m esptool --chip esp32s3 -p COMx --before no-reset --after
  watchdog-reset write-flash 0x0 dist/SlimeNRF_ESP32S3_BleReceiver.bin`.
- Needs an antenna on its U.FL connector (no chip antenna): without, -92 to -98 dBm and no
  connection gets established (HCI 0x3E); with a 900 MHz antenna at desk range, -20 to -48 dBm.
- Its controller reserves 3.75 ms per connection in the interval: two trackers at 7.5 ms, four at
  15 ms, then HCI 0x0D (limited resources); the connection event length makes no difference. So
  30 ms by default, for eight, at most ~66 packets/s each (two notifications per event), short of
  the 100 Hz moving rate; `P 12` (15 ms) for up to four. Tested (2026-10-14): four at 15 ms and
  five at 30 ms, ~22 packets/s each while still, read by `slime_link.py` as a receiver. The nRF
  dongle as BLE receiver has no such limit.

LED: short blink every second while advertising (as when pairing), four flashes on connect.
With `user_shutdown` on, a tracker that stays unconnected for `connection_timeout_delay` powers
off, as it does without a receiver.

## Measured

Receivers compared (2026-10-14/15):

| | nRF52840 Dongle | XIAO ESP32-C6 | XIAO ESP32-S3 |
|---|---|---|---|
| Trackers at full rate | 10, 0 lost over the air | 7 (≤0.5% lost); 9: up to 45% on one | 4 at 15 ms; 8 only at 30 ms (~66/s) |
| Signal | reference (PCB antenna, +8 dBm) | U.FL: about equal (±7 dB) | needs a 2.4 GHz antenna: -92…-98 dBm without |
| To SlimeVR | USB HID | `ble_bridge.py --serial` | USB HID |

Signal at the dongle (RSSI it reports, 2M):

| | RSSI |
|---|---|
| On the desk, ten trackers | -35 to -62 dBm (Chrysalis -35/-38; once -76) |
| Worn, active play, average | -57 to -74 dBm per tracker |
| Worn, worst moment | -74 to -87 dBm (D235, 0791 lowest) |
| 2M needs / 1M needs | about -92 / -95 dBm |

Worn in play, per tracker (2026-10-14, "move a lot" minute):

| Tracker | Avg while moving | Longest gap | RSSI avg / worst |
|---|---|---|---|
| FBB1 | 132/s | 51 ms | -59 / -82 |
| 8DAA | 131/s | 51 ms | -59 / -78 |
| 0791 | 128/s | 63 ms | -67 / -82 |
| D235 | 117/s | 125 ms | -74 / -82 |
| 04A6 | 116/s | 101 ms | -70 / -79 |
| DEE8 | 111/s | 137 ms | -71 / -83 |
| FAE4 (XIAO, 8% battery) | 89/s | 188 ms | -74 / -83 |
| AB6C (polls, no IMU interrupt) | 64/s | 62 ms | -57 / -74 |

The rate follows the signal: weaker links retransmit, and a retransmission takes one of the two
exchanges a tracker gets per interval. In normal play trackers sit at their resting rate (~22-28/s)
and burst to 80-138/s when they move; longest gap 176 ms. The weakest trackers keep only 5-10 dB of
margin when the body blocks them: the dongle on a USB extension, up high and in the open, is the
first thing to try, then the 1M PHY. Not measured: RSSI against distance.

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

- Not measured: latency, RSSI against distance, current draw in both modes. OTA and flash writes
  in ESB mode with MPSL idle (timeslots) are not checked yet.
- 1M PHY measured on a desk only; whether its 3 dB helps body-blocked trackers in play is untested.
- Windows PC central: connection starts fail (Realtek adapter with headphone audio on it). The
  ESP32-C6 receiver avoids it.
- ESP32-C6 receiver: interval not tuned past seven moving trackers. It keeps scanning (30 of 60 ms)
  while it has room for more trackers. Once (2026-10-14) its USB output went silent while it kept
  running; flashing the current build fixed it. Not reproduced.
- Receiver: once (2026-10-15) the dongle did not come back after `ble off` then `ble on`; a replug
  did, and the switch worked on the next try. Not reproduced.
- Firmware updates over BLE are not implemented (ESB or USB only). This repository's own OTA
  protocol over GATT would fit; mcumgr would not fit the XIAO's slot (about 27 KB free).
- The bridge keeps scanning after its trackers are connected, which costs throughput on Windows.
- With the PC's Bluetooth, reconnect after a dropout waits for the 4 s supervision timeout and
  connects one tracker at a time (the ESP32-C6 receiver: 1 s).
- Always on: `prj.conf` now has `USE_IMU_WAKE_UP=n` and `USE_ACTIVE_TIMEOUT=n` (no sleep when
  still). The no-receiver / no-BLE-connection shutdown is a full power-off; boards without a
  button (7A93, 74DA) wake from it only by USB power or reset.
