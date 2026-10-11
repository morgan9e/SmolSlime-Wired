# Project handoff

[Documentation index](README.md) · Last reconciled with local source: 2026-10-10 (KST).

This is the short entry point for resuming work. Hardware observations below come
from the October 9–10 sessions; no devices were queried during the documentation
cleanup. The complete original memory is preserved in the
[dated archive](archive/MEMORY-2026-10-10.md).

## Goal and decisions

- Fully wireless body tracking with minimal cables and a stock SlimeVR PC server.
- Nordic nRF52840 Dongle receiver; six ProMicro stacked SPI trackers. XIAO Sense is
  a spare/test board, last recorded as ID 6.
- Stay on ESB at 2 Mbps. The dongle substantially improved reception over the old
  ProMicro receiver. Recorded load tests implicate collisions in the remaining loss;
  they do not establish performance in every body pose.
- Keep the 10 ms moving-loop setting, per-device retry delay, and newest-frame-wins
  behavior. The no-flush experiment was reverted after worse measured loss.
- MCUboot OTA with rollback; unsigned images; tracking pauses during transfer.
  Stream PC → dongle → tracker instead of storing the whole image on the dongle.
- Wired chains, relays and the ESP32 hub are historical work on `wired`.
  Receiver relocation remains conditional on [measurements](TESTING.md).

## Current source and last hardware state

`main` contains `Tracker/`, `Receiver/`, `tools/` and the root Makefile. Origin is
`https://github.com/morgan9e/SmolSlime-mod`. The old `SlimeVR-Hub` path and
`SmolSlime-Wired` remote in archived notes are historical.

The last recorded installations were 0.0.15 on IDs 0–4 and 6, 0.0.16 on test tracker
5, and OTA protocol 1 on the dongle. These are observations, not a live inventory.
Verify by device address using [hardware identification](HARDWARE.md).

Radio updates, MCUboot swap/rollback, and several refusal/abort cases were tested.
All-six-moving tracking on 0.0.3 or later, battery-powered OTA sleep behavior, and
the charge estimate still lack recorded hardware validation.

## Working rules

- Commit the current work before changing it. One commit per removal; do not push
  unless asked. Preserve unrelated user changes and local configuration.
- Propose firmware changes outside the user's requested scope before making them.
- Test new images on the spare/test tracker first (recorded ID 5, right ankle,
  address `628DCFC6318A`). Receiver firmware can only be tested on the receiver;
  flash it last when a change spans both sides.
- Run tools and flashing for the user when requested. Show progress for long jobs
  and report or beep on completion. Run only one flashing watcher at a time.
- Use `0.0.N` tags, bumping before each rebuild that will be flashed.
- Identify boards by device address, not COM port; IDs can change after pairing.
- Keep the hub minimal: relay existing data. The recorded exception permits raw
  receiver diagnostic packets on a second UDP port. Do not add hub features merely
  to support a PC tool.
- Separate tools by job; the viewer uses USB or network, one at a time.
- Keep configuration explicit per board and avoid unnecessary reflashing of working
  devices. Receiver naming should not include the tracker-specific word "stacked".
- Keep one repository, with upstream histories as subtrees; no nested repositories
  or submodules. Preserve useful history.
- Verify results, including generated/ignored files when relevant. Do not read
  other sessions' transcripts without a request.

## TDMA prototype (2026-10-11)

The user requested initial TDMA code. An opt-in receiver-beacon scheduler now
assigns eight 1 ms slots in a nominal 10 ms frame. See [TDMA](TDMA.md) for isolated
build targets, diagnostics, software timing limits and acceptance tests. Ordinary
builds remain legacy ESB. The prototype keeps HFXO/RX active and uses no retries
for tracking. Later hardware checks are recorded in [TDMA](TDMA.md); they do not
establish improved reception or range. User identifies ID 7 as the left foot.

### OTA repair in progress, 2026-10-11

- User-facing tracker names must use the last two hexadecimal address characters:
  7A, 91, 59, C8, 20, 8A (test unit), 78; XIAO E4 is excluded for now.
- User requested OTA restoration and a temporary bypass of idle suppression.
  `bench on/off/status` now controls a volatile sensor-test mode, off after reboot.
  It requires the tracker USB console and does not change TDMA scheduling.
- Receiver 0.0.24 is installed. 8A was USB-repaired to 0.0.21, then successfully
  updated by radio to 0.0.22 with MCUboot confirmation. Wrong-board refusal and
  interrupted-transfer recovery were verified. A second radio transfer to 0.0.25
  completed, and receiver diagnostics reported that version; its final USB image
  confirmation remains to be checked after hub recovery.
- 0.0.25 removes an offer cache that blocked retries after receiver session reuse.
  Source commit: `4de1a01`. Images: `dist/0.0.25/tdma/`.
- The other six ProMicros still need a one-time USB repair from 0.0.19 using
  the combined MCUboot UF2; their installed code cannot receive OTA offers.
  E4 remains excluded. Do not claim fleet deployment or enable bench testing
  until each device's version and confirmation have been checked.
- The hub disappeared during this work, returned with six tracker ports and a
  Windows USB port-reset error, then tracker console reads stalled before the
  next flash. No new USB flash was started on 7A. User was asked to reconnect
  the hub. Re-enumerate devices by serial/address before continuing.

## Next work

1. Record firmware versions and a six-tracker moving baseline on battery, then real
   worn poses. Use [the test procedure](TESTING.md) before changing radio behavior.
2. Finish the untested OTA cases listed in [OTA](OTA.md), starting with a resting
   battery-powered test tracker. Verify confirmation after reboot.
3. Validate the charge estimate on one tracker; the configured 100 mA is an
   assumption, not a measured charger current.
4. Confirm which two ProMicros have damaged/missing antennas. IDs 0 and 5 were
   suspected from RSSI, never physically confirmed. Compare boards under equal
   power, firmware and placement conditions.
5. Consider 80 Hz, less frequent info packets, TDMA, or receiver placement only
   after the baseline identifies the remaining problem. These are proposals.

Old cleanup requests about force-pushing branches, restoring tags and deleting
backup folders are preserved in the archive, not standing instructions. Inspect
current Git state and the actual files before reviving any of them.

## Where details live

| Topic | Document |
| --- | --- |
| Board addresses, USB identities, bootloaders | [Hardware](HARDWARE.md) |
| Builds, versioning, source map and pitfalls | [Development](DEVELOPMENT.md) |
| Flashing, rollback, transfer protocol and layouts | [OTA](OTA.md) |
| Monitoring, viewer and configuration | [PC tools](TOOLS.md) |
| Packet layouts and transport boundaries | [Protocol reference](PROTOCOL.md) |
| Validation and conditional receiver relocation | [Testing](TESTING.md), [receiver placement](RECEIVER_PLACEMENT.md) |
| Measurements, wired era, timeline and original plans | [Archive index](archive/README.md) |
