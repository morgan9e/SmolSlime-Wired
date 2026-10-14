# SlimeVR nRF: stock firmware with small changes

Stock SlimeVR nRF receiver and tracker firmware (Tracker `cbfff16`, Receiver `e3d66d5`, both
upstream main as git subtrees), with only the changes this setup uses: an nRF52840 Dongle
receiver, ProMicro stacked trackers and a Seeed XIAO nRF52840 Sense.

| Path | What |
|------|------|
| `Receiver` | SlimeVR-Tracker-nRF-Receiver (git subtree of upstream) |
| `Tracker` | SlimeVR-Tracker-nRF (git subtree of upstream) |
| `BleReceiver` | ESP32-C6 BLE receiver for trackers in BLE mode (ESP-IDF, [docs/BLE.md](docs/BLE.md#esp32-c6-receiver)) |
| `tools` | PC tools |
| `QuestForwarder` | Android app: forwards the receiver's USB HID to SlimeVR Server (Quest Bridge) |

## Changes from stock

- **Link diagnostics** (receiver console `diag on` / `diag off`): per-tracker frame counts,
  losses and CRC errors on the receiver's console.
- **Send rate setting** (tracker): `sensor_update_time_ms`, how often a moving tracker sends its
  orientation, default 10 ms (100 Hz; stock 6 ms, about 166 Hz). Change it at runtime with
  `write_config sensor_update_time_ms <6-33>`, no rebuild. Each tracker also gets its own ESB
  retransmit delay from its device address, so trackers whose frames collide do not retry in
  lockstep.
- **Firmware updates over the radio (OTA)**: MCUboot builds of the tracker take new images
  through the receiver (`tools/ota.py`), with rollback if a new image never reaches the
  receiver. See [docs/OTA.md](docs/OTA.md).
- **Settings over the radio**: read, write and reset a tracker's settings from the PC through the
  receiver, no USB cable (`tools/remote_config.py`, receiver console `cfg`), in every tracker
  build. See [docs/OTA.md](docs/OTA.md#settings-over-the-radio).
- **BLE mode** (tracker console `ble on` / `ble off`, reboots): the tracker talks BLE instead of
  ESB. Best with the **receiver in BLE mode** (receiver console `ble on`): SlimeVR Server reads it
  as before (USB HID), trackers keep their ids, `remote_config.py --tracker` works; six moving
  trackers at ~135 packets/s each. Also: the PC's Bluetooth with `tools/ble_bridge.py`, or an
  ESP32-C6/S3 (`BleReceiver/`). No OTA over BLE. nRF52840 trackers. See [docs/BLE.md](docs/BLE.md).
- **Always on**: no sleep when still (`USE_IMU_WAKE_UP=n`, `USE_ACTIVE_TIMEOUT=n`), as SlimeVR's
  "Sleep (WOM) off" builds. A tracker still powers off after 5 min without its receiver.
- The tracker version comes from this repository's tags (`tools/bump_version.py`).
- Fix: the stock tracker did not build for the stacked I2C board.

## Build (WSL/Linux) and flash (Windows)

```bash
make tracker        # ProMicro stacked SPI
make tracker-i2c    # ProMicro stacked I2C: an IMU whose CS/SDO are not wired to the ProMicro
make tracker-xiao   # XIAO nRF52840 Sense
make tracker-chrysalis  # ABRJ Chrysalis (ProMicro, LSM6DSV + QMC6309 magnetometer)
make receiver       # nRF52840 Dongle
make ble-receiver   # ESP32-C6 BLE receiver (ESP-IDF v5.5): flash with esptool, docs/BLE.md

make tracker-ota       # ProMicro SPI with MCUboot: _OTA.uf2 (first USB install) + _OTA_app.bin (OTA)
make tracker-ota-i2c   # the same, stacked I2C
make tracker-ota-xiao  # the same, XIAO
```

A tracker running MCUboot must be USB-flashed with its `_OTA.uf2` only: the plain `.uf2`
overwrites MCUboot. Updates after that go over the radio:
`python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker <id>`.

Images land in `dist/`. Flash trackers with `python tools/flash_usb.py <image> --only <id>`
(console `dfu`, then the UF2 drive), the dongle with the `.zip` through nrfutil or the `.hex`
through nRF Connect Programmer. Pairing and calibration are kept: the images leave the settings
area alone.

## PC tools

Install with `python -m pip install pyserial hidapi PySide6`.

| Tool | Use |
| --- | --- |
| `slime_link.py` | Link monitor from the receiver's USB HID: rate, RSSI, battery, gaps, firmware |
| `slime_view.py` | Orientation and skeleton viewer (`--usb`) |
| `serial_monitor.py` | Console for the receiver and trackers |
| `flash_usb.py` | USB-flash trackers by ID (`--only 0,3`) |
| `ota.py` | Update a tracker over the radio through the receiver |
| `remote_config.py` | Read and change a tracker's settings over the radio (`--tracker 3 set sensor_update_time_ms 10`) |
| `quest_bridge.py` | PC side of the Quest Bridge |
| `ble_bridge.py` | Trackers in BLE mode to SlimeVR Server (needs `bleak`; `--serial` through the ESP32-C6 receiver) |
| `dfu_zip.py` | Package the dongle image for Nordic DFU (`make receiver` calls it) |
| `bump_version.py` | Tag the next patch version on HEAD |
