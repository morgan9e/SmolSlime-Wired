# Synthetic ESB test

Use diagnostic tracker **0.0.27** with receiver **0.0.26** or later. Do not deploy
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
