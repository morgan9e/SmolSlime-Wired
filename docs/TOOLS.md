# PC tools

[Documentation index](README.md)

Run these commands from the repository root in Windows. Install the dependencies
with `python -m pip install pyserial hidapi PySide6`. Install the package named
`hidapi`, not `hid`. `slime_view.py` uses PySide6; `serial_monitor.py` uses Python's
Tkinter.

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

The window shows eight tracker cards (4 × 2). With more trackers it grows, adding
columns and then rows while it still fits on the screen. It only scrolls when the
screen is too small for every card.

Quit SlimeVR Server before Wi-Fi mode because both need UDP 6969. USB HID is shared
on Windows; use the viewer alone if you want an isolated diagnostic session.

Assign chest, hips, both thighs and both ankles; feet, upper arms and lower arms are
optional, and an unassigned part moves with its parent. "Ankle" is SlimeVR's name for
the lower-leg tracker. The resets follow SlimeVR Server:

| Button | Pose |
| --- | --- |
| **Full reset** | Stand upright, legs straight, arms at your sides (3 s) |
| **Mount reset** | Ski pose: feet parallel and flat, knees bent, hips back, torso forward without turning; upper arms back, forearms forward (5 s) |
| **Feet reset** | On tiptoes, both feet pointing forward (3 s). Needed because feet stay flat in the ski pose |

Repeat after remounting or tracker restarts. Older valid samples can be used for inactive trackers, so ensure they
still represent the requested pose and read the status message.

While **Follow SlimeVR** is ticked, body parts follow SlimeVR
Server's own tracker assignments: `vrconfig.yml` (`trackers:` → `designation`) and
`settings/<profile>.json` (`trackers` → `bodyPart`) in
`%APPDATA%\dev.slimevr.SlimeVR`. Chest/upper chest map to Chest, hip/waist to Hips,
upper legs to thighs, lower legs to ankles, plus feet, upper arms and lower arms;
hands, shoulders and head are ignored. SlimeVR keeps separate Wi-Fi
(`udp://MAC/0`) and HID (`MAC/0`) entries; the one for the tracker's current transport
wins, and the newer file wins between the two formats. Choosing a part by hand unticks
it. Use `--slimevr-config PATH` for another location or `--no-slimevr` to disable.

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
