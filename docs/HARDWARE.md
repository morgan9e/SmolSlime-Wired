# Hardware and identification

[Documentation index](README.md) · Hardware observations updated: 2026-10-11 (KST).

## Identify before flashing

Use the device address printed by the tracker's `info` command. Receiver IDs are
pairing assignments; COM ports can change. Neither is a permanent board identity.
The table below is the last recorded fleet, not a fresh hardware query.

Open consoles at 115200 baud with DTR enabled. Use `list` on the receiver to check
the current ID-to-address mapping, and `info` on a tracker to check its address,
`Target`, firmware and MCUboot confirmation state.

| Recorded ID | Role | Board | Device address | USB serial | Last firmware |
| --- | --- | --- | --- | --- | --- |
| 0 | Chest | ProMicro stacked SPI | D235DBBB9B7A | 2D2949174DDE772E | 0.0.27 radio-only, confirmed |
| 1 | Left thigh | ProMicro stacked SPI | 847BEBC50791 | A4C462FDB5E01827 | 0.0.25 normal, last confirmed; absent during test rollout |
| 2 | Right thigh | ProMicro stacked SPI | DEE876FA4759 | 823B25D6352D1CF7 | 0.0.27 radio-only, confirmed |
| 3 | Waist/hip | ProMicro stacked SPI | 6DF7EC0CB8C8 | 94530FD952246331 | 0.0.27 radio-only, confirmed |
| 4 | Left ankle | ProMicro stacked SPI | D81D4F94B220 | 0D60E9D03DC04A2C | 0.0.27 radio-only, confirmed |
| 5 | Right ankle; first test target | ProMicro stacked SPI | 628DCFC6318A | 629BE058486D6B6E | 0.0.27 radio-only, confirmed |
| 6 | Spare, not worn | XIAO nRF52840 Sense | 157A3123FAE4 | E6A1176A6A36031A | Last known 0.0.19; excluded |
| 7 | Left foot | ProMicro stacked SPI | 04A6D591E478 | DC3C3581CBDDDAB2 | 0.0.27 radio-only, confirmed |
| — | Receiver | Nordic nRF52840 Dongle | BCF6929F4E52 | F9F258A5111356F3 | 0.0.26, synthetic diagnostics + normal tracking |

Use address suffixes (7A, 91, 59, C8, 20, 8A, E4, 78) in user-facing reports.
Body roles above are historical assignments; boards have been swapped in tests.
All trackers retain MCUboot. The six radio-only images remain installed by user
request and emit no pose data; see [the comparison](RADIO_TEST.md). Tracker USB
was used only to diagnose 8A during this rollout; all six updates used OTA.

The dongle's bootloader serial is `FCF6929F4E52`, different from its application
USB serial. Tracker 78 previously served as a ProMicro receiver;
its wired/hub roles are historical.

## Board targets and sensors

| Board | Build target | IMU |
| --- | --- | --- |
| ProMicro stacked SPI | `promicro_uf2/nrf52840/stackedspi` | LSM6DSV over SPI, VQF, no magnetometer |
| XIAO Sense | `xiao_ble/nrf52840/sense` | LSM6DS3TR-C family over I2C |
| Nordic dongle | `nrf52840dongle/nrf52840` | Receiver only |

The recorded ProMicro cells are 180 mAh. Charge estimation defaults to that
capacity and an assumed 100 mA current. Neither the charging estimate nor the
charger current has been validated on hardware.

Two ProMicros reportedly have missing/damaged antennas. IDs 0 and 5 were weaker in
the recorded tests and are suspects only. Confirm physically before attributing a
specific board's loss to antenna damage. USB vs battery is not an equal antenna
comparison: the cable and power state both change the test.

## USB and bootloaders

| Mode | VID:PID / identity | Entry and flashing |
| --- | --- | --- |
| Tracker application | `1209:7692` | Console `dfu`, or double-tap reset |
| Receiver application | `1209:7690` | HID + CDC console; console `dfu` |
| ProMicro UF2 | `239A:00B3`, NICENANO | Copy the correct UF2 to the mounted drive |
| XIAO UF2 | `2886:8045` | Copy the XIAO UF2 to its mounted drive |
| Dongle bootloader | Open DFU | Side RESET button or `dfu`; use Nordic DFU ZIP |

Recorded bootloaders: ProMicro nice!nano 0.11.0; XIAO UF2 0.6.1 with SoftDevice
S140 7.3.0. The recorded writable upper limit was 0xEA000 on both boards; do not
assume a different bootloader has the same limits.

Use [OTA and recovery](OTA.md) for image selection and the exact flash layouts.
Pairing and calibration occupy NVS at 0xEE000–0xF4000 on these trackers. The
repository's combined OTA UF2 leaves it intact. Reusing a tracker as a receiver
can overwrite incompatible settings in that region.

## Wiring reference

The active setup requires no inter-tracker UART wiring. For board work, the
ProMicro stack uses MISO P0.11, MOSI P0.24, SCK P0.22, CS P1.00, IMU interrupt
P0.02, clock P1.11, VCC P0.17 and ground P0.20, as recorded in the original notes.
Confirm against the board's devicetree before changing hardware.

Retired UART pin assignments, ESP32 hub power and the full wired layouts remain
in section 3 of the [original memory](archive/MEMORY-2026-10-10.md). They are not
instructions for the current wireless fleet.
