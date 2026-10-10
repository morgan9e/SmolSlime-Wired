# PC tools

[Documentation index](README.md)

Run these commands from the repository root in Windows. Install `pyserial` and
`hidapi` with `python -m pip install pyserial hidapi`; the graphical tools also
need Python's Tkinter. Install the package named `hidapi`, not `hid`.

## Link monitor

```powershell
python tools/slime_link.py --duration 60 --gap 50 --csv link.csv
```

Reads the receiver's USB HID output without writing to it. On Windows it can run
alongside SlimeVR Server. It shows firmware, received rate, RSSI, battery, gaps and
offline events; Ctrl+C prints a summary.

| Option | Meaning |
| --- | --- |
| `--duration SECONDS` | Stop after the specified period |
| `--gap MILLISECONDS` | Gap threshold; default 250, use 50 for moving tests |
| `--offline SECONDS` | Offline threshold; default 2 |
| `--csv PATH` | One row per tracker per second |

The HID stream has no ESB sequence number. The monitor cannot measure exact radio
loss or CRC failures; use receiver console `diag on` as well. Quiet trackers send
less often, so a gap threshold suitable for motion is not a failure threshold at
rest. See [testing](TESTING.md).

## Orientation and skeleton viewer

```powershell
python tools/slime_view.py
python tools/slime_view.py --usb
python tools/slime_view.py --wifi --port 6969
```

Default mode checks every two seconds: use USB when a receiver is present,
otherwise the legacy ESP32 hub's Wi-Fi output. Only one source runs at a time.
`--usb` is the explicit choice for this wireless tracker/dongle setup. The
`--usb` and `--wifi` flags are mutually exclusive.

Quit SlimeVR Server before Wi-Fi mode because both need UDP 6969. USB HID is shared
on Windows; use the viewer alone if you want an isolated diagnostic session.

Assign chest, hips, both thighs and both lower legs. Click **Calibrate**, stand
upright with straight legs during the three-second countdown, then use **Ski-pose
mounting**: feet parallel and flat, knees bent, hips back and torso forward without
turning; hold through the five-second countdown. Repeat after remounting or tracker
restarts. Older valid samples can be used for inactive trackers, so ensure they
still represent the requested pose and read the status message.

Assignments and calibration save to `tools/slime_view_config.json`; override with
`--config PATH`. Treat it as personal local state. The skeleton is a preview with
fixed proportions and a stationary hip, not a replacement for server calibration.

## Serial console

```powershell
python tools/serial_monitor.py
```

Select the device by identity, then open its console at 115200 baud with DTR enabled.
Useful receiver commands: `list`, `diag on`, `diag off`, `ota status`.
Use `info` on a tracker to inspect firmware, board target and MCUboot state.
The GUI stores preferences in ignored `tools/serial_monitor_settings.json`.

Close the relevant serial connection before OTA or USB flashing. A read error
after `dfu` is expected because the application port disappears.

## Update and build helpers

| Tool | Use |
| --- | --- |
| `ota.py IMAGE --tracker ID` | Radio update; optional `--port`, `--board`, `--timeout` (default 300 s) |
| `flash_usb.py IMAGE --only 5` | USB-flash selected tracker IDs; optional `--board`; without `--only`, flashes every matching connected tracker |
| `dfu_zip.py INPUT.hex OUTPUT.zip` | Package the dongle image for Nordic DFU; normally called by `make receiver` |
| `bump_version.py` | Create the next annotated patch tag on HEAD; commit tracked changes first |

Use [OTA](OTA.md) for the full update procedure. Board inference in the flash tools
uses the image filename; do not rename images ambiguously or supply a guessed
`--board`. The updater permits only one attached receiver and uses its serial
console plus HID OUT.

The old `slime_diag.py`, UART hub helpers and flashing watchers described in the
archive belong to the wired history or old scratch folders, not the current toolset.
