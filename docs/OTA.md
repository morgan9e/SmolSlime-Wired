# Tracker firmware updates and recovery

[Documentation index](README.md) · Source checked: 2026-10-10 (KST).

Radio OTA was tested on all six ProMicro stacked SPI trackers and the XIAO
nRF52840 Sense. The historical tests include MCUboot boot, test swap, rollback,
radio updates and the failure cases below. They do not verify what is currently
plugged in or installed; identify devices using [hardware](HARDWARE.md).

The chosen design is MCUboot with rollback and unsigned images. MCUboot checks an
image SHA-256 and the tracker checks transfer CRC32; these detect corruption but
do not authenticate the sender. Tracking pauses during transfer.

## Choose the right file

| File | Use |
| --- | --- |
| `SlimeNRF_ProMicro_StackedSPI_Tracker_OTA.uf2` | ProMicro USB installation or recovery; MCUboot + app |
| `SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin` | ProMicro radio update |
| `SlimeNRF_XIAO_Sense_Tracker_OTA.uf2` | XIAO USB installation or recovery |
| `SlimeNRF_XIAO_Sense_Tracker_OTA_app.bin` | XIAO radio update |
| `SlimeNRF_Nordic_Dongle_Receiver.zip` | Dongle via Nordic DFU |
| `SlimeNRF_Nordic_Dongle_Receiver.hex` | Dongle via nRF Connect Programmer |

**Never USB-flash the plain tracker UF2 onto an MCUboot tracker:** it overwrites
MCUboot. Reinstall that board's combined `_OTA.uf2` to recover. The
`_OTA_slot1_test.uf2` is an experimental secondary-slot image, not a normal update.

Run commands from the repository root. Build in WSL/Linux; run the host tools in
Windows with `python -m pip install pyserial hidapi`. See
[development](DEVELOPMENT.md) for SDK setup and versioning.

```bash
make tracker-ota
make tracker-ota-xiao
make receiver
```

Build only the targets needed for the change. Bare `make` produces a plain
tracker build, not an OTA image. Existing protocol-1 devices do not need the
receiver reflashed for each tracker-only update.

## First installation or USB recovery

Identify the test tracker by address `628DCFC6318A` and check its current receiver
ID. The commands below assume it is still ID 5.

For a running tracker application, close its serial monitor connection and run:

```powershell
python tools/flash_usb.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA.uf2 --only 5
```

The tool checks the reported board target, sends `dfu`, copies to the newly
appearing UF2 drive, waits for USB to return and prints `info`.
**Without `--only`, it flashes every matching connected tracker.** Verify the
test result before intentionally updating the rest.

For the XIAO, use its own image and verified ID (last recorded as 6):

```powershell
python tools/flash_usb.py dist/SlimeNRF_XIAO_Sense_Tracker_OTA.uf2 --only 6
```

If the app does not boot or answer its console, double-tap reset and manually copy
the correct combined `_OTA.uf2` to that board's UF2 drive. The automatic tool
requires a working application console. Wait for the app to return and verify
firmware, tracking and confirmation. Pairing/calibration were preserved in the
recorded tests because these images leave NVS untouched.

## Radio update

Commit the intended source, run `python tools/bump_version.py`, then build the
matching OTA target. The tracker must be on, paired and running a confirmed
MCUboot image; the dongle must have this repository's OTA-capable receiver.

```powershell
python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker 5
```

For the XIAO, use `SlimeNRF_XIAO_Sense_Tracker_OTA_app.bin` and its verified ID.
Update one tracker at a time. Board inference uses the filename; `--board` is
available for explicitly identified custom image names. Keep only one receiver
attached. Close any program holding its serial console; the HID monitor and
SlimeVR Server can remain open on Windows.

The updater shows progress and reads the receiver's stored outcome. Optional
`--port` selects its current console port, and `--timeout` changes the whole-job
limit (default 300 seconds). Ctrl+C stops the offer.

**An installed/transfer result is not proof of a confirmed boot.** After reboot:

1. Check the new firmware in `slime_link.py` and confirm that tracking resumes.
2. On the tracker console, run `info` and wait for `Image: confirmed`.
3. Confirm pairing and calibration before proceeding to other trackers.

The app confirms after 200 acknowledged frames by default. At rest this took up
to a minute in the recorded tests. Rebooting before confirmation can roll back.
Historical transfer time was 21–27 seconds for roughly 248 KB, plus swap/reboot
and confirmation time; it is not a fixed update deadline.

## Receiver USB flashing

The dongle has its own Nordic bootloader; tracker OTA does not update it.
Flash it last when a coordinated change affects both sides.

Send `dfu` on its console or press the side RESET button, then use nRF Connect
Programmer with the HEX, or a device-capable nrfutil installation:

```powershell
nrfutil device list
nrfutil device program --firmware dist/SlimeNRF_Nordic_Dongle_Receiver.zip --serial-number <verified-bootloader-serial>
```

Replace the placeholder; the recorded dongle bootloader serial is `FCF6929F4E52`.
Its application USB serial is different. Verify the enumeration before flashing.

The previous machine installation also provided a standalone `nrfutil-device.exe`
under nRF Connect's versioned sandbox; that executable takes `list` and
`program` directly, without the `device` subcommand. Locate the installed tool
rather than copying an old version-specific absolute path. The command-line
programmer needs the ZIP, not the HEX.

After flashing, check the receiver's USB identity, stored tracker list and tracking.
Use `ota status` to inspect OTA state; an application serial read error immediately
after `dfu` is expected.

## How it works

### Tracker flash layout: MCUboot between the UF2 bootloader and the app

ProMicro ([partition map](../Tracker/pm_static/mcuboot_nrf52840_uf2.yml);
historical build sizes: MCUboot about 24 KB, app about 242 KB):

| Region | Range | Size (KiB) |
|---|---|---|
| MBR | 0x0 - 0x1000 | 4 |
| MCUboot | 0x1000 - 0xD000 | 48 |
| Slot 0 (running app) | 0xD000 - 0x7C000 | 444 |
| Slot 1 (incoming image) | 0x7C000 - 0xEA000 | 440 |
| (unused) | 0xEA000 - 0xEE000 | 16 |
| NVS settings | 0xEE000 - 0xF4000 | 24, as without MCUboot |
| UF2 bootloader | 0xF4000 - 0x100000 | unchanged |

XIAO nRF52840 Sense ([partition map](../Tracker/pm_static/mcuboot_xiao_ble.yml); its UF2 bootloader 0.6.1 comes with
the SoftDevice S140 and starts the app at 0x27000, where MCUboot goes; it writes up to 0xEA000 too):

| Region | Range | Size (KiB) |
|---|---|---|
| SoftDevice (unused, kept) | 0x0 - 0x27000 | 156 |
| MCUboot | 0x27000 - 0x33000 | 48 |
| Slot 0 (running app) | 0x33000 - 0x8F000 | 368 |
| Slot 1 (incoming image) | 0x8F000 - 0xEA000 | 364 |
| NVS settings, UF2 bootloader | as on the ProMicro | |

The XIAO board's own partition table names its UF2 bootloader `boot_partition`, so its overlays
(`Tracker/ota_xiao.overlay`, `Tracker/sysbuild/mcuboot_xiao.overlay`) replace the table.

- swap-using-move needs slot 0 at least one page larger than slot 1. It is configured
  for resumable swaps, but power interruption during a swap has not been tested on
  these boards. An unconfirmed test image is swapped back on the next boot.
- The UF2 bootloader (nice!nano 0.11.0) writes only 0x1000 - 0xEA000 (its `CURRENT.UF2` shows the
  range), so slot 1 ends there; `make tracker-ota-slot1` writes a test image for it over USB
  (`OTA_NOCONFIRM=1`: one that never confirms itself, for a rollback test).
- MCUboot is built for `nrf52840dk/nrf52840` (the tracker boards' defconfigs set tracker options it
  does not have) with this layout (`Tracker/sysbuild/mcuboot.overlay`, `.conf`); the app gets
  `Tracker/ota.conf` / `ota.overlay`. The OTA targets enable MCUboot; the plain
  tracker targets do not. The XIAO target selects its own layout files.

### The transfer (`ota_proto.h`, the same in `Tracker/src` and `Receiver/src`)

The receiver is the ESB PRX: it never transmits on its own, it attaches data to the ACK of a
tracker's frame. ESB sends a queued ACK payload with the next frame on that pipe and drops it at
the frame after.

1. **Offer.** `ota.py` sends `ota begin <size> <crc32> <board crc32> <tracker id>` on the
   receiver's console and checks the OTA protocol version in its answer. The receiver queues an
   OFFER on pipe 1 with every tracker frame; whichever tracker sends next gets it, and only the
   addressed one takes it. The offer carries the protocol version and a session number (1 - 15,
   new with every offer).
2. **Check.** The tracker takes the offer, or answers DONE with the reason and keeps tracking:
   another protocol version, another board (crc32 of `CONFIG_BOARD_TARGET`), too large, running
   image not confirmed yet (slot 1 still holds its fallback), already running this image (CRC32 of
   slot 0). The receiver reports it and `ota.py` stops; an offer nobody answers ends after 10 s.
3. **Fetch.** The tracker pauses tracking (and any sleep request waits), keeps the radio clock on
   and sends REQ(position) on pipe 7 (only it uses that pipe); position = session << 20 | offset,
   so nothing left over from an aborted update is taken for this one. The receiver queues exactly
   that chunk (28 bytes); it comes with the ACK of the tracker's next request, and the tracker asks
   again as soon as a frame is done without its chunk: about two frames per chunk,
   without waiting for a per-request retry timeout. A 5-second no-data stall still
   aborts the tracker transfer; the receiver has its own 10-second stall limit.
   (A look-ahead version, queueing the next chunk early, fell out of step whenever the receiver's
   handler ran late: 118 s instead of 26 s.)
4. **Stream.** `ota.py` streams the image into the receiver's 16 KB RAM window as HID OUT reports
   (`[0xF5][offset u24][length][59 bytes]`), polling `ota status` (received / asked) to stay within
   the window. The console carries only begin / stop / status: streaming the data through it
   jammed it (every line is echoed and answered). The status also says how the last update ended,
   as a log line on the console can be cut by the echo.
5. **Install.** The tracker writes slot 1 as it goes (pages erased progressively), checks the
   CRC32, marks a test swap, sends DONE and reboots; MCUboot checks the SHA-256 and swaps.
6. **Confirm.** The new image confirms itself after 200 frames the receiver acknowledged (a
   resting tracker sends few: that can take a minute; turned off before, it rolls back).

Changing a message means bumping `OTA_PROTOCOL` (`ota_proto.h`, `PROTOCOL` in `ota.py`). Every
version keeps the start of OFFER (`[type][tracker id][protocol]`) and all of DONE, so a tracker can
always say which version it speaks. Trackers from before protocol 1 (0.0.14 and older) do not
answer a protocol 1 offer: update them through a receiver of their time, or over USB.

### Protocol 1 wire messages

The [tracker header](../Tracker/src/ota_proto.h) and
[receiver header](../Receiver/src/ota_proto.h) must remain identical. Multi-byte
fields are little-endian; `position = session << 20 | offset`, with session 1–15.
Board identity is CRC32 of `CONFIG_BOARD_TARGET`.

| Message | Direction | Bytes |
| --- | --- | --- |
| OFFER 0xF1 | Receiver ACK, pipe 1 | type, id, protocol, session, size u32, CRC32 u32, board CRC32 u32 (16 total) |
| REQ 0xF3 | Tracker, pipe 7 | type, id, position u24 (5 total) |
| CHUNK 0xF2 | Receiver ACK, pipe 7 | type, position u24, up to 28 data bytes |
| DONE 0xF4 | Tracker, pipe 7 | type, id, status, protocol (4 total) |

DONE status: 0 installed, 1 flash error, 2 transfer CRC mismatch, 3 timeout,
4 protocol mismatch, 5 wrong board, 6 too large, 7 running image pending,
8 same image. The beginning of OFFER and the entire DONE layout are kept stable
across protocol revisions so refusals can identify a version mismatch.

## Failure cases

Tested on a ProMicro (0.0.15 / 0.0.16) unless marked otherwise.

| What happens | Result |
|---|---|
| Transfer stopped half way (offer stopped at 71 of 243 KB) | Slot 0 untouched: after 5 s without data the tracker gives up (`Update: last one transfer timed out`) and tracks again with its image; the next update went through |
| Image for another board | Refused, reported ("image for another board") |
| Image the tracker already runs | Refused, reported |
| Update offered while the running image is not confirmed | Refused, reported (with a test image that never confirms itself) |
| Tracker off, not paired, or firmware without protocol 1 | No answer: the offer ends after 10 s, reported |
| Other protocol version | Refused, reported with both versions (not tested: needs a second version) |
| Corrupted transfer | CRC32 mismatch: not installed; a damaged image in slot 1 is refused by MCUboot's SHA-256 check (not tested) |
| Resting tracker on battery during an update | Sleep and power off wait for the update (not tested on battery: all tests ran on USB power) |
| Power lost during the swap | Expected to resume with swap-using-move; not tested on these boards |
| New image does not confirm itself | MCUboot swaps the old one back on the next boot (tested over USB and over the radio) |
| New image hangs without crashing | No watchdog: needs a power cycle; rollback applies only while the test image is still unconfirmed |
| MCUboot or both slots broken | UF2 bootloader: double-tap reset, flash the `_OTA.uf2` over USB |
| Settings | NVS area untouched by every step: pairing and calibration survive (tested) |

## Open

- No image authentication: CRC32 and SHA-256 are integrity checks, not proof of an
  authorized sender. Images are deliberately unsigned; signing is a separate design
  change, not enabled by these instructions.
- No version downgrade check: an older compatible image can be installed. This is
  distinct from MCUboot reverting an unconfirmed test image.
- Finish the untested cases in the table on the test tracker, with USB recovery
  available. See [testing](TESTING.md).
- Historical speed was about 9 KB/s. Profile a new transfer before choosing an
  optimization; larger ESB payloads would require coordinated protocol changes.
- MCUmgr/SMP was considered but not implemented. This repository has no SMP-over-ESB
  transport; adopting it would be a separate design and interoperability project.
