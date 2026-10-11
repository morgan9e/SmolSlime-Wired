# Experimental TDMA v1

## Hardware timing implementation (pending hardware validation)

`CONFIG_RADIO_TDMA_HW` now defaults on in TDMA builds. It reserves TIMER1
(1 MHz, 32-bit) and three dynamically allocated PPI channels plus one PPI group.
Receiver diagnostics already reserve TIMER3/TIMER4; ESB retains its protocol
timer (normally TIMER2). The dormant old `timer_init()` must not be started.

CRCOK captures received packet time; END captures beacon transmission completion.
The scheduler prepares ESB up to 700 us before a slot, then TIMER1 compare
triggers ESB's existing EGU/PPI radio-start sequence without thread wakeup.
A PPI fork disables the scheduling group on the same event, making it one-shot.
ESB continues to own payload encoding, ACK turnaround, retries and radio IRQs.

The build generates an ESB source copy with a single guarded `radio_start()`
hook; it does not edit the installed SDK. CMake refuses an unexpected hook or
library layout. The hook delays only an explicitly requested start. Ordinary
RX, legacy, pairing and OTA use the original SDK path. Hardware starts are
cancelled before reconfiguration, mode changes and OTA takeover. An already
late preparation is rejected rather than immediately transmitted.

Wire format and slot allocation are unchanged, allowing test-tracker-first
OTA rollout against the previous receiver. There is still no missed-beacon
holdover. `tdma hw` diagnostics show scheduled/fired/cancelled/rejected starts
and minimum preparation lead. Hardware validation and measured outcomes are
recorded below when complete.

This is an opt-in implementation for the nRF52840 ProMicro, XIAO Sense and Nordic
dongle. It schedules tracking transmissions instead of letting every tracker
transmit immediately. The original 0.0.18 prototype passed a basic eight-tracker
USB/radio smoke test, but showed frequent late slot skips; range improvement is
not established. The wireless mode-control extension has the limited 0.0.19/0.0.20
hardware checks recorded below. Default builds keep the existing ESB behavior.

## Wireless mode control (experimental)

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
The receiver snapshots paired IDs heard in the last five seconds. Offline IDs
are excluded, and a request with no active trackers is refused. It waits up
to five seconds for matching preparation replies from every selected ID, then changes
mode and requires every selected ID to report the new mode within another five seconds.
Missing preparation replies leave the old mode active. Missing arrival replies
roll the receiver back and advertise the old mode again. `radio cancel` also
returns to the old mode. The required snapshot stays fixed during the transaction: a selected tracker
that disappears still causes timeout/rollback; late joiners follow the advertised
mode without becoming required. The console shows paired, active and offline masks.
Pairing-table changes cancel an in-progress transaction. Replies are checked against token,
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
unchanged. This changes no RF channel. Firmware OTA now has a radio takeover path
as described below; installed 0.0.18/0.0.19 TDMA trackers require one USB update to
gain that support, even when switched to legacy mode.
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

This receiver-side policy update is compatible with the 0.0.19 tracker firmware.
The eight-slot geometry is unchanged; an absent tracker leaves its slot empty.
Presence is recent reception, not proof that a tracker is powered off. A tracker
blocked by interference for five seconds can be excluded and must recover through
the normal beacons/ACKs when reception returns.

### 0.0.20 receiver check — 2026-10-11 (KST)

Receiver commit `2547976`, tagged `0.0.20`, was built and flashed; trackers remain
on 0.0.19. Both TDMA → legacy and legacy → TDMA completed with `result=success`,
`ready=ff`, `arrived=ff`, and `missing=00`. The receiver was left in TDMA and all
eight pairings were retained. XIAO was absent from USB but still active over radio,
so these were eight-tracker transitions. Offline selection/expiry and a fixed
seven-tracker snapshot passed software tests; a seven-tracker hardware transition
and late-join recovery remain untested. Receipts: `dist/0.0.20/`.

ID 4's previously pending USB check also completed: 0.0.19, MCUboot confirmed.

### 0.0.19 installation check — 2026-10-11 (KST)

Tag `0.0.19` identifies implementation commit `65f8bcb`. All eight trackers were
USB-flashed using the combined MCUboot images, and the Nordic receiver was restored
from the RSSI scanner to the matching receiver build. IDs 0, 1, 2, 3, 5, 6 and 7
reported 0.0.19 and confirmed MCUboot images over USB; their pairing and gyro bias
matched the pre-flash readings. ID 4 did not return on USB, but subsequently
reported 0.0.19 and delivered tracking frames over radio. Its post-flash MCUboot
confirmation has not been read. The receiver retained all eight paired addresses.

A 20-second stationary/unspecified-motion observation received IDs 1, 2, 3, 4, 6
and 7. IDs 0 and 5 were absent. A request for legacy mode collected preparation
mask `de`, then timed out with `missing=21` (IDs 0 and 5) and stayed in TDMA,
verifying refusal when paired trackers are absent. This does **not** verify a
successful mode transition, rollback after a partial transition, or range.
Local receipts and capture: `dist/0.0.19/`; release images: `dist/0.0.19/tdma/`.

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
confirmation. Use the board's combined `_OTA.uf2` for first installation/recovery.
The restored OTA path accepts addressed offers from normal tracking ACKs in either
runtime mode. The target pauses tracking and its radio worker installs unslotted
PTX with retries; only then may the OTA thread send requests on pipe 7. When the
receiver gets the first valid request, it holds PRX and stops TDMA beacons until
completion, stop, or transfer timeout. Other TDMA trackers therefore pause while
beacons are absent; legacy trackers can continue transmitting. Transfer chunk ACKs
take priority over mode-control ACKs. Concurrent mode changes and OTA begins are
refused. Refusal/abort returns the target to normal radio operation; successful
installation reboots into MCUboot's test swap and reacquires the receiver's mode.
No wire-format or board/layout checks have been removed.

This restoration requires updated receiver **and** tracker firmware. A receiver
update cannot make old TDMA trackers accept offers. Validate an actual transfer,
confirmed boot, refusal, and timeout recovery on the spare before fleet rollout.

For stationary load testing, tracker console `bench on` bypasses motion-change
suppression and keeps the sensor in its active-rate mode. `bench off` restores
normal behavior; reboot also turns it off. It is RAM-only, appears in `info`, and
does not change transmit scheduling, retries, calibration, or pairing. This command
is local to the tracker console, not a wireless control command.

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
beacons, reserved retries and allocations beyond eight IDs remain
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

## OTA restoration hardware checks, 2026-10-11

Receiver 0.0.24 and tracker 0.0.21 or later restore firmware OTA in TDMA
builds. Installed 0.0.18/0.0.19 trackers need one USB repair using the combined
`_OTA.uf2`; changing the receiver alone cannot restore their offer handler.
Tracker 0.0.25 also removes a persistent offer cache that incorrectly suppressed
retries when the receiver rebooted and reused its session number.

On test tracker **8A** (`628DCFC6318A`), a real radio transfer from 0.0.21 to
0.0.22 completed in 27 seconds. Its USB console then verified the new version,
MCUboot confirmation and resumed TDMA lock. A wrong-board offer was refused.
A partial transfer stopped after 3,052 requested bytes timed out on the tracker;
the confirmed 0.0.22 image remained installed and TDMA resumed. A second full
radio transfer to 0.0.25 completed in 28 seconds, and its version appeared in
receiver diagnostics; its USB console subsequently confirmed the image. The six
remaining ProMicros (7A, 91, 59, C8, 20, 78) were USB-repaired to 0.0.25 and
verified with confirmed images, original pairing and gyro bias. XIAO E4 remains
excluded on 0.0.19. These checks do not establish range or battery behavior.

The receiver's one-entry ACK FIFO required special handling: `esb_flush_tx()`
returns busy during PRX, so flushing in the receive callback did not remove the
old offer. The receiver worker now reinitializes PRX once at transfer takeover,
then leaves it running to preserve queued firmware chunks.

Local receipts: `dist/0.0.22/ota-receiver24-retry.log`,
`dist/0.0.22/8A-after-ota.txt`, `dist/0.0.22/abort-check.txt`, and
`dist/0.0.25/8A-ota.txt`. Protocol, deadline, eight-tracker simulation and all
seven Python link-tool tests passed after the retry fix.

After deployment, `bench on` was enabled on all seven ProMicros and verified
again after a 20-second capture. Each delivered data throughout; no observed
gap exceeded 250 ms. Per-second rates are preserved in
`dist/0.0.25/bench-radio.csv`, with console receipts in
`dist/0.0.25/bench-enable.txt` and each `*-bench-after.txt`. This bypasses sensor
idle suppression but leaves the known scheduler lateness unchanged. E4 still
transmitted its old firmware during this check and was not modified.
