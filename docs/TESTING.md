# Testing and outstanding validation

[Documentation index](README.md)

The next priority is a baseline of all six worn trackers on the current firmware.
The [historical measurements](archive/MEASUREMENTS.md) end before an all-six-moving
test of 0.0.3 or later. The procedures here are planned work, not completed tests.

## Record the setup

For every run, record date/time in KST, source commit/tag if known, reported firmware
per device address, receiver firmware/protocol, power source, distance, placement,
motion and duration. Note any attached USB cables and whether other trackers are on.

Use tracker `info` and receiver `list` to verify the ID/address mapping. Do not
assume that the [last recorded inventory](HARDWARE.md) is unchanged. Save raw logs
alongside a short result note; CSV files are ignored by Git, so deliberately preserve
the logs needed for future comparisons.

## Six-tracker link baseline

1. Verify that all six run the intended rate/retry firmware and have confirmed
   MCUboot images. Do not change firmware and placement in the same comparison.
2. Power the worn trackers from battery. Connect only the dongle to the PC.
3. Open the receiver console at 115200 baud with DTR, send `diag on`, and capture
   its output. Run the HID monitor alongside SlimeVR Server:

   ```powershell
   python tools/slime_link.py --duration 60 --gap 50 --csv link-moving.csv
   ```

4. Start with all six moving at about 1 m. Keep them moving through the measured
   interval; note pauses rather than counting ordinary stillness as failure.
5. Record per tracker: received/s, sequence loss, repeats/late frames, longest gap,
   gaps of at least 50 ms, average/worst valid RSSI. Record radio CRC and noise data.
6. Repeat under the same conditions for any later firmware comparison. Send
   `diag off` when finished.

`slime_link.py` sees only the receiver's output. Use `diag` for sequence loss and
radio errors; HID report rate is not radio frame rate. High loss under load in the
old tests implicated collisions, but a single counter does not prove the cause of
a new failure.

## Real worn poses and placement

Run a 20-minute normal VRChat session, noting times for at least two minutes each
of back turned at about 3 m, crouching and lying down. Keep receiver diagnostics
running, and use:

```powershell
python tools/slime_link.py --duration 1200 --gap 250 --csv link-session.csv
```

If pose-dependent failures remain, repeat comparable motion with the dongle raised,
then on a USB extension near the headset or hip. This tests position before any
new host software is built. See [receiver placement](RECEIVER_PLACEMENT.md).

The inherited acceptance targets are under 5% sequence loss per moving tracker,
no unexplained silence over 250 ms during active motion, and worst valid RSSI
above -80 dBm in each pose, followed by a two-hour endurance run. These are project
targets, not achieved results or guaranteed hardware limits. Log collision-related
gaps too: they still affect tracking and should not disappear from the report.

## Other open hardware checks

| Check | What would count as useful evidence |
| --- | --- |
| Resting battery tracker during OTA | Transfer completes without sleep/power-off interruption; boot, confirmation and tracking verified |
| Corrupted OTA transfer | Controlled corrupted transfer refused; old image still runs |
| Protocol mismatch | A deliberately incompatible protocol is rejected with both versions reported |
| Power loss during swap | Controlled test on the spare tracker; recovery and settings verified |
| Charge estimate | Starting on battery, plug in and compare reported percentage over time with configured assumptions |
| XIAO vs ProMicro reception | Same firmware behavior, power source, location and movement |
| Suspected missing antennas | Physical identity confirmed before comparing RSSI or planning repairs |

Use the identified spare/test tracker for destructive fault-injection work and
prepare its USB recovery image first. See [OTA](OTA.md) for which cases were
already tested and the absence of a watchdog/version rollback restriction.

## Software checks

`make test` runs the link-monitor unit tests. On Windows:

```powershell
python -m unittest discover -s tools -p test_slime_link.py
```

These tests do not validate firmware timing, RF behavior, USB bootloaders, charging,
or a successful OTA boot. Documentation-only edits need link/source consistency
checks; firmware builds and flashing are not part of a documentation cleanup.
