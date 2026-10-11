# Synthetic ESB test

Current deployment is diagnostic tracker and receiver **0.0.29**, with hardware
TDMA timing. **0.0.27** with receiver **0.0.26** is the earlier polling-scheduler
test configuration used for the first comparison below. Do not deploy
the initial 0.0.26 tracker image: the first hardware attempt did not remain on
that image and 8A returned to confirmed 0.0.25. A likely stack problem was fixed
by moving sensor-shutdown I/O from the 256-byte startup thread to the 1 KB sensor
worker. The original failing boot log was not captured, so the precise original
fault is not proven. 0.0.27 subsequently booted and confirmed over OTA, and an
OTA return to normal 0.0.25 booted and confirmed too. USB was used only for serial
diagnostics on 8A during these subsequent checks.

`make radio-test-tracker` builds a ProMicro MCUboot diagnostic image under
`dist/radio-test/`. It generates one ordinary-sized 21-byte ESB packet per 10 ms
deadline in both runtime modes. Late generator deadlines are counted and skipped,
never sent as a catch-up burst. Tracker-specific initial phase avoids deliberately
synchronizing legacy producers. TDMA keeps the existing slot worker and retry
policy, so this isolates sensor/fusion workload, not every source of radio timing.

The normal connection producer is stopped. Sensors are discovered and explicitly
shut down (an MCU reset does not necessarily reset the IMU), and the fusion loop
is never started. No calibration or pairing is cleared. A separate optional
`CONFIG_RADIO_TEST_IMU_WORKLOAD=y` build keeps the active sensor/fusion workload
while still replacing its output with identical synthetic traffic. The initial
deployment uses workload off and `CONFIG_RADIO_TEST_HZ=100`.

The image retains OTA, USB recovery, and ACK-based MCUboot confirmation. Use
`_OTA_app.bin` through `tools/ota.py`; do not use a plain non-MCUboot image. To
restore tracking, OTA-install the known-good normal TDMA image (0.0.25). This
diagnostic image intentionally emits no pose packets.

The updated receiver recognizes reserved type `0xe1`, validates the ordinary
CRC/sequence, updates radio presence for mode switching, and retains telemetry.
It never forwards these test packets to the SlimeVR HID stream. `diag on` retains
per-second receive/loss/gap/RSSI diagnostics; `tdma` additionally prints `rftest`
counter snapshots, MCUboot confirmation, workload flag, target rate, received
telemetry-kind mask and snapshot age. Unmodified receivers ACK/drop this reserved
type but cannot report its telemetry or keep it in mode-switch presence.

The packet layout is in `shared/radio_test.h`. Five alternating telemetry kinds
carry uptime/pseudorandom data and cumulative generated, generator-skipped,
TDMA-skipped, submitted, replaced, acknowledged, failed and API-error counters.
No extra telemetry packets are generated. Some counters are 24-bit modulo values;
kind snapshots arrive at different times. `kinds=1f` means all five kinds have
arrived since boot. ACK/failure counters include radio mode-control transactions;
exclude transition and OTA intervals from comparisons. A submission is an accepted
ESB API call, not a hardware TX-START timestamp. PRX ACKs are not proof of host HID
delivery; the receiver's independent diagnostics provide the receive evidence.

Keep positions and power unchanged for 30 s TDMA, 30 s legacy, then 30 s TDMA.
Capture serial diagnostics and counter snapshots, verify `confirmed=1 imu=0
hz=100 kinds=1f`, and compare time windows instead of just whole-run averages.
Single-transmitter tests require other transmitters off; a diagnostic image does
not silently disable unrelated trackers. XIAO remains excluded from this rollout.

`python tools/radio_test.py` captures three 30-second stages with beeps, raw serial
chunks, per-interval receive diagnostics and transmitter counter snapshots. It
verifies the currently active devices before starting. If an unmodified tracker
cannot be turned off, `--background E4` explicitly permits and records that
background traffic; such a run is not an isolated all-synthetic radio test.

## Hardware comparison, 2026-10-11 17:36 KST

Six ProMicros (7A, 59, C8, 20, 8A, 78) received 0.0.27 by OTA and confirmed.
91 was absent at its rollout turn, so no image was offered. Receiver 0.0.26
captured 30 seconds each of TDMA, legacy, then TDMA. Start/end beeps played.
The receiver was left in TDMA, diagnostics off. **Radio-only firmware remains
installed at the user's request**; these six trackers intentionally do not
produce normal body-tracking data.

Local evidence: `dist/diagnostics/synthetic-20261011-173604-KST/`, containing
`raw.jsonl`, `intervals.csv`, `counters.csv`, `setup.json`,
`five-second-windows.csv`, and `comparison.png`. Deployment receipts are in
`dist/0.0.27/`. All 540 captured test-counter snapshots reported confirmed,
IMU workload off, 100 Hz target. No generator deadline skips or ESB API errors
were counted during any stage.

The following are **ranges across six consecutive five-second windows**, not
whole-run averages. Rates are received packets per approximately one-second
receiver diagnostic interval; see the CSV and graph for their order.

| Tracker | TDMA first | Legacy | TDMA repeat |
| --- | ---: | ---: | ---: |
| 7A | 53.8–63.4 | 84.0–95.6 | 56.8–58.4 |
| 59 | 24.2–52.6 | 77.6–90.2 | 40.8–53.0 |
| C8 | 71.8–76.0 | 95.6–97.0 | 70.4–74.4 |
| 20 | 69.0–75.0 | 91.2–93.8 | 69.0–73.8 |
| 8A | 45.8–67.2 | 81.8–89.0 | 41.0–50.2 |
| 78 | 43.6–50.6 | 89.6–98.0 | 45.2–49.6 |

78 repeatedly counted roughly 44–51 late TDMA slots/s while submitting only
44–54 packets/s. C8 counted roughly 19–25 late slots/s while submitting around
71–79 packets/s. Legacy submissions were near the 100 Hz target across all
six. Thus a substantial TDMA deficit happens **before submission to ESB** and
survives removal of the sensor/fusion workload. Current code polls from a worker
with `k_usleep(100)` and accepts transmission starts within a 150 us window;
actual wakeup, reconfiguration and interrupt latency need measurement. Hardware
timer scheduling and beacon/TX timestamps are the next candidates to investigate.
This comparison does not establish which individual latency source dominates.

There are additional delivery failures. During the first TDMA stage 59 dropped
from five-second receive windows of 52.6 to 46.2, 33.0, 24.2, then recovered to
40.6 and 46.8. Its longest receive gaps were 882 ms around capture second 10.7,
507 ms around 19.8, and 332 ms around 23.8. Legacy's largest observed gap for 59
was 91 ms; the repeated TDMA stage reached 131 ms. This is directly relevant to
visible freezes/catch-up of the affected body segment, but this radio-only test
does not measure rendered pose jitter or end-to-end latency.

Limits: 8A was USB-connected for diagnostics, and physical placement/power were
not controlled. Its RSSI changed markedly during the first stage. Although the
preflight active mask was `bd` and background mask was zero, normal 91 appeared
briefly around seconds 6.7–11.7 of TDMA first, and E4 appeared around second 24.9
and stayed active throughout legacy and TDMA repeat. Both remain unmodified.
`setup.json` is only an initial snapshot; raw/interval records establish these
later participants. This is not a fixed-fleet interference or range test.

Counter-rate estimates use host receipt times and rotating telemetry snapshots;
individual counter kinds can be older than the newest received packet. Packet
loss can distort short counter-delta windows (notably 59), so these estimates are
not precise hardware TX timing. ACK failures are not identical to receiver loss:
the receiver can receive a packet whose ACK does not reach the sender. Legacy
also uses its existing retry policy, whereas TDMA uses no tracking retries.

## Hardware-timed comparison, 2026-10-11 18:20 KST

The user authorized implementing hardware-timed TDMA and fixing 91. USB
diagnostics identified 91 on normal 0.0.25 with bench off and messages about
sensors entering low power after 500 ms without movement. These messages do
**not** prove that the MCU powered off or explain every earlier radio absence.
It accepted 0.0.27 over OTA, then joined the full 0.0.29 hardware-timer rollout.
Its pairing and calibration readouts were retained. It participated throughout
all three stages below. Radio-only mode bypasses the sensor-idle path, but
connection-timeout, user-button and battery-related shutdown paths still exist.
Do not describe an absent radio as definitely powered off.

All seven ProMicros (7A, 91, 59, C8, 20, 8A, 78) and the Nordic receiver now run
0.0.29. Tracker image: 225560 bytes, CRC `b3f46ce1`. Every tracker confirmed its
boot and refused a repeat offer because the running image matched, independently
verifying the deployed image. 8A's first update was temporarily silent; its later
USB diagnostic read showed confirmed 0.0.29 with 6588 scheduled/fired starts and
zero late skips. No failing boot log or definite cause of that gap was captured.
Subsequent OTA round trips 0.0.29 → 0.0.27 → 0.0.29 worked with both the previous
receiver and the new hardware-timed receiver. USB was used only for tracker
diagnostics; the receiver itself was flashed by USB DFU. Radio-only firmware
remains installed and the receiver was left in TDMA, diagnostics off.

Evidence: `dist/diagnostics/synthetic-20261011-182006-KST/`, plus deployment and
round-trip receipts under `dist/0.0.29/`. The timed run was 30 s TDMA, 30 s legacy,
30 s TDMA, with beeps. E4 remained unmodified background traffic throughout.

| Tracker | TDMA first | Legacy | TDMA repeat |
| --- | ---: | ---: | ---: |
| 7A | 85.2–90.6 | 94.8–98.6 | 78.6–93.2 |
| 91 | 67.2–85.6 | 89.6–93.2 | 47.8–76.8 |
| 59 | 87.4–95.0 | 100.2–100.8 | 87.8–94.6 |
| C8 | 89.0–96.6 | 94.2–98.6 | 85.0–97.0 |
| 20 | 89.4–95.6 | 94.0–98.0 | 87.0–95.6 |
| 8A | 88.6–92.8 | 85.6–97.0 | 78.6–95.0 |
| 78 | 81.2–91.6 | 99.0–100.4 | 88.4–91.4 |

These are ranges of five-second-window receive counts per diagnostic interval,
not a whole-run score. The receiver interval is slightly longer than 1000 ms,
so a perfectly delivered 100 Hz stream can show just above 100 in this table.
The CSV preserves window order and the plot shows individual intervals.

All seven trackers recorded **zero increments in late-slot, generator-skipped
and API-error counters** throughout the three stages. Typical received packet
phases were 464–465 us after nominal slot start (the 200 us guard plus radio
ramp-up and packet airtime). Five out-of-slot records appeared between the last
legacy snapshot and the first repeat-TDMA snapshot: one on 59, two on C8 and
two on 8A. This interval spans the mode change and its first few seconds; the
capture cannot locate the packets more precisely or prove their cause. No
subsequent out-of-slot increments occurred during the capture. This establishes
a large timing improvement over the previous worker-only scheduler, not
lossless RF or entirely anomaly-free transitions.

Receiver preparation remains imperfect: across the first/last counter snapshots
of the TDMA stages (about 29 seconds each), rejected beacon starts increased by
28 and 62. It safely skipped those late preparations. Missed beacons and empty
pending buffers can also lower submissions without increasing a tracker's
late-slot counter. Further instrumentation should separate those causes.

91's delivery issue is **not fully fixed**: its TDMA-repeat five-second receive
windows were 58.5, 47.8, 76.8, 53.2, 56.4 and 69.8, with a maximum receive gap
of 120 ms. Its received RSSI was approximately -70 to -71 dBm, versus -69 to
-70 during legacy. Its slot timing remained correct. The data does not establish
damaged hardware; it leaves beacon reception, interference and the lack of TDMA
retries as relevant follow-up questions. Legacy has retries and performed better
for 91 in this run. 59's longest gap was 50/30/40 ms across the three stages,
but its placement/signal had also improved since the earlier test, so do not
attribute its entire recovery to this code change.

Rotating telemetry occasionally makes five-second submission-rate estimates
physically implausible (an old kind-2 snapshot is replaced by a newer one).
The raw counters and estimates are retained, but the current plot omits that
panel instead of presenting it as precise TX timing. Positions, power and E4
traffic are still confounds; this was not a worn-motion or 5–7 m range test.
