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

The latest recorded installations are six ProMicros on radio-only 0.0.27 and
receiver 0.0.26; 91 remains last-known normal 0.0.25 and E4 last-known 0.0.19.
See the current experiment below and [hardware identification](HARDWARE.md).
These are observations, not a live inventory; verify by device address.

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

### OTA repair deployment, 2026-10-11

- User-facing tracker names must use the last two hexadecimal address characters:
  7A, 91, 59, C8, 20, 8A (test unit), 78; XIAO E4 is excluded for now.
- User requested OTA restoration and a temporary bypass of idle suppression.
  `bench on/off/status` now controls a volatile sensor-test mode, off after reboot.
  It requires the tracker USB console and does not change TDMA scheduling.
- Receiver 0.0.24 is installed. 8A was USB-repaired to 0.0.21, then successfully
  updated by radio to 0.0.22 with MCUboot confirmation. Wrong-board refusal and
  interrupted-transfer recovery were verified. A second radio transfer to 0.0.25
  completed; USB subsequently verified 0.0.25 and MCUboot confirmation.
- 0.0.25 removes an offer cache that blocked retries after receiver session reuse.
  Source commit: `4de1a01`. Images: `dist/0.0.25/tdma/`.
- The other six ProMicros received a one-time USB repair from 0.0.19 using
  the combined MCUboot UF2. All seven now report 0.0.25 and confirmed images;
  pairing and gyro bias matched the pre-flash checks. E4 remains excluded on
  0.0.19, which cannot receive firmware OTA offers.
- The hub disappeared during this work, returned with six tracker ports and a
  Windows USB port-reset error, then tracker console reads stalled before the
  next flash. Reconnecting the upstream cable restored all seven consoles and
  allowed deployment. Re-enumerate devices by serial/address before use.
- Bench mode was enabled on all seven ProMicros and remained enabled after a
  20-second radio capture with start/end beeps. All seven delivered data without
  gaps over 250 ms during that check; this is not a range test or a fix for late
  TDMA slots. Raw per-second data: `dist/0.0.25/bench-radio.csv`. Bench mode is
  currently on; `bench off` or reboot restores normal idle suppression.

## Current radio-only experiment, 2026-10-11

- Six ProMicros **7A, 59, C8, 20, 8A, 78** now run diagnostic **0.0.27**,
  installed entirely by OTA and verified through confirmed radio telemetry.
  **Leave radio-only firmware installed**, per user request. It emits no pose
  data. Restore normal tracking only when requested, using OTA (normal 0.0.25).
- Receiver is **0.0.26**. 91 was absent during rollout and remains last-known
  normal 0.0.25; E4 was excluded and remains last-known 0.0.19. Both appeared as
  unmodified background transmitters during portions of the experiment.
- Tracker USB is authorized for **diagnostics only** in this work. On 8A, the
  initial 0.0.26 OTA attempt returned to confirmed 0.0.25; the original failure
  log was not captured. Moving sensor shutdown to the larger sensor-worker stack
  produced 0.0.27, which booted/confirmed. An actual OTA return to 0.0.25 also
  booted/confirmed before redeploying 0.0.27. No tracker was USB-flashed here.
- Completed 30 s TDMA / 30 s legacy / 30 s TDMA with 100 Hz synthetic generation,
  IMU/fusion off, and start/end beeps. All captured test telemetry was confirmed,
  workload off, target 100 Hz; generator-skip and API-error counters did not grow.
- The TDMA delivery ceiling repeated after switching back: e.g. 78 received
  43.6–50.6, 89.6–98.0, then 45.2–49.6 packets per diagnostic second in successive
  five-second windows. It recorded roughly 44–51 late slots/s in TDMA. This
  identifies a scheduler limitation independent of IMU work, not an unavoidable
  radio-loss baseline. 59 also had 882/507/332 ms receive gaps in the first stage;
  its separate delivery failures remain unexplained.
- Raw capture, counters, five-second windows and plot:
  `dist/diagnostics/synthetic-20261011-173604-KST/`. See [radio test](RADIO_TEST.md)
  for findings and confounds. Positions/power were not controlled; 8A had USB
  attached for diagnostics. E4 and briefly 91 changed background traffic.
- Recommended next software investigation: instrument actual beacon/TX times
  and missed send windows, then improve TDMA scheduling. This is a proposal,
  not permission to silently flash a new scheduler. A pure radio comparison
  requires a fixed transmitter set and unchanged placement/power.

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
