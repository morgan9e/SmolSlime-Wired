# SlimeVR nRF: stock firmware with small changes

Stock SlimeVR nRF receiver and tracker firmware, with only the changes this setup uses: an
nRF52840 Dongle as the receiver and ProMicro stacked SPI trackers.

| Path | What |
|------|------|
| `Receiver` | SlimeVR-Tracker-nRF-Receiver, git subtree of upstream `main` (e3d66d5) |
| `Tracker` | SlimeVR-Tracker-nRF, git subtree of upstream `main` (cbfff16) |
| `tools/slime_link.py` | link statistics per tracker from the receiver over USB-HID; runs alongside SlimeVR Server |
| `tools/dfu_zip.py` | DFU package from a `.hex`, for `nrfutil device program` on the dongle |
| `tools/serial_monitor.py` | desktop serial monitor for the consoles (needs pyserial) |

## Changes to upstream

- Receiver: link diagnostics. `diag on` on its console logs every second, per tracker, frames
  received and lost (from the frame sequence numbers), repeats, longest silence and RSSI; for the
  radio, good/bad CRC counts and the noise floor. Receiving and the HID output are unchanged.
- Tracker: sends at 100 Hz while moving (`CONFIG_SENSOR_UPDATE_TIME_MS`, upstream 6 ms, about
  166 Hz), and each tracker gets its own ESB retransmit delay, so trackers whose frames collide do
  not retry in lockstep. With six stock trackers moving, the channel was busy 44% of the time
  and 40-65% of the frames were lost.

## Build

From WSL/Linux, with the nRF Connect SDK workspaces named in the `Makefile`:

    make            # dist/SlimeNRF_ProMicro_StackedSPI_Tracker.uf2, dist/SlimeNRF_Nordic_Dongle_Receiver.{hex,zip}
    make test

## Flash

- Tracker: `dfu` on its console (or double-tap reset), then copy the `.uf2` to the drive.
- Receiver: `dfu` on its console starts the dongle's bootloader; then the `.hex` with nRF
  Connect Programmer, or
  `nrfutil device program --firmware dist/SlimeNRF_Nordic_Dongle_Receiver.zip --serial-number <bootloader's>`
  (`nrfutil device list` shows it).

## Upstream updates

    git subtree pull --prefix=Receiver https://github.com/SlimeVR/SlimeVR-Tracker-nRF-Receiver main
    git subtree pull --prefix=Tracker https://github.com/SlimeVR/SlimeVR-Tracker-nRF main
