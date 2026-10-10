# Project memory

What this project is, what was decided and why, what was measured, and what bit us, for anyone
(person or Claude session) picking it up later. State as of 2026-10-10. The how-tos live in
`README.md` and `docs/OTA.md`; this file links to them instead of repeating them.

## Goal

Full-body tracking with SlimeVR nRF trackers that is fully wireless and stays close to stock
firmware: stock SlimeVR Server on the PC, an nRF52840 Dongle as the receiver, ProMicro trackers,
and firmware updates over the radio instead of USB.

How it got here:

1. Trackers lost many frames with a ProMicro board as the receiver. First tried: BLE instead of
   ESB, then a wired setup (ankles cabled to thighs, an ESP32 hub on the hip sending everything
   over Wi-Fi, see branch `wired`). It worked, but everything ended up wired, which was not the
   point ("Of course I want to go full wireless").
2. The official nRF52840 Dongle arrived: the original problem was mostly the ProMicro receiver's
   antenna. Back to stock firmware, wireless.
3. Remaining loss was collisions between trackers: capped the rate, spread the retries.
4. A clean branch with only what this setup uses (now `main`), then OTA updates.

## Repository

One repository at `C:\Users\Morgan\Lab\SlimeVR-Hub` (one folder, branch `main` checked out).
Remote `origin` = https://github.com/morgan9e/SmolSlime-Wired.

| Branch | What |
|---|---|
| `main` | This setup: stock Receiver and Tracker as git subtrees (`Receiver/`, `Tracker/`) with small changes, PC tools, OTA. Started 2026-10-10 as an orphan branch (`clean`), renamed to `main` the same day |
| `wired` | The earlier wired/hub work (was `main` until 2026-10-10): `src/Tracker` and `src/Receiver` forks, `src/Hub` (ESP32 hub firmware), its own tools. Tag `hub-mode-ok` = working hub-mode setup (`dist/wired/hub-mode/SETUP.md`) |
| `hub/full-wired` | ESP32 hub reading wired tracker chains directly, no receiver. Not merged |
| `charge-estimate` | Merged into `main` (battery percentage while charging); can be deleted |

- `origin/main` on GitHub still holds the `wired` history. Nothing from the new `main` has been
  pushed. Pushing it means a force push; push `wired` as its own branch first.
- Subtree upstreams: `Receiver` from SlimeVR-Tracker-nRF-Receiver `main` (e3d66d5), `Tracker`
  from SlimeVR-Tracker-nRF `main` (cbfff16). Pull with `git subtree pull` (README). The clones
  they came from are in `docs/Ref/` (ignored).
- `dist/` (ignored) holds the built images; `dist/wired/` the images of the `wired` branch.
- `docs/Quest-Hosted SlimeNRF Receiver — Implementation Spec*.md`: the user's own drafts, not
  committed yet.

## Hardware

### Boards

Identify boards by device address (`info` on the console), never by COM port. Tracker ids are
the receiver's pairing order.

| Id | Role | Board | Device address | USB serial number |
|---|---|---|---|---|
| 0 | chest | ProMicro stacked SPI | D235DBBB9B7A | 2D2949174DDE772E |
| 1 | left thigh | ProMicro stacked SPI | 847BEBC50791 | A4C462FDB5E01827 |
| 2 | right thigh | ProMicro stacked SPI | DEE876FA4759 | 823B25D6352D1CF7 |
| 3 | waist / hip | ProMicro stacked SPI | 6DF7EC0CB8C8 | 94530FD952246331 |
| 4 | left ankle | ProMicro stacked SPI | D81D4F94B220 | 0D60E9D03DC04A2C |
| 5 | right ankle | ProMicro stacked SPI | 628DCFC6318A | 629BE058486D6B6E |
| 6 | (not worn) | Seeed XIAO nRF52840 Sense | 157A3123FAE4 | E6A1176A6A36031A |
| - | receiver | Nordic nRF52840 Dongle | BCF6929F4E52 | F9F258A5111356F3 (its bootloader: FCF6929F4E52) |

- Two ProMicros have no antenna; in the link diagnostics ids 0 and 5 were the weakest.
- Tracker 5 (right ankle) is the test tracker: new images go there first.
- The XIAO is USB powered and not used for tracking; it has OTA support anyway.
- On the `wired` branch, a ProMicro (04A6D591E478, with an LSM6DSV) was the receiver/hip and an
  ESP32-C6 the hub (192.168.0.56).
- All boards can sit on one USB hub at once: `tools/flash_usb.py` flashes all of them in one go.

### USB and bootloaders

| | USB id | Bootloader | Notes |
|---|---|---|---|
| ProMicro trackers | 1209:7692 | Adafruit UF2 0.11.0 (nice!nano), no SoftDevice | UF2 writes only 0x1000 - 0xEA000 |
| XIAO | 1209:7692 | Adafruit UF2 0.6.1 with SoftDevice S140 7.3.0 | App starts at 0x27000; UF2 writes up to 0xEA000 too |
| Dongle receiver | 1209:7690 | Nordic Open DFU bootloader | `dfu` on its console starts it; flash with nrfutil |

The UF2 write limit is not documented anywhere: read it from the range of `CURRENT.UF2` on the
bootloader drive. Settings (pairing, calibration; NVS) are at 0xEE000 - 0xF4000 on both boards.

## Toolchain

- Builds run in WSL: `make` in the repo (via `/mnt/c/Users/Morgan/Lab/SlimeVR-Hub`).
- Tracker: NCS v3.1 in `~/ncs-tracker`. Receiver: NCS v3.3 in `~/ncs-ws` (needs newer nrfx).
  Zephyr SDK `~/zephyr-sdk-0.17.4`, west venv `~/ncs-ws/.venv`. All in the `Makefile`.
- The receiver build sets `CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n`: the dongle board's own USB
  console clashes with the receiver's.
- PC tools: Windows Python 3.14 with `pyserial` and `hidapi`.
- nrfutil: use `C:\Users\Morgan\AppData\Roaming\nrfconnect\nrfutil-sandboxes\8.1.1\device\2.19.3\bin\nrfutil-device.exe`
  (`list`, `program --firmware dist/SlimeNRF_Nordic_Dongle_Receiver.zip --serial-number FCF6929F4E52`).
  The plain `nrfutil.exe` there has no `device` command installed.

### Versions

- Tracker versions are git tags `0.0.N`. For every rebuild that gets flashed:
  1. Commit.
  2. Run `python tools/bump_version.py`: patch + 1, an annotated tag on HEAD. It refuses a dirty
     tree; the build would be marked dirty otherwise.
  3. `make tracker-ota`.
- The firmware shows `0.0.N-<branch>+<commits since tag>` in `info` and in slime_link's firmware
  column.
- The Makefile hands `git describe` to the build in `FW_GIT_DESCRIBE` / `FW_GIT_BRANCH`. Sysbuild
  does not pass `GIT_EXECUTABLE` on to the images, and WSL git could not read a Windows
  worktree.
- Tag 0.0.8 is a rollback test image only. 0.0.1 / 0.0.2 were renamed from 0.1.0 / 0.2.0.
  `tracker/0.0.1` is upstream's `tdma` branch, kept for reference.

## Changes to upstream (on `main`)

Receiver:

- **Link diagnostics.** `diag on` logs, every second and per tracker: frames received and lost
  (from sequence numbers), repeats, longest silence and RSSI; also CRC counts and noise floor.
- **OTA relay.**
  - Console `ota begin|status|stop`.
  - HID OUT reports (`0xF5`) carry the image from the PC.
  - Pipe 7 carries the transfer to the tracker.
  - See `docs/OTA.md`.

Tracker:

- **Rate cap.** `CONFIG_SENSOR_UPDATE_TIME_MS` = 10, about 100 Hz on paper (upstream 6 ms). The
  IMU FIFO is quantized, so 10 ms gives 2 samples per loop, about 120 frames/s in practice.
- **Spread retries.** Each tracker has its own ESB retransmit delay,
  `600 + (hash of DEVICEADDR & 7) * 125` µs, so colliding trackers do not retry in lockstep.
- **Flush kept.** A new frame replaces an unsent old one: newest wins. 0.0.2 tried keeping old
  frames for their retries and lost more; reverted in 0.0.3.
- **OTA.**
  - MCUboot build (`make tracker-ota`, `make tracker-ota-xiao`).
  - Update thread (`Tracker/src/ota.c`).
  - Self-confirm after 200 frames the receiver acknowledged.
  - Sleep requests wait while an update runs.
- **Charging estimate.** While plugged in, the battery percentage is estimated from the charge
  put in: `BATTERY_CHARGE_ESTIMATE_CAPACITY_MAH` (180) and `BATTERY_CHARGE_ESTIMATE_CURRENT_MA`
  (100). The voltage reads high while charging.

## Measurements and findings

- **The first loss was the antenna.** The original loss came mostly from the ProMicro receiver's
  antenna; the Dongle fixed that.
- **Collisions caused the rest.** With all trackers moving:
  - Stock trackers send about 240 frames/s each (gyro at 240 Hz, FIFO threshold).
  - With six moving, the channel was busy 44% of the time and 40-65% of frames were lost.
  - The rate cap plus spread retries took loss from about 53% to about 25%.
  - Not yet measured with every tracker on the current firmware.
- **More options against collisions:** 80 Hz, TDMA (upstream `tdma` branch, tag
  `tracker/0.0.1`), or a second dongle on another channel.
- **ESB payload size.** The maximum ESB payload is 32 bytes. Keep it: raising it changes the
  length field and breaks every tracker on the receiver.
- **OTA speed.** About 248 KB in 21 - 27 s (about 9 KB/s), two radio frames per 28-byte chunk.
  The PC side is not the limit.
- **Confirmation at rest.** A resting tracker sends few frames, so a new image can take up to a
  minute to reach 200 acknowledged frames and confirm itself. Switched off before that, it rolls
  back: safe, but surprising.

## OTA in short (details: docs/OTA.md)

- **Once per tracker, over USB:** flash `_OTA.uf2` (MCUboot + app).
  `python tools/flash_usb.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA.uf2` does every
  tracker on USB.
- **Then for each update:**
  `python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker <id>`
- **Never flash the plain tracker UF2** on an OTA tracker: it overwrites MCUboot. The `_OTA.uf2`
  repairs that.
- **Protocol versions.** OTA protocol 1 since 0.0.15. Trackers on 0.0.14 or older do not answer
  a protocol 1 offer: update them over USB. Changing a message means bumping `OTA_PROTOCOL` in
  both `ota_proto.h` copies (identical files in `Tracker/src` and `Receiver/src`) and
  `PROTOCOL` in `ota.py`.
- **Firmware now:** trackers 0 - 4 and the XIAO run 0.0.15, tracker 5 runs 0.0.16. The tracker
  code is the same in both; 0.0.16 only changed the receiver and `ota.py`. The receiver runs
  protocol 1.
- **Decided:**
  - MCUboot with rollback (swap using move).
  - Unsigned images: MCUboot still checks SHA-256, the tracker a CRC32 of the transfer.
  - Tracking pauses during an update.
- **SMP** (Zephyr's standard management protocol) was considered and skipped. It has no ESB
  transport and would be slower over 32-byte payloads, for standard tools that this setup does
  not need.

## Rules (from the user)

- **Test on the spare first.** New images go to a spare tracker first (tracker 5), never first to
  the receiver or the hip. The receiver can only be tested on itself: flash it last, after the
  trackers.
- **Commit, don't push.** Commit only; no pushes unless asked.
- **Watch the system workqueue.** Never run NVS writes or printing on the tracker's 256-byte
  system workqueue: that caused boot loops on the `wired` branch.
- **The hub stays minimal (`wired` branch).** Never change `src/Hub` to serve PC tools; decode
  what it already sends. The one approved exception is the raw pass-through of the receiver's
  diag packets.
- **Propose firmware changes first.** Firmware changes the user did not ask for are proposed
  first, not made.
- **Versioning.** Patch-level versions (0.0.N), bumped for every flashed rebuild.
- **Long jobs.** For long runs (flashing, OTA), show progress and beep or report when done.

## Pitfalls hit (so they are not hit again)

- **MCUboot build.**
  - It is built for `nrf52840dk/nrf52840`: the tracker boards' defconfigs set tracker options
    MCUboot does not know, and Kconfig warnings are fatal.
  - Each board's layout is written three times and they must agree:
    `Tracker/pm_static/mcuboot_*.yml`, `Tracker/sysbuild/mcuboot*.overlay`,
    `Tracker/ota*.overlay`.
  - The XIAO board names its UF2 bootloader `boot_partition`, so its overlays replace the
    partition table instead of adding to it.
- **Sysbuild per-image options.** `-D<image>_EXTRA_CONF_FILE`, `_BOARD` and
  `_DTC_OVERLAY_FILE` work per image; `GIT_EXECUTABLE` does not get passed.
- **ESB ACK payloads.** The receiver (PRX) only talks through ACK payloads. A queued payload goes
  out with the next frame on that pipe, and the frame after drops it. The receiver cannot flush
  while receiving.
  - Queueing the next chunk early fell out of step: 118 s per update.
  - Queueing exactly the requested chunk takes 26 s.
- **Receiver console.**
  - It echoes every line. Status polls cut log lines, so read results from `ota status`, not
    from log lines.
  - Streaming data through it jammed it; data goes over HID OUT.
  - The argument parser rejects a full argv array (`argv[8]` now).
- **HID.** hidapi opens the receiver shared on Windows: `slime_link.py` and `ota.py` run
  alongside SlimeVR Server. `slime_view.py --usb` needs the server closed.
- **Worktrees.** A branch can be checked out in only one worktree. WSL git cannot read a worktree
  made by Windows git.
- **Editing from Claude's shell.** Heredocs through the Bash tool lose tabs and mangle `\r`,
  `\b` and line continuations. Edit tabbed C/Makefile code with the Edit tool, or write the text
  to a file first.
- **Line endings.** `git stash` round trips can turn LF into CRLF in the working copy. Keep a
  byte copy of uncommitted user files before stashing.

## Open

- **Battery test of OTA.** Never run: a resting tracker on battery should hold off sleep during
  an update. Tracker 5 off USB, still for 30 s, then update.
- **Untested OTA cases:** another protocol version, a corrupted transfer, power loss during the
  swap.
- **No watchdog:** a new image that hangs (without crashing) needs a power cycle before it rolls
  back.
- **Collision measurement:** all trackers moving on the current firmware.
- **README.md** does not mention OTA or `flash_usb.py` yet.
- **GitHub:** decide on pushing `main` and `wired`, see Repository.
- **`wired` branch bug, not fixed:** an explicit `pair` on a relay forgets the previous receiver,
  leaving stale wired ids.
- **Cleanup:**
  - An empty `C:\Users\Morgan\Lab\SlimeVR-Hub-clean\tools` folder stayed behind, held by an open
    terminal. Delete it.
  - The `charge-estimate` branch is merged and can be deleted.
