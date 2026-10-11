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
