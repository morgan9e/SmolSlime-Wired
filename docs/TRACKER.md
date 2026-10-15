# Tracker notes

Tracker firmware changes beyond BLE ([BLE.md](BLE.md)) and OTA ([OTA.md](OTA.md)), and the state of
the boards they were tested on.

## Robustness

- **Hardware watchdog** (`src/system/watchdog.c`): nRF WDT, 8 s, fed by a thread at the lowest
  preemptible priority, so any thread that hogs the CPU resets the tracker. Before the reset it
  stores the running thread's name, priority and the uptime in RAM that survives the reset; `info`
  then shows `Watchdog: on, this boot came from a watchdog reset: <thread> ...`. The UF2 bootloader
  keeps feeding it, so `dfu` and flashing still work.
- **Frozen trackers fixed** (b58f43e): a second USB "connected" event created the console and HID
  threads again on top of the running ones, which corrupted the scheduler. The tracker kept its BLE
  link alive with empty packets but sent nothing. Each thread is now created and aborted once
  (atomic guards). The receiver also drops a tracker that sends nothing for 5 s ([BLE.md](BLE.md)).
- **IMU recovery** (d91ad91): if the IMU had been sending and its FIFO stays empty (10 loops), the
  sensor loop keeps the fusion state and initializes the IMU again, once until it works again.
  Before, the tracker set the sensor error status and stayed connected, frozen. A FIFO that holds
  packets that cannot be processed still reboots, as upstream.
- Console `scan` is a full rescan: it forgets the stored IMU and magnetometer addresses first.

## Settings

- `sensor_send_unchanged` (0/1, default 0): at 1 a still tracker sends every update, not only when
  its orientation changes, so a radio load test runs with trackers on a desk. Raise
  `sensor_lp_timeout` too (e.g. 2147483647) so they stay out of low power; `reset_config` both
  afterwards. Appended after `connection_over_ble`; the other ids are unchanged.

## ABRJ Chrysalis (ProMicro nRF52840, LSM6DSV + QMC6309)

`make tracker-chrysalis`; `flash_usb.py` recognizes it. The QMC6309 magnetometer is not wired to the
nRF: it sits on the LSM6DSV's auxiliary I2C bus and is read through the IMU's sensor hub. Upstream
(stock) firmware never found it; three fixes make it work:

1. **Sensor hub reads and writes** (2641b79): the one-shot routines waited while the done bits
   were set, starting from 0, so they never waited. They now wait for XLDA and SENS_HUB_ENDOP /
   WR_ONCE_DONE and fail on SLAVE0_NACK; the accelerometer is switched on for the scan (the hub runs
   on its samples).
2. **No DRDY through the hub** (23c8aa7): DRDY showed on only 2-3 of the 10 samples per second.
   Behind the hub the driver reads the data registers directly; the loop skips repeats.
3. **Magnetometer polling** (64890dc): each hub read waits for an accelerometer sample (4-8 ms at
   120 Hz), and the loop polled on every pass until a sample came: the FIFO overflowed and the
   tracker sent 46 packets/s instead of ~120. Polls are now 10 ms apart. Also fixed for every
   magnetometer: a read with no new sample let uninitialized stack values into calibration and
   fusion.

Result on CF025C07AA21: 122 packets/s at full rate with the magnetometer, no FIFO warnings.

## Boards (2026-10-16)

| Board | State |
|---|---|
| AB6C (7A93, stacked I2C) | IMU interrupt line does not work: polls, ~64 packets/s. Rework planned: CS→P1.00, SDO→P0.11, INT1→P0.02. |
| FAE4 (XIAO Sense) | Weakest signal in play; ran down to 5% battery during tests. |
| CF025C07AA21 (Chrysalis) | Magnetometer works, but the board resets: the IMU alone at random (1-46 min, every register back to its reset value), and twice the whole board (USB gone 1-30 s, no watchdog). Same with the magnetometer off. Suspect power or a joint; try another cable and port, else return it. Runs with `sensor_use_mag 0`. |
| 8A46270F33C2 (Chrysalis) | Magnetometer never found, also after power cycles: every sensor-hub transfer times out. Hardware. Otherwise fine: 1.5 h without a reset or stall. |
