# SlimeVR-Hub

On-body hub for SlimeVR nRF trackers: ankles are wired to their thighs, a receiver and an
ESP32 hub ride on the hip, and the hub sends every tracker to the stock SlimeVR Server over
Wi-Fi. No PC program runs in the data path. The design is in
`docs/On-Body_Hub_Implementation_Plan_v3.md`.

```
ankle ── cable (D8/D9) ── thigh ──┐
thigh, hip, chest ──── radio (ESB) ┴─► receiver ── UART (D0 → D7) ──► hub (ESP32-C6) ── Wi-Fi ──► SlimeVR Server
```

## Layout

| Folder | Contents |
| --- | --- |
| `src/Tracker` | SlimeVR-Tracker-nRF fork (wired link, relay and hub modes). Details: `src/Tracker/WIRED.md` |
| `src/Receiver` | SlimeVR-Tracker-nRF-Receiver fork (UART mirror, link diagnostics). Details: `src/Receiver/HUB.md` |
| `src/Hub` | ESP32 hub firmware (ESP-IDF). Details: `src/Hub/README.md` |
| `slime_view.py` | live view of the trackers' data on the PC |
| `slime_diag.py` | radio link diagnostics from the receiver, relayed by the hub to UDP 6971 (`diag hub on` on the receiver) |
| `slime_link.py` | link statistics per tracker (rate, RSSI, gaps, offline, battery) from any nRF receiver over USB-HID, stock firmware included; runs alongside SlimeVR Server |
| `dist` | flashable images, written by `make` |
| `docs` | the plan, and a backup patch of earlier tracker UART work |

One git repository. `src/Tracker`, `src/Receiver` and `src/Hub` were separate repositories;
their full histories (including the upstream SlimeVR commits of the two forks) are merged
into `main`, rewritten so every commit has its files under its folder. Also kept:

- branch `hub/full-wired`: the ESP32 hub reading wired tracker chains directly (no receiver), not merged
- tag `hub-mode-ok`: the tracker firmware of the working hub-mode setup (`dist/hub-mode/SETUP.md`)
- tag `tracker/0.0.1`: upstream SlimeVR-Tracker-nRF `tdma` branch (time-slotted radio), for reference

Upstream: https://github.com/SlimeVR/SlimeVR-Tracker-nRF and
https://github.com/SlimeVR/SlimeVR-Tracker-nRF-Receiver.

## Firmware

| Image in `dist/` | Flash on | How |
| --- | --- | --- |
| `SlimeNRF_ProMicro_StackedSPI_Wired_Tracker.uf2` | every tracker | double-tap reset, copy to the UF2 drive |
| `SlimeNRF_ProMicro_HubReceiver.uf2` | receiver ProMicro | double-tap reset, copy to the UF2 drive |
| `slime_hub-xiao-esp32c6.bin` | XIAO ESP32-C6 | `python -m esptool --chip esp32c6 -p COMx write_flash 0x0 slime_hub-xiao-esp32c6.bin` |
| `slime_hub-xiao-esp32s3.bin` | XIAO ESP32-S3 (alternative hub) | same, with `--chip esp32s3` |

## Setup

1. **Trackers.** Flash the tracker image on all of them. On each ankle, in its USB console:
   `write_config connection_over_uart 1`, then `reboot`. The ankle is now in *wired mode*
   (radio off). All others stay in *relay mode* (default) and pair with the receiver as usual.
2. **Ankle cable**, straight through: D8 to D8 (data), D9 to D9 (ground reference).
   A 470R-1k resistor in series on D8 is recommended.
3. **Pair each ankle once, through its thigh:** receiver in pairing mode (one press of its
   button), then `pair_wired` on the thigh (or 6 button presses). Press the receiver button
   again to leave pairing mode. `wired` on a tracker shows the link state.
4. **Hub.** Receiver D0 (P0.08) to hub D7, plus GND. On the hub's USB console:
   `wifi "SSID" password`. Accept each new tracker once in the SlimeVR GUI.
5. **Optional link diagnostics:** `diag hub on` on the receiver's USB console (stored), then
   `python slime_diag.py` on the PC (`--log diag.csv` to keep them).

## Building

Run in WSL or Linux from this folder:

```
make            # tracker, receiver and ESP32-C6 hub into dist/
make tracker    # or: receiver, hub, hub-s3
make test       # host tests: hub decoder/framing, tracker wired link
make clean
```

The Makefile expects the toolchains installed during development; override any of them on
the command line (`make NCS_TRACKER=... tracker`):

| Variable | Default | What |
| --- | --- | --- |
| `NCS_TRACKER` | `~/ncs-tracker` | west workspace, NCS `v3.1-branch` (tracker `west.yml`) |
| `NCS_RECEIVER` | `~/ncs-ws` | west workspace, NCS `v3.3.0-preview3-branch` (receiver `west.yml`) |
| `ZEPHYR_SDK` | `~/zephyr-sdk-0.17.4` | Zephyr SDK with the ARM toolchain |
| `IDF_PATH` | `~/esp/esp-idf` | ESP-IDF v5.5.5 |
| `BUILD` | `~/build/slimevr-hub` | build directories (kept off `/mnt/c` for speed) |
| `HOST_CC` | `cc`, `gcc`, or zig | C compiler for the host tests |

## Status

- Receiver mirror and hub: working on hardware.
- Wired ankle to thigh link: working on hardware (UART, pairing through the thigh).
- Still to measure (plan steps 4-5): thigh gap statistics while relaying versus not
  (D8 is a "low frequency I/O only" pin), and a full-body session.
