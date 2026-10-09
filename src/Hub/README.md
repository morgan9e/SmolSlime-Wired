# Slime Hub (XIAO ESP32-C6)

Paths in this file are relative to the `SlimeVR-Hub` folder; this project is `src/Hub`.
Build it with `make hub` (or `make hub-s3`) from there.

Firmware for the hip hub in the On-Body Hub plan v3. It reads the SlimeVR nRF receiver's
UART mirror and presents every tracker to the stock SlimeVR Server as a Wi-Fi tracker,
so no PC program runs in the data path.

Default target: Seeed XIAO ESP32-C6, ESP-IDF v5.5. The same tree builds for the Seeed
XIAO ESP32-S3 (see Build). An ESP32-C5 also works with `-D IDF_TARGET=esp32c5`, with the
RX and LED pins set in menuconfig. The C6 is 2.4 GHz only like the S3; the C5 remains the
5 GHz fallback if Wi-Fi beside the receiver causes loss.

## Wiring

| Receiver (ProMicro) | XIAO pin | C6 GPIO | S3 GPIO | Notes |
| --- | --- | --- | --- | --- |
| P0.08 / D0 (UART TX) | D7 (RX) | 17 | 44 | 3.3 V logic, under 10 cm |
| GND | GND | | | |
| B+ / RAW | battery + | | | receiver powered from the cell, never from USB at the same time |

The XIAO battery pads take the protected 1S cell. The user LED shows state: fast blink =
no Wi-Fi, slow blink = Wi-Fi but no server, solid = streaming. It is GPIO15 on the C6
and GPIO21 on the S3.

**C6 antenna.** The XIAO ESP32-C6 has an RF switch. The firmware drives GPIO3 low to
power it and GPIO14 low to select the built-in antenna before Wi-Fi starts. For the U.FL
connector, enable `Slime Hub > Board > Use the U.FL external antenna`. Leave GPIO3 and
GPIO14 unconnected.

Optional hub battery voltage: two 220 kOhm resistors from BAT+ to GND, midpoint to A0
(GPIO0 on the C6, GPIO1 on the S3), then enable
`Slime Hub > Board > Measure hub battery voltage` in menuconfig.

## Receiver link contract (for the receiver fork)

The receiver side is implemented in `src/Receiver` (see its `HUB.md`). A prebuilt image for the ProMicro is
`dist/SlimeNRF_ProMicro_HubReceiver.uf2`.


Every 16-byte tracker packet the receiver passes to `hid_write_packet_n()` (after the RSSI
byte is written) is sent as:

```
COBS( packet[16] + crc16_le[2] ) + 0x00        1,000,000 baud, 8N1
crc16 = crc16_itu_t(0xFFFF, packet, 16)         poly 0x1021, init 0xFFFF, not reflected
```

Use Zephyr's `crc16_itu_t()`. Do not use `crc16_ccitt()`, which is the reflected variant.
The receiver must also send a type 255 packet (`packet_device_addr()` layout) for each
stored tracker about every 100 ms. The hub ignores data for an id until it has seen its 255
packet, and uses the address as the tracker's MAC in SlimeVR.

Packet types 224-254 are the receiver's own link diagnostics ("diag hub on" on the receiver).
The hub sends each one unchanged as a 16-byte UDP datagram to port 6971 at the server's
address, once the server is known, and never decodes them. `slime_diag.py` in the
`SlimeVR-Hub` folder shows them; the layout is in the receiver's `HUB.md`.

## Build

ESP-IDF v5.5 in WSL or Linux:

```
. ~/esp/esp-idf/export.sh
idf.py -B ~/build/slime_hub_c6 build          # XIAO ESP32-C6 (default)
idf.py -B ~/build/slime_hub_s3 -D SDKCONFIG=$HOME/build/slime_hub_s3/sdkconfig -D IDF_TARGET=esp32s3 build
idf.py -B ~/build/slime_hub_c6 menuconfig     # Slime Hub options
```

Building in the Linux filesystem (`~/build/...`) is much faster than on `/mnt/c`.
The S3 command keeps its own sdkconfig so the two targets never overwrite each other.

On Windows, the ESP-IDF VS Code extension or ESP-IDF PowerShell opens this folder as is.

## Flash

WSL cannot see USB serial ports without usbipd, so flash from Windows. Prebuilt merged
images are in `dist/` (written at offset 0):

```
pip install esptool
python -m esptool --chip esp32c6 -p COM7 write_flash 0x0 dist\slime_hub-xiao-esp32c6.bin
python -m esptool --chip esp32s3 -p COM7 write_flash 0x0 dist\slime_hub-xiao-esp32s3.bin
```

The browser flasher at https://espressif.github.io/esptool-js/ also works with that file at
address 0x0. If the port does not appear, hold BOOT while plugging the XIAO in.

The merged image also covers the settings partition (0x9000), so it erases the stored Wi-Fi
credentials and server IP: run `wifi` again afterwards. To update a hub that is already set
up, write only the application instead, which keeps them:

```
python -m esptool --chip esp32c6 -p COM7 write-flash 0x10000 <build dir>\slime_hub.bin
```

## First setup

Open the XIAO's USB serial port (115200, any terminal, or `idf.py monitor`) and type:

```
wifi "MyNetwork" mypassword     # stores credentials and reboots
server 192.168.0.10             # optional: fixed server IP if broadcast is blocked
server auto                     # back to discovery
```

The SlimeVR Server asks once per tracker MAC whether to accept a new device. Accept each
one; the MAC is the tracker's radio address, so body assignments stay with the tracker.

## What it sends

Each receiver id gets its own UDP socket on local port 6980 + id, with its own packet counter.

| SlimeVR packet | When |
| --- | --- |
| 3 Handshake (board CUSTOM, protocol 22, tracker IMU/MCU/firmware) | once per second until the server answers |
| 15 SensorInfo | after the handshake until acknowledged, and when the tracker reports an error status |
| 23 RotationAndAcceleration | each type 1, 2 and 7 packet (same Q15/Q7 scaling as the HID path) |
| 17 RotationData | each type 4 packet |
| 12 Battery, 19 Signal strength, 20 Temperature | 1 Hz |
| 0 Heartbeat, 10 Ping echo | in reply to the server |

Raw sensor-frame quaternions are passed through; the server applies its axis offset and
mounting exactly as for HID trackers. Only real tracker packets are forwarded. A tracker
without data for 2 s stops answering the server, so the server shows it as timed out; after
30 s of silence (`HUB_DROP_MS`) it re-handshakes when its data returns.

## Tools

- `tools/fake_receiver.py --port COM5` drives the hub from a USB-UART adapter (TX to D7)
  with fake trackers, for bench tests before the receiver fork exists.

## Tests

- `test/run.sh` runs host unit tests for COBS, CRC16, the deframer and the decoder against
  packets built with the tracker firmware's own formulas.
- `test/sim/run_sim.sh [seconds] [server_ip|auto]` runs the real `slime.c` on Linux with
  simulated trackers (continuous, periodic 300 ms gaps, a 7 s dropout, one corrupt frame).
  It was run against SlimeVR Server v21.1.0 in an isolated network namespace: discovery,
  handshake, sensor registration, timeout and re-handshake all behaved as described, with
  no parse or packet-order errors in the server log.

## Notes against the plan

- Type 1 and 4 packets carry the quaternion as x, y, z, w, not w, x, y, z.
- Packet 23 replaces the separate 17 + 4 pair for data with acceleration.
- New MACs need a one-time accept in the SlimeVR GUI.
- On the single-core C6 all tasks share one core; the UART parser runs above the hub
  task and below Wi-Fi and lwIP.
- The ESP32 UART driver uses an interrupt-fed 4 KB ring buffer rather than DMA; at 22 KB/s
  this is well inside its limits.
