# SlimeVR nRF: stock firmware with small changes

Stock SlimeVR nRF receiver and tracker firmware (Tracker `cbfff16`, Receiver `e3d66d5`, both
upstream main as git subtrees), with only the changes this setup uses: an nRF52840 Dongle
receiver, ProMicro stacked trackers and a Seeed XIAO nRF52840 Sense.

| Path | What |
|------|------|
| `Receiver` | SlimeVR-Tracker-nRF-Receiver (git subtree of upstream) |
| `Tracker` | SlimeVR-Tracker-nRF (git subtree of upstream) |
| `tools` | PC tools |
| `QuestForwarder` | Android app: forwards the receiver's USB HID to SlimeVR Server (Quest Bridge) |
| `ImuProbe` | Test firmware: is there an IMU on the stacked ProMicro's SPI pins? |

## Changes from stock

- **Link diagnostics** (receiver console `diag on` / `diag off`): per-tracker frame counts,
  losses and CRC errors on the receiver's console.
- **Send rate setting** (tracker): `sensor_update_time_ms`, how often a moving tracker sends its
  orientation, default 10 ms (100 Hz; stock 6 ms, about 166 Hz). Change it at runtime with
  `write_config sensor_update_time_ms <6-33>`, no rebuild. Each tracker also gets its own ESB
  retransmit delay from its device address, so trackers whose frames collide do not retry in
  lockstep.
- The tracker version comes from this repository's tags (`tools/bump_version.py`).

## Build (WSL/Linux) and flash (Windows)

```bash
make tracker        # ProMicro stacked SPI
make tracker-i2c    # ProMicro stacked I2C: an IMU whose CS/SDO are not wired to the ProMicro
make tracker-xiao   # XIAO nRF52840 Sense
make receiver       # nRF52840 Dongle
```

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
| `quest_bridge.py` | PC side of the Quest Bridge |
| `dfu_zip.py` | Package the dongle image for Nordic DFU (`make receiver` calls it) |
| `bump_version.py` | Tag the next patch version on HEAD |
