# Tracker firmware updates over the radio (OTA)

Status: works on the ProMicro (stacked SPI) trackers, all six of them run it, and on the XIAO
nRF52840 Sense. Tested: MCUboot boot, test swap, rollback, updates over the radio (242 KB in
21 - 27 s per tracker; 0.0.15 went to all seven one after the other), and the failure cases below.

Decided: MCUboot with rollback; unsigned images (MCUboot still checks each image's SHA-256, and the
tracker a CRC32 of the transfer); the tracker pauses tracking while it receives an update.

## Use

    make tracker-ota      # dist/..._Tracker_OTA.uf2 (MCUboot + app) and _OTA_app.bin (the update)
    make tracker-ota-xiao # the same for the XIAO (SlimeNRF_XIAO_Sense_Tracker_OTA*)
    make receiver         # the dongle needs this repository's receiver (ota commands, HID OUT)

1. Once per tracker, over USB: flash `_OTA.uf2`. With the trackers plugged in (a USB hub takes
   them all), `python tools/flash_usb.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA.uf2`
   does each in turn (`dfu`, copy to its drive, show the new version; `--only 0,3` for some).
   Pairing and calibration are kept.
2. After that, for each new version: commit, `python tools/bump_version.py`, `make tracker-ota`, and

       python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker <id>

   with the tracker on and paired (its id: the receiver's `list` order, or slime_link). It shows a
   progress bar; the tracker reboots into the new image, which confirms itself once the receiver
   has acknowledged 200 of its frames. `info` on a tracker shows `Image: confirmed|pending` and
   MCUboot's next swap; slime_link shows each tracker's version over the air.

A USB flash of a tracker running MCUboot must use the `_OTA.uf2` (never the plain tracker UF2,
which would overwrite MCUboot; flashing the `_OTA.uf2` afterwards repairs that).

## How it works

### Tracker flash layout: MCUboot between the UF2 bootloader and the app

ProMicro (`Tracker/pm_static/mcuboot_nrf52840_uf2.yml`; MCUboot 24 KB, app 242 KB):

| Region | Range | Size |
|---|---|---|
| MBR | 0x0 - 0x1000 | 4 KB |
| MCUboot | 0x1000 - 0xD000 | 48 KB |
| Slot 0 (running app) | 0xD000 - 0x7C000 | 444 KB |
| Slot 1 (incoming image) | 0x7C000 - 0xEA000 | 440 KB |
| (unused) | 0xEA000 - 0xEE000 | 16 KB |
| NVS settings | 0xEE000 - 0xF4000 | 24 KB, as without MCUboot |
| UF2 bootloader | 0xF4000 - 0x100000 | unchanged |

XIAO nRF52840 Sense (`Tracker/pm_static/mcuboot_xiao_ble.yml`; its UF2 bootloader 0.6.1 comes with
the SoftDevice S140 and starts the app at 0x27000, where MCUboot goes; it writes up to 0xEA000 too):

| Region | Range | Size |
|---|---|---|
| SoftDevice (unused, kept) | 0x0 - 0x27000 | 156 KB |
| MCUboot | 0x27000 - 0x33000 | 48 KB |
| Slot 0 (running app) | 0x33000 - 0x8F000 | 368 KB |
| Slot 1 (incoming image) | 0x8F000 - 0xEA000 | 364 KB |
| NVS settings, UF2 bootloader | as on the ProMicro | |

The XIAO board's own partition table names its UF2 bootloader `boot_partition`, so its overlays
(`Tracker/ota_xiao.overlay`, `Tracker/sysbuild/mcuboot_xiao.overlay`) replace the table.

- swap-using-move: power-fail safe, needs slot 0 at least one page larger than slot 1. An image
  that does not confirm itself is swapped back on the next boot.
- The UF2 bootloader (nice!nano 0.11.0) writes only 0x1000 - 0xEA000 (its `CURRENT.UF2` shows the
  range), so slot 1 ends there; `make tracker-ota-slot1` writes a test image for it over USB
  (`OTA_NOCONFIRM=1`: one that never confirms itself, for a rollback test).
- MCUboot is built for `nrf52840dk/nrf52840` (the tracker boards' defconfigs set tracker options it
  does not have) with this layout (`Tracker/sysbuild/mcuboot.overlay`, `.conf`); the app gets
  `Tracker/ota.conf` / `ota.overlay`. Only `make tracker-ota` enables MCUboot.

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
   again as soon as a frame is done without its chunk: about two frames per chunk, no timeouts.
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
| Power lost during the swap | swap-using-move resumes the swap on the next boot (MCUboot's guarantee, not tested here) |
| New image does not confirm itself | MCUboot swaps the old one back on the next boot (tested over USB and over the radio) |
| New image hangs without crashing | Stays until switched off and on (no watchdog), then rolled back |
| MCUboot or both slots broken | UF2 bootloader: double-tap reset, flash the `_OTA.uf2` over USB |
| Settings | NVS area untouched by every step: pairing and calibration survive (tested) |

## Settings over the radio

The same transport carries the tracker's settings (`list_config` / `write_config` on its console),
so they can be read and changed without a USB cable. Any build of this repository's tracker has it
(`CONFIG_TRACKER_REMOTE_CONFIG`, default on), with or without MCUboot; the receiver must be this
repository's too.

    python tools/remote_config.py --tracker 3 list
    python tools/remote_config.py --tracker 3 get sensor_update_time_ms
    python tools/remote_config.py --tracker 3 set sensor_update_time_ms 10
    python tools/remote_config.py --tracker 3 reset radio_tx_power

On the receiver's console that is `cfg <tracker id> get|set|reset <setting id> [value]` and
`cfg status`; the setting ids are `list_config`'s, which `remote_config.py` maps from names.

1. The receiver queues `CFG_REQ` (`[type][tracker id][seq][op][setting id u16][value i32]`) on
   the ACK of every tracker frame on pipe 1, as it does an update offer; only the addressed
   tracker takes it.
2. The tracker carries it out on its own thread (a write goes to flash), with the same checks as
   `write_config`, and answers `CFG_RESP` (`[type][tracker id][seq][status][setting id u16]
   [value i32]`, value = the setting afterwards) on the OTA pipe.
3. The receiver keeps queuing the request until the answer with its seq arrives, at most 3 s. The
   tracker carries out each seq once and answers repeats from its last answer, so a lost answer
   never applies a write twice.

An update offer and a settings request share the ACKs on pipe 1, so the receiver takes one at a
time (`ota begin` ends a waiting request, `cfg` is refused during an update), and a tracker busy
with an update answers `busy`. Some settings take effect at once (`sensor_update_time_ms`),
others on the tracker's next start (`radio_tx_power`, the sensor rates). A setting that cuts the
tracker off the receiver has to be undone over USB.

## Open

- No authentication: anyone who knows the receiver's address could offer an image (MCUboot
  signing would close it, with a key to keep).
- Speed: 26 s for 242 KB (about 9 KB/s). The PC side is not the limit; more would take larger ESB
  payloads, which change the radio format for every tracker on the receiver.
- The standard alternative for the transfer is MCUmgr/SMP (Zephyr's image management, with the
  usual PC and phone tools), which has no ESB transport: it would need an SMP transport over ESB
  with the same polling, relayed by the receiver.
