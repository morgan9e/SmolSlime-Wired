# Experimental TDMA v1

This is an opt-in implementation for the nRF52840 ProMicro, XIAO Sense and Nordic
dongle. It schedules tracking transmissions instead of letting every tracker
transmit immediately. The original 0.0.18 prototype passed a basic eight-tracker
USB/radio smoke test, but showed frequent late slot skips; range improvement is
not established. The wireless mode-control extension below has only software
validation so far. Default builds keep the existing ESB behavior.

## Wireless mode control (development, not flashed)

Build with `make tdma` and install the new tracker images and receiver together
over USB. The receiver console accepts:

```text
radio mode legacy
radio status
radio mode tdma
radio cancel
```

Only the receiver needs a USB console for subsequent mode changes. `radio status`
reports the active/target modes, preparation or checking phase, masks and missing
tracker IDs. A queued request is not success: wait for `phase=idle result=success`.
The receiver snapshots **all paired IDs**, including offline trackers. It waits up
to five seconds for matching preparation replies from every ID, then changes
mode and requires every ID to report the new mode within another five seconds.
Missing preparation replies leave the old mode active. Missing arrival replies
roll the receiver back and advertise the old mode again. `radio cancel` also
returns to the old mode. There is no force/visible-only option. Pairing-table
changes cancel an in-progress transaction. Replies are checked against token,
phase, modes and required mask; duplicate replies cannot satisfy another ID.

The receiver is authoritative: it repeats a 12-byte control message in tracking
ACK payloads and appends it to the 12-byte timing beacon. In legacy mode it sends
one recovery beacon per second, allowing a restarted tracker (which boots in
TDMA listening mode) to discover legacy mode without USB. A tracker already
sending legacy frames learns a rebooted receiver's TDMA mode from its ACKs.
Beacons and replies use the existing paired radio addresses and ESB CRC; this
does not add authentication beyond the existing link. Recovery requires actual
RF reception; severe interference can still prevent reconnecting.

Mode selection is RAM-only. Receiver reboot resets the fleet toward TDMA;
tracker reboot reacquires the receiver's current mode. Pairing/calibration stay
unchanged. This changes no RF channel and does not restore firmware OTA: USB
firmware updates remain required in both runtime modes of this prototype.
Older firmware does not understand the extended beacons: update the whole fleet
before testing. Ordinary builds without `CONFIG_RADIO_TDMA` remain unchanged.

Legacy mode means unslotted ESB with the existing device-specific retry delay and
default retry count. The control build retains the active HF clock and a worker
holding the newest pending frame until ESB is idle; it is not a byte-for-byte
reproduction of the old firmware's flush-on-every-new-frame or low-power behavior.
Control replies replace at most one tracking opportunity per 50 ms during a
transaction. Recovery beacons briefly interrupt legacy reception once per second;
larger ACKs/beacons also consume airtime. These costs and transition outages need
hardware measurement. Mode changes are coordinated and recoverable, not an
instantaneous atomic switch of eight radios.

`make test` exercises the shared control state machine in both directions,
timeouts, missing IDs, duplicates, stale/malformed replies, cancellation and token
wrap. Hardware acceptance must cover both mode changes, a powered-off tracker,
packet loss during transition, cancellation, receiver/tracker reboot recovery,
pairing-table changes, and actual delivery/slot timing under motion and USB load.
The frequent timing skips in the original TDMA prototype are not fixed by this
control feature.

## Schedule

The receiver broadcasts one unacknowledged beacon on pipe 2, then switches back
to receive tracking packets on pipe 1. Both use the existing paired receiver
address and 2 Mbps channel. A listening tracker enables **only pipe 2**, so it
cannot acknowledge another tracker's tracking packet. Discovery still uses pipe 0.

Each valid beacon grants one opportunity per tracker:

| Interval after beacon reception | Use |
| --- | --- |
| 0–1 ms | Receiver switches back to RX; guard |
| 1–2 ms | ID 0 |
| 2–3 ms | ID 1 |
| … | … |
| 8–9 ms | ID 7 |
| Remaining time to next beacon | Guard and receiver prepares its next beacon |

A tracker prepares its radio 350 microseconds before its transmit target, which
is 200 microseconds into its slot. It can submit within the following 150
microseconds. If reconfiguration or scheduling takes too long, it skips the slot.
The decision is checked again after reconfiguration, with interrupts locked
through submission. Transmissions request an ACK but have **zero ESB retries**;
the budget for one transaction is 600 microseconds. New sensor data replaces the
pending frame, and at most one frame is submitted per beacon.

The receiver spaces beacon submissions by at least 10 ms, giving up to 100
tracking transmissions per second per device. Delayed wakeups shift its schedule
forward; they never cause a burst of catch-up beacons. This does not change the
sensor FIFO's sampling rate. A tracker with no pending data leaves its slot empty.

The 12-byte timing prefix is
`[CD 54][version=1][slots=8][frame_us:u16][slot_us:u16][serial:u32]`,
with little-endian integers. Control builds append the 12-byte control message
described above. Version, size and geometry must match. Duplicate or
out-of-order serials do not grant another slot, including at counter wrap. After
20 ms without an accepted beacon, a reset serial can reacquire a rebooted receiver.
There is no free-running holdover or unscheduled legacy fallback: losing a beacon
loses that frame's opportunity. IDs outside 0–7 stay silent; new pairing allocations
above ID 7 are rejected. Existing pairing addresses and IDs are reused.

## Build

From WSL/Linux at the repository root:

```bash
make test               # C timing/protocol tests plus existing Python tool tests
make tdma               # both OTA-layout tracker boards and the Nordic receiver
# Or independently:
make tdma-tracker
make tdma-tracker-xiao
make tdma-receiver
```

These targets enable `CONFIG_RADIO_TDMA=y` through each app's `tdma.conf`.
Outputs are isolated under `dist/tdma/`; build directories are under
`$(BUILD)/tdma/`. Ordinary firmware images in `dist/` are not overwritten.
Host tests require a C11 compiler (`CC`, default `cc`) and Python 3.

Tracker images retain their board-specific MCUboot layouts and ACK-based image
confirmation. **Radio OTA transfers are disabled in this prototype**, both at the
receiver's `ota begin` command and the tracker transport. Use the board's combined
`_OTA.uf2` for USB installation/recovery. Supporting OTA needs an explicit transfer
reservation protocol; sending update requests between slots would defeat TDMA.

The generated images are development builds, not a tagged release. Before any
hardware rollout, follow the version/build workflow in [Development](DEVELOPMENT.md),
including a fresh version tag and rebuild. Building does not flash devices.

## Observability

The receiver console's `tdma` command reports beacon, error and late-wakeup
counters, followed by per-ID receive counts and last/min/max phase within that
ID's slot. `outside` counts callbacks outside its nominal 0–999 microsecond slot
interval. These are software callback timestamps after ACK handling, **not**
hardware RX-END captures. Treat them as a way to spot timing problems, not a
precise airtime measurement. All these counters are cumulative since boot.

The tracker connection status includes `tdma:` counters:

- `beacons`: accepted synchronization beacons.
- `submitted`, `acked`, `failed`: actual radio submissions and their ESB results.
- `late`: skipped transmit windows; `empty`: slots with no pending data.
- `replaced`: pending frames superseded before a slot; `errors`: API/timeout errors.
- `locked`, `listening`, or `unsupported-id`: current synchronization state.

Existing receiver `diag on` logging remains available. In TDMA builds, byte 20's
sequence number advances only on successful radio submission. Deliberate
coalescing and skipped slots therefore **do not appear as sequence loss**. A lost
ACK can also increment tracker `failed` even when the receiver got the data.
Compare delivered rate, time-series gaps, scheduler counters and sequence loss
together; a lower sequence-loss percentage alone does not prove improvement.

## Hardware acceptance still needed

1. Use one identified test tracker and the receiver, installing the receiver last.
   Use USB recovery if they cannot synchronize; a TDMA tracker is silent with a
   legacy receiver. Verify tracking ACKs, MCUboot confirmation, and USB console
   access before adding the other trackers.
2. Observe `tdma` and `diag on` under sensor and USB load. Check beacon cadence,
   slot phases, skipped slots, errors and actual delivered frame rates. For timing
   acceptance, capture RADIO events/GPIO with a logic analyzer; verify the beacon
   and all eight transactions stay separated under worst observed IRQ latency.
3. Remove/restart the receiver and restore it. Verify silence without beacons,
   reacquisition after restart, and no catch-up burst. Exercise pairing/reset too.
4. Run the full fleet with the same pose, receiver position and motion as the
   legacy baseline. Legacy trackers still transmit asynchronously, so mixed
   firmware cannot demonstrate collision isolation. Keep one-second traces and
   outage durations rather than reducing the result to one average.
5. Repeat on battery at the intended distances and body positions. Record current
   draw: this version keeps HFXO on and listens between slots, increasing power use.

Synchronization currently timestamps the ESB callback using the 32.768 kHz system
clock, and radio work runs in a preemptible priority-4 thread. The guards model
bounded interrupt latency; software cannot guarantee that bound under every load.
The host simulation exercises a 100 microsecond per-endpoint skew assumption,
missed beacons, delayed wakeups, invalid IDs and serial wrap. It does not establish
hardware latency, RF delivery or battery life. Hardware capture, sleeping between
beacons, reserved retries, radio OTA and allocations beyond eight IDs remain
follow-up work.

This can remove collisions between cooperating trackers. It cannot repair antenna
matching, body attenuation or external interference, and no range improvement is
claimed before the controlled comparison.

The implementation uses [shared timing rules](../shared/radio_tdma.h), the
[tracker ESB worker](../Tracker/src/connection/esb.c), and the
[receiver ESB worker](../Receiver/src/connection/esb.c). The old `timer.c` routines
and receiver `tdma.h` allocator remain dormant and are not started by this mode.

## Software checks, 2026-10-11

- `make test`: C protocol/deadline checks and a 1,000-frame eight-tracker
  simulation passed; all seven existing Python link-tool tests passed.
- TDMA ProMicro MCUboot, XIAO MCUboot and Nordic receiver builds passed using the
  configured SDKs. Generated configs have `RADIO_TDMA=y` and `STACK_USAGE=y`.
- TDMA-disabled ProMicro MCUboot and receiver builds also passed in isolated
  `dist/tdma/legacy-check` output directories.
- Reviewed compiler stack reports: the radio thread's own frame is 104 bytes on
  the ProMicro and 80 bytes on the receiver, before callees/interrupts. Both use
  1,024-byte thread stacks. This is not a measured runtime stack high-water mark.
- Existing SDK/deprecation and dormant timer warnings remain. The final compile
  reports no warning from the added TDMA code. `git diff --check` passed.

Build logs are local ignored files: `dist/tdma-final-build.log`,
`dist/tdma-review-build.log`, and `dist/tdma-legacy-build.log`. No devices were
flashed and no radio or battery tests were run for this implementation.
