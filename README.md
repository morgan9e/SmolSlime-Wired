# SmolSlime-mod

SlimeVR nRF firmware with small changes for fully wireless body tracking: a Nordic
nRF52840 Dongle receiver, six ProMicro stacked SPI trackers, and a spare Seeed XIAO
nRF52840 Sense. The PC runs SlimeVR Server. Trackers use ESB at 2 Mbps and can receive
firmware updates over the radio through the dongle.

The working firmware is in `Tracker/` and `Receiver/`. Earlier wired chains and the
ESP32 hub belong to the separate `wired` history. They are not needed for this setup.

## Start here

- [Documentation index](docs/README.md): setup, tools, development and historical notes.
- [Project handoff](docs/MEMORY.md): decisions, working rules and outstanding work.
- [Hardware](docs/HARDWARE.md): board identities and last recorded installed versions.
- [OTA and recovery](docs/OTA.md): which image to flash and how to verify an update.
- [Link testing](docs/TESTING.md): establish the current six-tracker baseline.

**On a tracker with MCUboot installed, USB-flash only its board's `_OTA.uf2`.**
The plain tracker UF2 overwrites MCUboot. Use `_OTA_app.bin` for radio updates.

## What differs from upstream

| Component | Changes |
| --- | --- |
| Tracker | 10 ms moving-loop setting, device-derived retry delay, radio-clock idle fix, MCUboot/OTA support, charge-time battery estimate |
| Receiver | Per-tracker and radio diagnostics, OTA offers and streaming relay through HID OUT |
| PC tools | Link monitor, orientation/skeleton viewer, USB flashing, OTA updater and version tagging |

The 10 ms setting is a nominal 100 Hz target. IMU FIFO rounding gives about 120 Hz
on the ProMicro's LSM6DSV; it is not an exact 100-packet/s limit. New frames still
replace unsent old frames. The charge estimate is implemented but its hardware
validation remains open.

The recorded stock six-tracker moving test lost 53% of frames; the first rate/retry
change reduced that to 25%. The no-flush experiment performed worse and was reverted.
There is no recorded all-six-moving test on 0.0.3 or later. See the
[dated measurements](docs/archive/MEASUREMENTS.md); these are not current performance claims.

An opt-in [TDMA prototype](docs/TDMA.md) adds receiver beacons and eight fixed
transmit slots. It is awaiting hardware validation; ordinary builds still use
legacy ESB. Build it separately with `make tdma`.

## Build and update

Build in WSL/Linux using the SDK workspaces configured in the [Makefile](Makefile).
See [development setup](docs/DEVELOPMENT.md) for versions, paths and overrides.
Run commands from the repository root.

```bash
make tracker-ota       # ProMicro: combined USB UF2 + OTA application BIN
make tracker-ota-xiao  # XIAO Sense: its own combined UF2 + application BIN
make receiver         # Nordic dongle HEX + DFU ZIP
make test             # PC link-monitor unit tests
```

**Bare `make` builds a plain ProMicro tracker and the receiver, not an OTA tracker.**
Use the explicit OTA targets above for the installed MCUboot setup.

Before a rebuild that will be flashed, commit the source and run
`python tools/bump_version.py` to create the next annotated `0.0.N` tag, then build.
Do not bump a firmware version for documentation-only changes.

Install host dependencies in Windows:

```powershell
python -m pip install pyserial hidapi
```

For an already paired MCUboot tracker, update the test tracker first. Confirm its
device address and current receiver ID before using the recorded ID 5:

```powershell
python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker 5
```

The transfer finishing is not confirmation of a successful boot. Check the new
version, resumed tracking, and `Image: confirmed` on the tracker's `info` console.
Follow [OTA and recovery](docs/OTA.md) for first installation, USB recovery, XIAO
images and receiver flashing.

## Observe the trackers

```powershell
python tools/slime_link.py --duration 60 --gap 50 --csv link.csv
python tools/slime_view.py --usb
python tools/serial_monitor.py
```

`slime_link.py` runs alongside SlimeVR Server on Windows. It measures received rate,
RSSI and gaps; exact sequence loss requires `diag on` on the receiver console.
The viewer chooses one input at a time: USB when available, otherwise the legacy
Wi-Fi hub. Stop SlimeVR Server before using Wi-Fi mode. See [PC tools](docs/TOOLS.md)
for options and calibration.

## Repository and upstream

This is one repository with upstream Git histories imported as subtrees:

| Directory | Upstream | Imported base |
| --- | --- | --- |
| `Tracker/` | [SlimeVR-Tracker-nRF](https://github.com/SlimeVR/SlimeVR-Tracker-nRF) | `cbfff16` |
| `Receiver/` | [SlimeVR-Tracker-nRF-Receiver](https://github.com/SlimeVR/SlimeVR-Tracker-nRF-Receiver) | `e3d66d5` |

The configured origin is [morgan9e/SmolSlime-mod](https://github.com/morgan9e/SmolSlime-mod).
The subtree READMEs retain upstream information and license terms. See
[development](docs/DEVELOPMENT.md) before integrating upstream changes.
