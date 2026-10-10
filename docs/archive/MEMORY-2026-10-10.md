# Project memory

Everything learned, built, measured and decided in this project from 2026-10-08 to 2026-10-10, for anyone
(person or Claude session) picking it up later. How-tos live in `README.md` and `docs/OTA.md`; the
earlier wired design is documented on branch `wired` (`README.md`, `src/Tracker/WIRED.md`,
`src/Receiver/HUB.md`, `src/Hub/README.md`, `docs/On-Body_Hub_Implementation_Plan_v3.md`).

Times are KST (UTC+9), the user's clock; firmware `Build:` stamps are KST too. Session transcripts
(Claude Code, `C:\Users\Morgan\.claude\projects\C--Users-Morgan-Lab-SlimeVR-Hub\`) are listed at the end.

Contents:

1. Timeline
2. Goal and the decisions that shaped it
3. Hardware
4. Radio, packets and protocols
5. Measurements
6. Repository: branches, history, tags
7. `main`: what differs from upstream
8. OTA (summary; details in docs/OTA.md)
9. PC tools
10. Toolchain and builds
11. The wired era (branch `wired`) in detail
12. Analyses: BLE, 1 Mbps, TDMA, power
13. Quest-hosted receiver plan, charging and power
14. Rules and preferences from the user
15. Pitfalls and lessons
16. Open items
17. Sources

---

## 1. Timeline

| When (KST) | What happened |
|---|---|
| Oct 7 | Separate session "Quest Slime Proxy" (cwd `Downloads`) started from a "Quest-Hosted SlimeNRF Receiver" spec (see 13). Its code in `Downloads\QuestSlimeProxy` is gone from disk. |
| Oct 8 | `docs/On-Body_Hub_Implementation_Plan_v3.md` (wired branch): ankle wired to thigh, thigh relays by ESB, receiver on hip, UART to an ESP32 hub, Wi-Fi to SlimeVR Server. |
| Oct 9 02:04 | First session here: receiver logging (`packet_log`), tracker `link` stats, DEBUG/TRACE log modes, receiver `hid`/`mirror` switches, `slime_view.py` viewer. Hub firmware minimized after the user's "HUB STAYS MINIMAL". |
| Oct 9 03:20-04:40 | "Two devices on id 1" (sequence mismatches) solved: receiver pairing generation (new radio address on `clear`), tracker background re-pair, relay resend fix. `DEBUG_NO_POWER_SAVING`, radio stall watchdog, per-tracker retransmit delay. |
| Oct 9 04:00-05:00 | All six trackers fade together, RSSI -63..-82 dBm at 30 cm: antennas suspected. User wears all six. Receiver on a USB extension "stutters even more". |
| Oct 9 05:30-06:19 | Full-wired chain mode built (ankle→thigh→hub by UART, S3 hub reads 3 chains), then "commit and revert": parked on `full-wired` branches. XIAO nRF52840 receiver build made for a test (never run). |
| Oct 9 06:20 | "Can you modify the Tracker and Receiver code to use BLE instead of ESB?" Plan only. |
| Oct 9 17:17-21:30 | Receiver `diag on` (CRC counters, noise floor, per-tracker loss), single-tracker tests: decode floor ~-81 dBm at 2 Mbps, 1 Mbps no better. BLE shelved: same radios, same floor. `diag hub on` + hub pass-through to UDP 6971 + `slime_diag.py` (approved exception to the hub rule). |
| Oct 9 23:16 | Side Q&A: Quest 3S charging, USB hub power, a DIY charge dock for the trackers (see 13). |
| Oct 9 23:30-Oct 10 01:20 | Relay bug found: radio clock stopped mid-frame on battery (`clocks_release_when_idle`, relayed-ankle loss 64% → 1%). |
| Oct 10 01:34-04:30 | Hip relay with explicit `mode` command, then hub mode (hip replaces the receiver, outputs to ESP32 on D0). Boot loops fixed (stack overflows, power.c UART suspend, sysworkq). Hub mode worked end to end 04:56-05:07: tag `hub-mode-ok`, `dist/wired/hub-mode/SETUP.md`. |
| Oct 10 05:17-06:28 | Switched to a simpler relay (waist `mode wired in both` → receiver). Rearm race fixed (not flashed). Batteries died. User: "it's all wired now... which i dont think is what was intended", "Of course i want to go full wireless". BLE plan given. |
| Oct 10 06:57-07:24 | Three nested git repos merged into one with `git filter-repo` (old .git dirs in `C:\Users\Morgan\Lab\SlimeVR-Hub-git-backup\`). |
| Oct 10 10:23 | Official nRF52840 Dongle arrived; with stock firmware on all trackers it "works just very well": the old ProMicro receiver's antenna was the main problem. `slime_link.py` (host stats over HID). |
| Oct 10 10:30-10:46 | Stock receiver + diagnostics patch for the dongle; `dfu_zip.py` to flash it with nrfutil. Measurements: still 7-10% loss, all six moving 53% loss = collisions. |
| Oct 10 11:23-12:01 | 100 Hz cap + per-tracker retry delay; clean branch with `Receiver/` and `Tracker/` subtrees; versions from tags; flash watchers with sounds. 0.1.0→0.0.1 (25% loss moving), 0.2.0→0.0.2 "no flush" (40%, reverted), 0.0.3. |
| Oct 10 12:01-12:16 | XIAO nRF52840 Sense flashed as tracker id 6 (~15 dB stronger, 2.4% loss, USB powered). |
| Oct 10 12:18-12:55 | ESB rate and firmware size → OTA question; plan `docs/OTA.md`; decisions "1 yes 2 no 3 pause". |
| Oct 10 13:01-14:27 | OTA built: MCUboot (swap and rollback tested on tracker 5), radio transfer 647 s upload + 118 s → streaming 26 s. |
| Oct 10 11:28-15:18 | Side session: BLE vs ESB power, 1 Mbps analysis, `slime_view` auto source (USB or Wi-Fi, never both), charge-time battery estimate (branch `charge-estimate`), Quest spec v2. |
| Oct 10 ~14:30-16:00 | `flash_usb.py`; all six ProMicros on the MCUboot build over a USB hub; 0.0.13 to all six over the radio; XIAO OTA layout (0.0.14 over the radio); review; OTA protocol 1 (0.0.15) to all seven; failure tests (0.0.16 on tracker 5). |
| Oct 10 ~16:00 | Branches: `clean` + `charge-estimate` merged → `main`; old `main` → `wired`; one folder `SlimeVR-Hub`. This file. |

## 2. Goal and the decisions that shaped it

- **Goal: full-body tracking that is fully wireless and close to stock firmware.**
  - Stock SlimeVR Server on the PC, an nRF52840 Dongle as the receiver, six ProMicro trackers.
  - Firmware updates over the radio.
  - In the user's words: "Of course i want to go full wireless unlike current almost full wired".
- **Batteryless:** only if the setup ever goes full wired ("if i am going to go full wired anyway, i do want to go batteryless too"). Parked.
- **Cables were a workaround** for weak links. The wired chains, hub mode and relays live on branch `wired`.
- **The receiver was the problem.** The dongle hears the trackers 15-25 dB stronger than the old ProMicro receiver.
- **The remaining loss is collisions** between trackers. Fixes, in order:
  - Cap the send rate at 100 Hz (about 120 Hz in practice).
  - Give each tracker its own retry delay.
  - Keep "newest frame wins" (flush): retrying old frames lost more.
  - Later options: 80 Hz, TDMA, or a second dongle on another channel.
- **Stay on ESB at 2 Mbps.**
  - BLE: same radios and antennas, more power, a rewrite.
  - 1 Mbps: double the airtime and more collisions; sensitivity is not the limit now.
- **`main` = stock code plus only what is used.**
  - Upstream histories are merged in as git subtrees, not repositories nested inside the repository.
  - Versions are `0.0.N` tags.
- **OTA:**
  - MCUboot with rollback.
  - Unsigned images: MCUboot still checks the SHA-256, the tracker a CRC32.
  - The tracker pauses tracking during an update.
  - Stream PC → dongle → tracker; don't store the image on the dongle first.
  - SMP (MCUmgr) was considered and recommended against (not built): no ESB transport, slower over 32-byte payloads.
- **Quest-hosted receiver:** only if a measurement step says it is needed (spec v2, section 13).

## 3. Hardware

### 3.1 Boards and their ids

Identify boards by device address (`info`), never by COM port (Windows assigns COM per board, but ids and roles changed often).

Current ids (receiver = the dongle):

| Id | Role | Board | Device address | USB serial | COM | Firmware now |
|---|---|---|---|---|---|---|
| 0 | chest | ProMicro stacked SPI | D235DBBB9B7A | 2D2949174DDE772E | COM7 | 0.0.15 (MCUboot) |
| 1 | left thigh | ProMicro stacked SPI | 847BEBC50791 | A4C462FDB5E01827 | COM18 | 0.0.15 |
| 2 | right thigh | ProMicro stacked SPI | DEE876FA4759 | 823B25D6352D1CF7 | COM16 | 0.0.15 |
| 3 | waist / hip | ProMicro stacked SPI | 6DF7EC0CB8C8 | 94530FD952246331 | COM20 | 0.0.15 |
| 4 | left ankle | ProMicro stacked SPI | D81D4F94B220 | 0D60E9D03DC04A2C | COM8 | 0.0.15 |
| 5 | right ankle (test tracker) | ProMicro stacked SPI | 628DCFC6318A | 629BE058486D6B6E | COM11 | 0.0.16 |
| 6 | none (not worn) | Seeed XIAO nRF52840 Sense | 157A3123FAE4 | E6A1176A6A36031A | COM32 | 0.0.15 |
| - | receiver | Nordic nRF52840 Dongle | BCF6929F4E52 | F9F258A5111356F3 | COM29 | this repo's receiver, OTA protocol 1 |

- **Dongle bootloader** (Open DFU): serial FCF6929F4E52, COM28.
- **Old ProMicro receiver** (stacked board with an LSM6DSV; on branch `wired` it was also the hub-mode hip):
  - Device address 04A6D591E478, USB serial DC3C3581CBDDDAB2, COM24 (COM14 when running tracker firmware).
  - Its radio address was DE00B8BD9999 until its settings were wiped. It is now 04A6D591E478 (pairing generation 0).
- **The trackers wear an id history.**
  - Old receiver, final list: 0 DEE876FA4759, 1 628DCFC6318A, 2 847BEBC50791, 3 6DF7EC0CB8C8, 4 D81D4F94B220, 5 D235DBBB9B7A.
  - In hub mode: hip 0, right thigh 1, right ankle 2, left thigh 3, left ankle 4, chest 5.
- **Antennas.**
  - Two ProMicros have no antenna ("2 broken one without any antenna").
  - Ids 0 and 5 were consistently the weakest with the dongle (-59..-66 dBm vs -43..-52), so probably those two. Never confirmed with the user.
- **The tracker build** (photo, Oct 9):
  - Blue 3D-printed case with strap slots.
  - Cell "TW 302530 3.7V 180mAh 0.666Wh".
  - ProMicro clone with USB-C, LSM6DSV breakout stacked on it.
  - A red wire antenna lying along the board edge (user's mod; "it didnt affected reception").
  - Advice given: about 31 mm straight from the solder point, at least 1 cm from metal, chip antenna removed. Never tested.
- **IMUs.**
  - ProMicros: LSM6DSV over SPI (address 0x008F), VQF fusion, no magnetometer.
  - XIAO Sense: LSM6DS3TR-C on I2C ("LSM6DS3TR-C/LSM6DSL/LSM6DSM/ISM330DLC" group).
- **ESP32 hub** (only on `wired`):
  - Seeed XIAO ESP32-C6: ESP32-C6FH4 rev v0.2, MAC 98:88:e0:7f:ad:14, IP 192.168.0.56 (gateway 192.168.0.1).
  - COM23 on USB (303A:1001).
  - Flash it app-only at 0x10000 to keep its Wi-Fi settings.
- **Router:** Wi-Fi channel 11, far from ESB channel 2 (2402 MHz). Ruled out as a cause.
- **Plan v3 hardware list (Oct 8):**
  - 2 padless ProMicros (ankles), 4 stacked ProMicros (thigh, hip, chest), 1 stacked ProMicro receiver.
  - 1 ESP32-S3; spares 2 Chrysalis boards; the Nordic dongle was then on order.
- **USB hub:** the user bought one (Oct 10), so all trackers can be on USB at once.

### 3.2 USB ids and bootloaders

| Device | USB id | Notes |
|---|---|---|
| Tracker firmware | 1209:7692 | ProMicro and XIAO |
| Receiver firmware | 1209:7690 | HID (64-byte IN and OUT reports) + CDC console; dongle HID name "SlimeNRF Receiver nRF52840 Dongle", manufacturer "Nordic Semiconductor" |
| ProMicro UF2 bootloader | 239A:00B3 | "UF2 Bootloader 0.11.0, Model: nice!nano, Board-ID: nRF52840-nicenano, Date: May 21 2026, SoftDevice: not found"; drive label NICENANO; its USB serial = chip id |
| XIAO UF2 bootloader | 2886:8045 | "UF2 Bootloader 0.6.1 lib/nrfx (v2.0.0) lib/tinyusb (0.10.1-293-gaf8e5a90)", Board-ID Seeed_XIAO_nRF52840_Sense, SoftDevice S140 7.3.0 |
| Dongle bootloader | (nordicDfu) | "Open DFU Bootloader"; flash with nrfutil and a DFU zip |
| ESP32-C6 | 303A:1001 | esptool |

- **Entering a bootloader:**
  - Trackers and receivers: console `dfu` (writes GPREGRET 0x57), or double-tap reset.
  - Dongle: `dfu`, or press its side RESET button (red LED pulses).
- **Expected noise:** after `dfu`, a serial read error ("ClearCommError ... device does not recognize the command") is normal.
- **UF2 write limits** are documented nowhere. Read them from the range of `CURRENT.UF2` on the drive. Both UF2 bootloaders write only up to 0xEA000:
  - ProMicro: 0x1000-0xEA000.
  - XIAO: reads 0x1000-0xEA000; writes from 0x27000, the SoftDevice stays.
- **Other PC devices seen:** COM1 (ACPI), COM5/COM6 (Bluetooth serial), Logitech receiver 046D:C548, ASUS 0B05:19AF.

### 3.3 Flash layouts

| Board / build | Layout |
|---|---|
| ProMicro, normal tracker or receiver | MBR 0x0-0x1000, app 0x1000-0xEE000 (948 KB), settings (NVS) 0xEE000-0xF4000, UF2 bootloader 0xF4000-0x100000 |
| ProMicro, MCUboot (OTA) | MBR, MCUboot 0x1000-0xD000 (48 KB), slot 0 0xD000-0x7C000 (444 KB), slot 1 0x7C000-0xEA000 (440 KB), unused 0xEA000-0xEE000, NVS, UF2 bootloader |
| XIAO, normal tracker | SoftDevice area 0x0-0x27000, app 0x27000-0xEE000 (796 KB), NVS 0xEE000-0xF4000, UF2 bootloader 0xF4000 |
| XIAO, MCUboot (OTA) | SoftDevice 0x0-0x27000 (kept; the bootloader starts the app at 0x27000), MCUboot 0x27000-0x33000, slot 0 0x33000-0x8F000 (368 KB), slot 1 0x8F000-0xEA000 (364 KB), NVS, UF2 bootloader |
| Dongle receiver | nrf5_mbr 0x0-0x1000, app 0x1000-0xDA000, NVS 0xDA000-0xE0000, Nordic bootloader 0xE0000-0x100000 |

- **Pairing and calibration live in NVS.**
  - Every step of this project kept them: switching to the MCUboot build, OTA updates.
  - A tracker board flashed as a receiver loses them: same NVS area, conflicting slots. The receiver then needs `clear`.
- **Retained RAM** on the ProMicro tracker: 4 KB at 0x2003F000 (struct `retained_data`, 1024 bytes). SRAM shrinks to 252 KB.
- **Wiping a tracker's settings properly:** `reset_data all`, twice; the second confirms. A UF2 that writes 0xFF over NVS does not work: the bootloader ignores it.

### 3.4 Pins (ProMicro / nice!nano)

| Pin | nRF | Use |
|---|---|---|
| D0 | P0.08 | full-speed pin; receiver UART mirror TX to ESP32 D7; hub-mode output on `wired` |
| D8 | P1.04 | wired left header data (UART1) |
| D9 | P1.06 | wired left header ground reference |
| D16 | P0.10 | wired right header data (UART0); an NFC pin |
| D10 | P0.09 | wired right header ground; an NFC pin |
| D14 | P1.11 | the stack's clock pin |
| D15 | P1.13 | free; early chain-mode upstream TX |
| P0.06 | | receiver pairing button (sw0, pull-up) |

- **Stacked SPI board:**
  - SPI: MISO P0.11, MOSI P0.24, SCK P0.22, CS P1.00.
  - IMU int0 P0.02, clk P1.11, vcc P0.17, gnd P0.20.
  - Base board: LED P0.15, button gpio1 0.
- **D0-D7 and D14** are taken by the SmolSlime stack.
- **Low-frequency I/O:** all header pins are "low frequency I/O only". Nordic advises against UART on them while the radio runs.
- **NFC pins as GPIO:** D10/D16 need `nfct-pins-as-gpios` in the UICR. The first boot writes it and restarts once.
- **UARTs:** the nRF52840 has two, each with one baud rate for TX and RX.
- **XIAO nRF52840:** a mirror TX was planned on D6 = P1.11. Its stock uart0 is TX P1.11 / RX P1.12.
- **XIAO ESP32 pins:**
  - D7 = GPIO17 (C6) / GPIO44 (S3); D8 = GPIO19 / GPIO7; D9 = GPIO20 / GPIO8.
  - C6 RF switch: GPIO3 low powers it; GPIO14 low selects the internal antenna, high U.FL.
  - LED: GPIO15 (C6) / GPIO21 (S3).

## 4. Radio, packets and protocols

### 4.1 ESB as configured

- **Bit rate and channel:** 2 Mbps, channel 2 (2402 MHz, the NCS default; nobody sets it), no hopping.
- **ESB settings:**
  - CRC16, dynamic payload length, maximum payload 32 bytes, 8 pipes, TX and RX FIFOs of 8, `ESB_SYS_TIMER2`.
  - Trackers are PTX with `selective_auto_ack` true; the receiver is PRX.
  - Stock retransmit: delay 600 µs, count 3.
  - TX power: trackers `radio_tx_power` 8 dBm (Kconfig default); the receiver asks for 30, capped at +8 dBm.
- **Tracker TX timeout:** `(retransmit_delay + 400) * (retransmit_count + 1) + 500` µs; 650 µs airtime at 1 Mbps.
  - Connection error after 30 consecutive TX errors (TX_ERROR_THRESHOLD; MAX 100, CLEAR_RATE 10).
  - After that, pairing requests about every 3 s.
- **NCS driver behaviour:**
  - `esb_write_payload` only starts TX when idle (-EBUSY otherwise).
  - A frame that exhausts its retries raises TX_FAILED but stays at the head of the FIFO until success or flush. This caused the relay resend bug.
  - The PRX sends a queued ACK payload with the *next* frame on that pipe; the frame after drops it.
  - `esb_flush_tx` only works when idle. `esb_is_idle`, `esb_start_tx`, `esb_pop_tx`, `esb_tx_full` exist in this SDK.
- **Airtime per frame** (21-byte payload, calculated):

  | | 2 Mbps | 1 Mbps |
  |---|---|---|
  | Frame | ~125 µs | ~240 µs |
  | ACK | ~40 µs | ~70 µs |
  | Whole transaction, with turnaround | ~0.42-0.43 ms | ~0.57 ms |

  - That is about 2,400 frames/s for the whole channel, even if perfectly scheduled.
  - Need: 6 × 120 = 720/s (about 30%). Stock offered about 1,500/s, more than half the maximum, so it collapsed.
  - Random access breaks down well below capacity: 20-23% busy already gave 25% loss.
- **Sensitivity:** about -89 dBm at 2 Mbps and -93 dBm at 1 Mbps (datasheet). The old ProMicro receiver decoded nothing below about -81 / -85.

### 4.2 Frames and packets

- **ESB frame:** 21 bytes = packet[16] + CRC32 little-endian (`crc32_k_4_2_update(0x93a409eb, data, 16)`) + sequence byte.
  - The receiver handles length 21 (sequence check), 20 (CRC) or 16.
  - It drops CRC failures, and out-of-order sequences within 100 ms ("Sequence missmatch ... Discarding").
  - It ignores ids it does not store; an id needs `DETECTION_THRESHOLD` 5 packets before it counts.
  - Ids above 223 are reserved for the receiver: 224-254 its own packets, 255 the address announcement.
- **Pairing:**
  - The tracker sends on the discovery address (base {0x62,0x39,0x8A,0xF2}, prefixes {0xFE,0xFF,0x29,0x27,0x09,0x02,0xB2,0xD6}).
  - Request: `[crc8_ccitt(0x07, addr, 6), 0 becomes 8][ack][device address 6]`.
  - Reply, as an ACK payload: `[checksum][id][receiver address 6]`.
  - Data goes on pipe 1. A tracker's id = its position in the receiver's stored list.
  - The tracker keeps its id until it re-pairs. So a receiver `clear`/`remove` can make two trackers share an id: that was the "two devices on id 1" problem.
- **Packet types** (byte 0 type, byte 1 id; RSSI in byte 15 except types 1 and 4):

| Type | Content |
|---|---|
| 0 | device info: [2] battery (bit 7 set → % in low 7 bits, 255 = charged, 128 = none), [3] voltage (`(v+245)*10` mV), [4] temperature (`(t-128.5)/2+25` °C, 0 = none), [5] board, [6] MCU, [8] IMU, [9] magnetometer, [10-11] firmware date `((year-2020)&127)<<9 \| month<<5 \| day`, [12-14] firmware major/minor/patch |
| 1 | Q15 quaternion x,y,z,w + Q7 acceleration (no RSSI) |
| 2 | exp-map quaternion (u32 at [5], 10/11/11 bits) + acceleration + battery/voltage/temperature |
| 3 | status: [2] server status, [3] tracker status |
| 4 | Q15 quaternion + magnetometer Q10 gauss (no RSSI) |
| 5 | runtime estimate, int64 µs |
| 6 | button + sleep-in ms |
| 7 | button + sleep + exp-map quaternion + acceleration |
| 253 / 254 | receiver / per-tracker link diagnostics (`wired` branch fork only, see 11) |
| 255 | receiver → host: id ↔ radio address ([1] id, [2-7] address, little-endian on HID) |

- **Status values:**
  - Server status: DISCONNECTED 0, OK 1, BUSY 2, ERROR 3, OCCLUDED 4, TIMED_OUT 5.
  - Tracker status bits: SENSOR_ERROR 1, CONNECTION_ERROR 2, SYSTEM_ERROR 4, USB_CONNECTED 8, PLUGGED 16, CALIBRATION_RUNNING 32, BUTTON_PRESSED 64.
- **Tracker send schedule** (stock `connection_thread`):
  - Data packet when there is new data.
  - Magnetometer packet every 200 ms when needed; info every 100 ms; packet 6 every 100 ms.
  - Status and runtime every 1000 ms.
- **Sensor loop:**
  - Moving: 6 ms stock (`LOW_NOISE`). Still: 33 ms (`LOW_POWER`, about 20-30 frames/s). Idle: 100 ms (`LOW_POWER_2`).
  - The IMU FIFO threshold rounds the loop period to whole gyro samples. Gyro 240 Hz on the LSM6DSV (200 asked, rounded up), 208 Hz on the XIAO.

    | Loop setting | FIFO samples | Actual period | Rate |
    |---|---|---|---|
    | 6 ms | 1 | 4.17 ms | 240 Hz |
    | 10 ms | 2 | 8.33 ms | 120 Hz |
    | 13 ms | 3 | 12.5 ms | 80 Hz |

  - Stock trackers send about 220-260 frames/s when moving.
- **Still trackers** send every 100-200 ms, and sleep (IMU wake-on-motion) after 5-15 s still: `USE_IMU_TIMEOUT`, ramp `IMU_TIMEOUT_RAMP_MIN` 5000 / `MAX` 15000 ms. `ACTIVE_TIMEOUT_DELAY` is 900000 ms.
- **Battery while plugged in** (stock):
  - `plugged` is set above 4300 mV and cleared at 4250 mV, or comes from VBUSDETECT.
  - While plugged, the reported voltage is forced to at least 4310 mV so the server shows charging.
  - The ProMicro has no charger status pin, so "charged" (255) is never sent.
  - The voltage-based % reads high while charging; the charge estimate on `main` replaces it (see 7.2).

### 4.3 Receiver → PC (USB HID)

- **Reports:** 64 bytes, up to four 16-byte packets each. HID OUT is 64 bytes (stock only logs it; on `main` it carries OTA data).
- **Sharing:** hidapi opens the receiver shared on Windows. Every program that opens it gets every report, so `slime_link.py`/`ota.py` run alongside SlimeVR Server.
- **Address byte order:** HID sends the address in packet 255 little-endian. Tools reverse it to match the receiver's `list` and the hub's MAC.
- **What the PC cannot see from a stock receiver:** it drops sequence numbers before HID, so the PC can't count lost frames exactly. It can't see CRC errors or noise either. Use the receiver's `diag on`.

### 4.4 SlimeVR Server (PC)

- **Version and ports:** v21.1.0, ports 6969 and 21110 (a second instance fails: "A required port (6969 and 21110) is busy").
- **Config:** `C:\Users\Morgan\AppData\Roaming\dev.slimevr.SlimeVR\vrconfig.yml`; trackers keyed `"<ADDR>/0"`; `hidConfig: trackersOverHID: false`.
- **Logs:** `...\dev.slimevr.SlimeVR\logs\`.
- **Launch:** `C:\Program Files (x86)\SlimeVR Server\jre\bin\java.exe -Xmx128M -jar slimevr.jar run`.
- **UDP protocol** (as the ESP32 hub uses it; protocol version 22, big-endian, header = u32 type + u64 packet number):
  - Hub → server: 0 heartbeat, 3 handshake (board CUSTOM 4, MAC = radio address, most significant byte first), 12 battery (float volts, float 0..1), 15 sensor info, 17 rotation, 19 signal strength, 20 temperature, 23 rotation + acceleration.
  - Server → hub: 1 heartbeat, 10 ping, 15 sensor-info ack.
  - Handshake reply `\x03Hey OVR =D 5`. One UDP socket per tracker id, local port 6980 + id.
  - New MACs need a one-time accept in the SlimeVR GUI.
- **`connection_over_hid`:** this stock tracker setting sends data over the tracker's own USB (needs `trackersOverHID` on). It is the "USB cable to limbs" option; never tested.

## 5. Measurements

### 5.1 Old ProMicro receiver (Oct 9, `wired` era firmware)

**Oct 9 morning** (receiver packet_log, RSSI):
- At 30 cm on a desk: good seconds -63..-71 dBm, bad seconds -74..-82. All six trackers fade together, by 8-10 dB at once.
- About 25 dB short of the -30..-50 expected at 30 cm.
- The thigh's ACK rate was 64-95% under load.

**Single tracker** (receiver `diag`):

| Test | Loss | RSSI | Notes |
|---|---|---|---|
| Still, 22 fps | 0% | -70 | |
| Full rate | ~23% | -68..-82 | Bursts of 50-200 ms |
| 10 cm, swinging | ~5% | -57..-66 | |
| 1 m line of sight | ~50% | | Never decoded below -81/-83 dBm |
| 1 m at 1 Mbps | 65% | | Weakest decoded -85/-86: no gain |

- The noise floor was clean, -92..-93 dBm, so 30-45 dB of signal was missing.

**On-body session** (382 s, all six; `diag.csv`):
- Loss per tracker 31-61%; bad CRC 39.6%.
- Loss followed signal strength:

  | RSSI | Loss |
  |---|---|
  | -57..-60 | 5-11% |
  | -65..-68 | 13-36% |
  | -73..-76 | 36-64% |
  | -77..-80 | 75-85% |

- Correlation of loss with bad CRC: 0.73.

**Relay clock bug:**
- A thigh relaying its ankle on battery lost 64% of the ankle's frames. On USB it lost 0.6-1.5%, because USB keeps the crystal on.
- Fixed by `clocks_release_when_idle`: 1.0% on battery. The fix did not cure the general weak-signal loss.

**Two trackers** (left thigh + ankle):

| Condition | Thigh lost | Ankle lost |
|---|---|---|
| Alone, -55 dBm | 0.3% | 1.3% |
| -73 dBm, battery | 41.6% | 55.3% |
| -71 dBm, USB | 25.5% | 38.0% |

**Hub mode** (6 trackers, hip as receiver, chest by radio at -50 dBm):
- 0 drops, 0 bad frames, about 33 fps each while still.
- Chest repeats 0.3% (68 of 20,423).

**Relay mode** (waist `in both` → receiver): 0.24% radio failures (3 of 1,267 in 10 s).

### 5.2 Nordic dongle (Oct 10)

**Stock trackers + stock receiver** (slime_link, still):
- 18-49 frames/s each.
- RSSI: ids 1-4 -43..-46 dBm, ids 0 and 5 -59..-64.
- Gaps up to 657 ms.

**All six still, about 60 cm** (diag, 21 s):
- Bad CRC 4.2%, noise -90 dBm, air busy about 4%.
- Loss per tracker: 0.7%, 10.4%, 8.1%, 8.3%, 8.1%, 6.8%.

**All six moving, 1-3 m** (31 s):

| Id | Received/s | Lost | RSSI |
|---|---|---|---|
| 0 | 99 | 60.1% | -66 |
| 1 | 108 | 56.2% | -64 |
| 2 | 129 | 48.4% | -62 |
| 3 | 147 | 41.5% | -59 |
| 4 | 130 | 47.9% | -60 |
| 5 | 87 | 64.8% | -66 |

- Overall: sent 1,491/s, received 699/s, lost 53%.
- Air busy 44%, bad CRC 33%, correlation of loss with bad CRC 0.92.
- Longest gap 405 ms. Each tracker froze for 50-150 ms about once a second.

**One tracker moving at 30 cm** (others still):
- The moving one: 218/s received of 235, 7.5% lost.
- Still id 2: 47.5% lost. Its frames kept colliding with id 1's, because both retried after the same 600 µs.
- So a single tracker is fine; the 40-65% loss is collisions, not range.

**User's slime_link, tracker 2 moving at 1 m:** 219/s, -57 dBm, no gaps over 250 ms.

**Rate cap and retry delay, all six moving at 2-3 m:**

| Firmware | Air busy | Bad CRC | Sent/s | Received/s | Lost | Repeats/s | Silences ≥ 50 ms per tracker/s | Longest |
|---|---|---|---|---|---|---|---|---|
| stock | 44% | 33.2% | 1,491 | 699 | 53.1% | 41 | 1.35 | 405 ms |
| 0.0.1 (cap + delay) | 20% | 17.6% | 794 | 594 | 25.2% | 20 | 0.50 | 183 ms |
| 0.0.2 (no flush) | 23% | 20.9% | 809 | 486 | 39.9% | 61 | 1.51 | 300 ms |

- 0.0.1 per tracker: 30.6%, 22.4%, 21.1%, 18.5%, 21.8%, 37.0% (ids 0-5).
- 0.0.2 per tracker: 49.5%, 41.2%, 35.5%, 21.4%, 31.3%, 60.9%.
- 0.0.2's repeats tripled and full-queue flushes dropped frames, so it was reverted in 0.0.3.
- A single 0.0.1 tracker moving: about 127/s, 6-8% loss.
- Never measured: all six moving on 0.0.3 or later.

**XIAO** (USB powered, moving, ProMicros then on 0.0.2):
- 94/s, 2.4% loss, -44 dBm: about 15 dB stronger than the ProMicros.
- Not a fair test: USB cable, positions and firmware differ.

**Old receiver vs dongle:**
- Old ProMicro receiver: about -70 dBm at 30 cm, about 45% loss for a single tracker at -73.
- Dongle: -44..-60 dBm at 0.3-1 m, single tracker about 7% (ProMicro) / 2.4% (XIAO).
- So the receiver was the main problem.

**Bad RSSI readings:** "worst" values of -127 are missing readings, not real.

### 5.3 OTA

| Run | Time | Notes |
|---|---|---|
| Upload the image to the dongle over its console (v1) | 647 s | One acknowledged line per 48 bytes |
| Radio transfer with look-ahead | 118 s | ~13 ms per 28-byte chunk |
| Final streaming | 21-27 s for ~248 KB | ~9 KB/s, two radio frames per chunk |

- MCUboot swap on the ProMicro: slot copy about 10 s, back on USB in about 27 s.
- Confirmation at 200 acknowledged frames: seconds while moving, up to a minute at rest.

### 5.4 Sizes

| Image | Flash | RAM |
|---|---|---|
| ProMicro tracker, stock-based | 235 KB of 948 KB | 42 KB of 252 KB |
| Same, OTA app | ~243-249 KB | |
| MCUboot | ~24 KB of 48 KB | 16.5 KB |
| XIAO tracker | 219 KB of 796 KB | |
| Dongle receiver, stock + diag | 112 KB of 868 KB | 64 KB of 256 KB |
| Dongle receiver, + OTA | ~116 KB | ~82 KB (16 KB OTA window) |
| `wired` tracker | ~280 KB | ~55 KB |

## 6. Repository: branches, history, tags

- **Location:** `C:\Users\Morgan\Lab\SlimeVR-Hub`, one folder, branch `main` checked out.
- **Remote** `origin` = https://github.com/morgan9e/SmolSlime-Wired. `origin/main` still holds the old (wired) history; nothing of the new `main` is pushed.
- **Git identity:** "Morgan J. <me@morgan.kr>". The first hub commits were made as claude@devpg.net before an identity was set.
- **Branches:**
  - `main`: stock-based. `Receiver/` subtree of SlimeVR-Tracker-nRF-Receiver `main` (e3d66d5dbf70db47fe0e854fb0b00599bf2c20b3, Apr 4 2026, "Merge branch 'dev'"). `Tracker/` subtree of SlimeVR-Tracker-nRF `main` (cbfff16ebed1659d4c0e9f361ded0030fb66855a, Sep 1 2026, "Merge pull request #159 from l0ud/main").
    - Started Oct 10 as orphan branch `clean` in worktree `SlimeVR-Hub-clean`.
    - `charge-estimate` (879780c4) merged in (5a698042); renamed to `main` and moved into the main folder the same day.
  - `wired`: the earlier work (was `main`).
    - `src/Tracker` and `src/Receiver` forks with their full upstream history, `src/Hub` (ESP32 hub), tools, plan v3.
    - Shares no history with `main`. Tip 33bd8934.
  - `hub/full-wired`: only `src/Hub`. Commit 315045ea adds an S3 hub reading wired chains; never merged.
  - `charge-estimate`: merged; can be deleted.
- **How `wired` was assembled** (Oct 10 06:57-07:24):
  - The project had three nested repositories (Tracker fork, Receiver fork, Hub) and a top folder whose own history had been lost.
  - The top-level `.git` had been re-initialized at 06:57; the old top commits 4377849 and c3c2c17 are gone.
  - Each repository was mirrored, its refs pruned, and rewritten with `git filter-repo --to-subdirectory-filter src/X`. Then they were merged into one `main` (1,763 commits) with `merge -s ours` + `read-tree --prefix`.
  - Old git dirs are in `C:\Users\Morgan\Lab\SlimeVR-Hub-git-backup\{Tracker,Receiver,Hub}.git`. They can be deleted once the merged history is checked.
  - The old Tracker stash "staged uart_tracking (v2 ankle UART, P0.29)" = `docs/backup-tracker-staged-uart_tracking-2026-10-08.patch` on `wired`.
- **Hash mapping:** commit hashes in old transcripts are pre-merge. Mapping examples:
  - Tracker c06b114 → 65e46228; de61fd2 → 62c22d81.
  - Hub 99e3007 → a9eb18ea; fffbae5 → 315045ea; ea5d2ce → a2a91020.
- **Tags:**
  - `0.0.1`-`0.0.16`: tracker versions on `main`.

    | Tag | Meaning |
    |---|---|
    | 0.0.1 | was 0.1.0: 100 Hz + retry delay |
    | 0.0.2 | was 0.2.0: no flush, reverted |
    | 0.0.3 | flush again, clock fix kept |
    | 0.0.4 | MCUboot |
    | 0.0.5-0.0.7 | layout tries |
    | 0.0.8 | rollback test image only (never confirms) |
    | 0.0.9 | OTA over the radio |
    | 0.0.10 | test image, same code as 0.0.9 |
    | 0.0.11 | streaming |
    | 0.0.12 | progress output |
    | 0.0.13 | status line |
    | 0.0.14 | XIAO OTA |
    | 0.0.15 | OTA protocol 1 |
    | 0.0.16 | receiver keeps the last outcome |

  - `tracker/0.0.1` → 5c605ab8: upstream's `tdma` branch (time-slotted radio, Eiren Rain, Oct 4), for reference.
  - `hub-mode-ok`: named by the `wired` README and `SETUP.md`, but **the ref is missing**. Its tag object 9f41db6 still exists and points to 62c22d81 on `wired`. Restore with `git update-ref refs/tags/hub-mode-ok 9f41db6` (or retag 62c22d81).
- **Line endings:** upstream files are CRLF and are kept that way. `.gitattributes` has `*.patch -text`, because CRLF would break `git am`.

## 7. `main`: what differs from upstream

### 7.1 Receiver (`Receiver/`)

- **Link diagnostics** (`src/packet_log.c/.h`, plus hooks in `esb.c`, `console.c`, `hid.c`; Kconfig `PACKET_LOG` y, `PACKET_LOG_INTERVAL_MS` 1000, `PACKET_LOG_ORIENTATION` off, `LOG_BUFFER_SIZE` 4096):
  - `diag [on|off]`, off at boot. Every second, per tracker:
    - frames received and lost (from sequence numbers, before any filter);
    - repeats (missed ACK), late;
    - longest silence, RSSI average and worst;
    - a 20 × 50 ms timeline: `#` frames, `x` frames after a loss, `.` nothing.
  - Radio good/bad CRC: RADIO events counted into TIMER3/TIMER4 through PPI.
  - Noise floor: RSSI sampled at 1 kHz while listening (median, quietest/loudest 10%, % above -80 dBm). A clean channel is about -95 dBm.
  - The HF clock source is shown (crystal / RC / off).
  - `diag gap` lines for silences of 50 ms or more that lost frames; `diag outage` for over 1 s.
  - Always on: warnings for dropped packets (unknown id, bad checksum, out of sequence, repeats), and for stored trackers silent for 10 s.
  - Example lines:

    ```
    diag noise: median -94 dBm, quietest 10% -97, loudest 10% -72, 6.1% of samples above -80 dBm (990 samples)
    diag id 2: rx 186, lost 4 (2.1%), dup 3, late 0, max gap 21 ms, RSSI -68 (worst -79) |#########..x########|
    diag gap: id 2 silent 180 ms, 34 frames lost, RSSI -71 before, -83 after
    ```

  - Side effect: sampling RSSI rarely spoils one packet's RSSI byte (it reads -127).
- **OTA relay** (`src/ota.c/.h`, `src/ota_proto.h`):
  - `ota begin <size> <crc32> <board crc32> <tracker id>`, `ota status`, `ota stop`.
  - Image data arrives as HID OUT reports `[0xF5][offset u24][length][59 bytes]` into a 16 KB RAM window.
  - The status includes how the last update ended.
  - The console argv is raised to 8: the parser rejects a full array.
- **Build:** with `-DCONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n`. The dongle board's own USB console (new USB stack) clashes with the receiver's legacy one ("multiple definition of `__device_dts_ord_...`").
- **USB serial number:** the dongle shows the same one with stock and with this firmware.

### 7.2 Tracker (`Tracker/`)

- **Rate cap:** `CONFIG_SENSOR_UPDATE_TIME_MS` (6-33, default 10): the moving loop period. 10 ms gives 120 Hz on the LSM6DSV and about 94 Hz on the XIAO.
- **Retry delay:** `600 + ((a ^ a>>8 ^ a>>16 ^ a>>24) & 7) * 125` µs from `DEVICEADDR[0]` (600-1475 µs).
- **Flush kept:** a new frame replaces an unsent old one.
- **Clock fix:** `clocks_release_when_idle()`. The connection thread no longer stops the HF crystal under a frame that is still being sent.
- **Version handling:** `CMakeLists.txt` takes `FW_GIT_DESCRIBE` / `FW_GIT_BRANCH` from the environment (see 10).
- **OTA** (MCUboot builds only; `CONFIG_TRACKER_OTA`, `TRACKER_OTA_CONFIRM_ACKS` 200; files `src/ota.c/.h`, `src/ota_proto.h`, hooks in `esb.c`, `console.c`, `system/power.c`; layouts in `pm_static/mcuboot_*.yml`, `sysbuild/mcuboot*.{overlay,conf}`, `ota*.overlay`, `ota.conf`, `ota_noconfirm.conf`; `sysbuild.cmake` picks `mcuboot_<name>.yml`):
  - The update thread.
  - Self-confirm after 200 frames the receiver acknowledged.
  - Sleep and power-off requests wait while an update runs.
  - `info` shows `Image: confirmed|pending (n of 200 frames acknowledged), next boot: swap ..., OTA protocol 1` and the last update's outcome.
- **Charge estimate** (`BATTERY_CHARGE_ESTIMATE_CAPACITY_MAH` 180, 0 = off; `BATTERY_CHARGE_ESTIMATE_CURRENT_MA` 100):
  - While plugged in, the reported % = the last % before plugging in, plus the charge put in at 90% efficiency (180 mAh at 100 mA = 50%/h), never above the voltage-based %.
  - Plugged in at boot there is no starting point, so the voltage-based value is sent, as upstream.
  - Battery statistics and the low-battery check keep using the measured value.
  - Not tested on hardware yet.

### 7.3 Builds (`make`, from WSL)

| Target | Output |
|---|---|
| `tracker` | `dist/SlimeNRF_ProMicro_StackedSPI_Tracker.uf2` (plain, no MCUboot) |
| `tracker-xiao` | `..._XIAO_Sense_Tracker.uf2` |
| `tracker-ota` | `..._ProMicro_StackedSPI_Tracker_OTA.uf2` (MCUboot + app, USB) and `_OTA_app.bin` (the update); always pristine |
| `tracker-ota-xiao` | the same for the XIAO |
| `tracker-ota-slot1` | a slot-1 test image as UF2; `OTA_NOCONFIRM=1` makes one that never confirms |
| `receiver` | `SlimeNRF_Nordic_Dongle_Receiver.hex` + `.zip` |
| `test` | runs `test_slime_link` |
| `clean` | |

- Build dir: `~/build/slimevr-clean`.
- MCUboot is built for `nrf52840dk/nrf52840` with `K32SRC_RC` (no 32 kHz crystal), no console, size-optimized.

## 8. OTA (summary; details in docs/OTA.md)

- **Once per tracker, over USB:** flash `_OTA.uf2`.
  ```bash
  python tools/flash_usb.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA.uf2
  ```
  This does every matching tracker on USB in turn; `--only 0,3` limits it.
- **Each update:** commit, `python tools/bump_version.py`, `make tracker-ota`, then:
  ```bash
  python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker_OTA_app.bin --tracker <id>
  ```
- **Never USB-flash the plain tracker UF2** on an MCUboot tracker: it overwrites MCUboot. The `_OTA.uf2` repairs that.
- **Protocol 1** (`ota_proto.h`, identical in `Tracker/src` and `Receiver/src`; `PROTOCOL` in `ota.py`):

  | Message | Direction | Layout |
  |---|---|---|
  | OFFER 0xF1 (16 bytes) | ACK payload, pipe 1 | `[type][id][protocol][session][size u32][crc32 u32][board crc32 u32]` |
  | REQ 0xF3 (5) | tracker, pipe 7 | `[type][id][position u24]` |
  | CHUNK 0xF2 (4 + up to 28) | ACK payload, pipe 7 | `[type][position u24][data]` |
  | DONE 0xF4 (4) | tracker | `[type][id][status][protocol]` |

  - position = session << 20 | offset.
  - Status codes:

    | Code | Meaning | Code | Meaning |
    |---|---|---|---|
    | 0 | installed | 5 | wrong board |
    | 1 | flash error | 6 | too large |
    | 2 | CRC | 7 | running image pending |
    | 3 | timeout | 8 | same image |
    | 4 | protocol | | |

  - The start of OFFER and all of DONE stay the same in every version.
  - Trackers on 0.0.14 or older don't answer protocol 1 offers.
- **Board id:** crc32 of `CONFIG_BOARD_TARGET`, e.g. `promicro_uf2/nrf52840/stackedspi`, `xiao_ble/nrf52840/sense`.
- **Timeouts:**
  - The tracker gives up after 5 s without data and keeps its image.
  - The receiver ends an offer nobody answers, or a transfer that stopped, after 10 s.
- **Tested:**
  - MCUboot swap and rollback, over USB and over the radio.
  - Aborted transfer, wrong board, same image, update while pending, no answer.
  - Pairing kept throughout.
  - All seven trackers updated over the radio.
- **Not tested:**
  - Battery-powered resting tracker during an update (does the sleep hold work?).
  - Another protocol version, a corrupted transfer, power loss during the swap.
  - There is no watchdog: an image that hangs needs a power cycle before it rolls back.
- **Not built:** SMP (recommended against), signing (the user decided no), a version check (it installs older images).

## 9. PC tools

On `main` (`tools/`; Windows Python 3.14 with `pyserial` and `hidapi`):

| Tool | What it does |
|---|---|
| `slime_link.py` | Per-tracker link stats from any nRF receiver over HID, stock included; runs alongside SlimeVR Server and never writes to the receiver. Shows rate (and lowest second), RSSI last/avg/worst, gaps, offline count, battery, firmware version and date. Options: `--gap` ms (250; 50 for moving), `--offline` s (2), `--csv FILE`, `--duration`. Ctrl+C prints a summary. Test: `test_slime_link.py` |
| `slime_view.py` | Stand-in for SlimeVR Server with a 3D view per tracker and a simple skeleton (assign body parts, calibrate, ski-pose mounting) saved in `slime_view_config.json`. Source is chosen automatically, never both: USB receiver if plugged in, else ESP32 hub on UDP 6969 (`--usb` / `--wifi` force one). Over Wi-Fi quit SlimeVR Server first. Shows "charging" instead of the forced 4.31 V |
| `ota.py` | Update a tracker over the radio (section 8). Live progress bar; checks the receiver's protocol; Ctrl+C stops the offer |
| `flash_usb.py` | USB-flash a UF2 to every tracker plugged in (`dfu`, copy to its drive, show the new version); `--only`, `--board` |
| `dfu_zip.py` | `.hex` → DFU zip for the dongle's Nordic bootloader: app from 0x1000, unsigned init packet, `fw_version` 0xFFFFFFFF, `is_debug`, `hw_version` 52, `sd_req` 0, SHA-256 reversed (reverse-engineered from nRF Connect Programmer) |
| `bump_version.py` | Next `0.0.N` as an annotated tag on HEAD; refuses a dirty tree |
| `serial_monitor.py` | The user's tkinter serial monitor; settings in `serial_monitor_settings.json`, ignored by git |

Saved body assignments (`slime_view_config.json`, version 3):

| Body part | Address |
|---|---|
| Chest | D235DBBB9B7A |
| Hips | 6DF7EC0CB8C8 |
| Left thigh | 847BEBC50791 |
| Right thigh | DEE876FA4759 |
| Left lower leg | D81D4F94B220 |
| Right lower leg | 628DCFC6318A |

Only on `wired`: `slime_diag.py` (receiver diag through the hub on UDP 6971, `--log`, `--plain`).

Scratch-only helpers of earlier sessions:
- `con.py` (console query); `watch_thigh.py`; `flash_watch.py` (flashes by bootloader serial, never the receiver).
- `pair_one.py`; `handshake_responder.py` (answers SlimeVR handshakes so the hub learns the server).
- `make_erase_uf2.py`, which did not work.

## 10. Toolchain and builds

- **WSL:** builds run in Debian on WSL, from the repo at `/mnt/c/Users/Morgan/Lab/SlimeVR-Hub`, as `wsl.exe -e bash -lc 'cd ... && make ...'`.
- **nRF Connect SDK:** tracker in `~/ncs-tracker` (v3.1-branch, Zephyr 4.1.99, like upstream releases); receiver in `~/ncs-ws` (v3.3.0-preview3-branch).
  - Upstream receiver e3d66d5 does not build on v3.1 (`NRFX_TIMER1_INST_IDX`, `nrfx_gppi_handle_t`).
- **Other tools:** Zephyr SDK `~/zephyr-sdk-0.17.4`; west venv `~/ncs-ws/.venv`; cmake 3.30.2 and ninja 1.12.1 from `~/.espressif`; xz in `~/.local/xz`.
  - ESP-IDF v5.5.5 in `~/esp/esp-idf`; host C compiler = zig cc (uv-installed); no `cc` in WSL.
- **West flags:** `-DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT=<src>`, `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`.
- **Kconfig warnings are errors** (except "set more than once").
- **Sysbuild per-image options:**
  - `-D<image>_EXTRA_CONF_FILE`, `_BOARD` and `_DTC_OVERLAY_FILE` work.
  - `GIT_EXECUTABLE` is not passed on, hence the `FW_GIT_DESCRIBE` environment variable.
- **Version string:** `0.0.N-<branch>+<commits since tag>`, from `git describe --dirty --tags --long --match '[0-9]*.[0-9]*.[0-9]*'` (`VERSION_SRC` → `Tracker/VERSION`, git-ignored). Without tags: `0.0.0-unknown`.
- **Windows tools:**
  - nrfutil: `C:\Users\Morgan\AppData\Roaming\nrfconnect\nrfutil-sandboxes\8.1.1\device\2.19.3\bin\nrfutil-device.exe` (`list`, `program --firmware <zip> --serial-number FCF6929F4E52`). The plain `nrfutil.exe` lacks `device`. A `.hex` fails ("Could not find EOCD"): it needs the zip.
  - nRF Connect for Desktop apps: programmer, rssi, serial-terminal, board-configurator, quickstart.
  - esptool v5.5.0 on Windows Python. WSL cannot see USB serial without usbipd, so flash from Windows.
  - Python 3.14.7 at `C:\Users\Morgan\AppData\Local\Programs\Python\Python314`; PowerShell 7.6.6; git 2.55.0.windows.5.
- **Manual tracker flashing:** open the COM port at 115200 with DTR on, send `dfu`, wait for a drive with `INFO_UF2.TXT`, copy the UF2, and wait for the port to come back (1209:7692). `flash_usb.py` does exactly this.
- **Checking a UF2's content:** a UF2 block carries 476 payload bytes at offset 32 of each 512 bytes.
- **Debugging a crash:**
  ```bash
  arm-zephyr-eabi-addr2line -f -i -p -e zephyr.elf <pc>
  ```
  `-DCONFIG_STACK_USAGE=y` writes `.su` files.

## 11. The wired era (branch `wired`) in detail

- **Topologies tried:**
  1. Plan v3: ankle cabled to thigh, thigh relays by ESB, receiver on the hip mirrors to an ESP32 by UART at 1 Mbaud, Wi-Fi to SlimeVR.
  2. Full-wired chains (`hub/full-wired` + tracker chain mode): no radio, chains into an ESP32-S3.
  3. Hip relay `mode wired in both`: legs by wire to the hip, hip relays by ESB.
  4. Hub mode: the hip replaces the receiver and outputs to the ESP32 on D0 at 460800.
  5. Back to the relay, then abandoned for full wireless.
- **One tracker image for every role,** chosen at run time with `mode` (stored setting `wired_mode`; reboots):

  | Command | Role |
  |---|---|
  | `mode wireless` | chest / standalone |
  | `mode wired out right` | ankle |
  | `mode wired in left out right` | thigh |
  | `mode wired in left` | thigh relaying by radio |
  | `mode wired in both` | hip relay |
  | `mode hub` | hip as receiver |

  - Bits: 1 in left, 2 in right, 4 out left, 8 out right, 16 hub (19 = hub); -1 = legacy `connection_over_uart`.
  - The radio is on exactly when there is no wired output.
- **Headers and wire format:**
  - Left header: data D8, ground D9 (uart1). Right header: data D16, ground D10 (uart0). 460800 8N1, upstream only.
  - Straight cables; 470 Ω-1 kΩ series resistor in the data line. Ground pins driven low; each tracker keeps its own battery.
  - Old ankle firmware: data D8, ground D9.
  - Frames: `COBS([type][body]) + 0x00`, with types:
    - 0x01 DATA: legacy 21-byte ESB frame, id 0xFF.
    - 0x02 HELLO: address[6] + firmware version, once a second.
    - 0x03 ADATA: address[6], packet[16], CRC-32K/4.2 over both, sequence; 29 bytes on the wire.
- **Relay behaviour:**
  - It stamps the receiver id and recomputes the CRC.
  - Queue of 32 frames, up to 8 per pass; it waits for the previous ESB frame instead of flushing.
  - `pair_wired` / `forget_wired`; 6 button presses = `pair_wired`.
  - Background re-pair every 3 s while the receiver is silent.
- **Receiver fork** (stacked ProMicro build `promicro_uf2/nrf52840/stacked`, image `SlimeNRF_ProMicro_HubReceiver.uf2`):
  - UART mirror to the ESP32: P0.08, 1 Mbaud, `COBS(packet16 + crc16_itu_t(0xFFFF) LE) + 0x00`, a type-255 address packet per id every 100 ms.
  - Console: `hid on|off`, `mirror on|off`, `diag on|off`, `diag hub on|off` (types 253/254 to the hub), `radio 1m|2m`.
  - `clear` moves the receiver to a new radio address (pairing generation, NVS key 1000).
  - The pairing reply is sent before the NVS write.
  - Pairing button on P0.06.
- **ESP32 hub** (`src/Hub`, ESP-IDF, XIAO C6 default, S3 option, C5 for 5 GHz):
  - Decodes the mirror and speaks the SlimeVR UDP protocol (section 4.4).
  - Console `wifi <ssid> [password]`, `server <ip>|auto`, `reboot`.
  - Settings in NVS at 0x9000: the merged image erases them, app-only at 0x10000 keeps them.
  - Minimized on Oct 9: gap hold/blend, the stats JSON on UDP 6971, and the status table and counters were removed. Battery ADC kept.
  - Forwards packet types 224-254 raw to UDP 6971 (the approved exception).
- **Hub mode details:**
  - The hip listens like a receiver (same discovery address, pipe addresses from its own device address).
  - Id table of 16 in NVS; `pair` opens a 60 s window.
  - Output in the receiver's mirror format; the ESP32 must be built with `CONFIG_HUB_UART_BAUD=460800`.
- **Bugs fixed there** (rules that still apply):
  - **Boot loop:** `wired_init` with `snprintf` on the 512-byte main stack (SYS_INIT).
  - **power.c suspended the running right-header UART** (assert in `uarte_pm_suspend`) after a devicetree name change.
  - **Writing NVS and printing from the 256-byte system workqueue** overflowed it; it is now 1 KB on that board.
  - **Crash recorder:** `crash.c` keeps a record in retained RAM, with safe mode after 3 early crashes.
  - **Rearm race:** suspending a UART that had restarted RX. The fix 65e46228 was never flashed.
- **Known bug, not fixed:** an explicit `pair` on a relay forgets the previous receiver and keeps stale wired ids. Workaround: `forget_wired`, then `pair_wired`.
- **Saved working setup:** `dist/wired/hub-mode/SETUP.md` (ids, wiring, how to go back) with its tracker UF2 and the 460800 ESP32 app.
- **Other earlier ideas** (not built):
  - Auto-detecting cable direction.
  - A separate hip-to-receiver wire (`mode wired in both out aux`).
  - The receiver as a junction.
  - A shared 2-wire bus.
  - A third wire for power from a hip battery.
  - USB cables to the limbs (`connection_over_hid`).

## 12. Analyses: BLE, 1 Mbps, TDMA, power

- **BLE** (planned Oct 9, never written):
  - Plan: receiver as central, trackers as GATT peripherals sending frames as notifications; 7.5 ms interval, about 133 packets/s per tracker; MTU ≥ 66, 2M PHY; receiver outputs unchanged.
  - Shelved because it uses the same radios and antennas, so it hits the same decode floor.
  - Its real advantage is scheduled connection events (no collisions) and channel hopping, not range.
  - Estimated cost: 1.5-2× radio energy per update, about 10-20% battery life; a rewrite of both firmwares.
- **1 Mbps:** about 3-4 dB better sensitivity, but twice the airtime: more collisions and about 30-40% more radio energy per frame.
  - Measured margins with the dongle: 23-45 dB, worst -76 dBm, so not needed.
  - Worth revisiting only if a tracker's worst RSSI goes below about -80 dBm in real use (check with `diag on`).
  - `wired` has `radio_1mbps` / `radio 1m`; the receiver and all trackers must match.
- **ESB power** (estimates at +8 dBm, about 15 mA TX): about 0.5 mA average radio at 100 Hz, 0.8 mA at 166 Hz. IMU and fusion draw more than the radio.
- **Bit rate vs frame rate:** one tracker at 100 frames/s × 30 bytes is about 24 kbit/s. Lowering the frame rate cuts airtime; lowering the bit rate adds airtime.
- **TDMA:** upstream has pieces, not finished.
  - Receiver: `src/connection/tdma.h`, slot constants, a sync timer that is never started.
  - Tracker: uses the sync packet only for the LED.
  - Upstream's `tdma` branch (tag `tracker/0.0.1`) is 62 commits not in our base.
  - Finishing it (clock sync, slot assignment, drift, join/leave) is a real project in both firmwares.
- **The upstream receiver has other branches** too (`esp-passthrough`, `hannah/dongleHID`, `dev`, `nrf54`).

## 13. Quest-hosted receiver plan, charging and power

- **Spec v2** (`docs/Quest-Hosted SlimeNRF Receiver — Implementation Spec v2.md`, uncommitted, Oct 10; v1 alongside):
  - Step 0 measures first:
    - Six moving at 1 m.
    - 20 min of VRChat with poses: back turned at 3 m, crouching, lying down.
    - The dongle on an extension at the headset or hip.
    - Decision rule: no gap over 250 ms outside collisions and worst RSSI above -80 dBm in every pose → only raise the dongle.
  - Option A: the dongle in the Quest 3S over USB-C OTG, running the Android SlimeVR server; VRChat via OSC.
  - Option B: a Quest HID-to-UDP forwarder (UDP 6970, `SNRF` header) + a PC translator.
  - Option C (new): the dongle on a XIAO ESP32-S3 USB host on the hip, a new input for `src/Hub`. It needs 5 V VBUS from a power bank or boost.
  - Preferred order A → C → B.
  - Acceptance: under 5% loss, no silence over 250 ms, worst RSSI above -80, a 2 h endurance run.
  - On-Body Hub plan v3 and the ProMicro receiver are superseded.
- **v1 context:**
  - PC VRChat via Steam Link; SteamVR 2.17.10; PC on Ethernet, headset on 5 GHz.
  - Android server CI artifact `SlimeVR-android.apk` (minSdk 26).
  - The Oct 7 session found a byte-offset error in the spec (packet type 7).
- **Quest 3S charging:** USB-C PD about 18 W (9 V/2 A), not Qualcomm QC; a QC-only charger falls back to 5 V.
  - USB hubs (GL3510, VL822 controllers) give at most 7.5 W per port without PD, so the headset won't charge during play.
  - A 5 V/3 A input is a shared pool, not split evenly.
- **Tracker charging:**
  - The ProMicro clones charge at about 100 mA (assumed, not measured), so six need about 0.6 A.
  - Plan: a DIY dock of six magnetic USB-C cables from one 5 V/2 A supply, with a polyfuse per cable.
  - The tracker's power switch usually sits between battery and B+, so it must be ON to charge.
  - Rejected: a 7-port hub fused at 500 mA, and an ORICO MH4UC-C3 (about 20,000 KRW, only 4 ports).
  - The user has a USB-C breakout with only VBUS/GND/D+/D−; check for 5.1 kΩ CC resistors (marked 512).
- **Hub power** (plan v3): 102540 LiPo (about 1000 mAh) with TP4056 + DW01/FS8205, about 120-130 mA, 6-7 h.

## 14. Rules and preferences from the user

- **Full wireless is the goal;** cables were a workaround. "I need minimal setup", "i want minimal cables too".
- **Hub stays minimal:**
  - "HUB DOESNT "SEND" that data. it RELAYs." Never add hub (`src/Hub`) features for PC tools; decode what it already sends.
  - The one approved exception (Oct 9): raw pass-through of the receiver's diag packets to a second UDP port, enabled by a receiver setting, no subscribe step.
- **Commit before changing things:** "GIT COMMIT CURRENT. BEFORE YOU DO FUCKING STUPID THING AGAIN". One commit per removal. Commit only; no pushes unless asked.
- **Root causes, not more warnings:** "SOLVE THE DAMN tHING". Verify before claiming "identical" or "done": check generated and ignored files too.
- **Firmware changes the user did not ask for** are proposed first, not made.
- **New images go to a spare tracker first** (tracker 5, the right ankle), never first to the receiver or the hip. The receiver can only be tested on itself: flash it last.
- **Run tools and flashing for the user** ("you do that for me please").
  - Long jobs show progress ("output progress later on for me to see") and beep or report when done ("beep when done").
  - Only one flashing watcher at a time.
- **Versions:** patch level, `0.0.N`, bumped for every flashed rebuild ("i prefer 0.0.3"). Version from the environment / tags, as the user suggested.
- **Repository:**
  - One plain repository: no submodules, no nested repos.
  - Upstream histories as subtrees ("merge it").
  - A clean reimplementation keeping only what is used.
- **Tool design:**
  - Separate tools for separate jobs ("seperate viewer for diag data").
  - `slime_view` uses one source at a time ("NOT BOTH, EITHER USB or NET").
  - "stacked" means nothing for the receiver: leave it out of names.
  - Prefer streaming over store-then-send ("cant it do pipe tho?").
- **Configuration:** explicit per board ("we can't make it automatically configless"), one image for all boards. Avoid reflashing boards that work ("CANT I use 460,800 and instead of refalshing all").
- **Don't read other sessions' transcripts** unasked. The user declined it once.

## 15. Pitfalls and lessons

- **Hardware and radio:**
  - Measure the receiver end first. The weak link was the receiver's antenna, not the trackers.
  - USB power hides radio-clock bugs: USB keeps the crystal on. Test on battery.
  - Body shadowing costs 10-20 dB.
  - A board's USB cable acts as an antenna counterpoise, so USB vs battery comparisons are unfair.
- **ESB:** a queued ACK payload rides the *next* frame. Queueing ahead fell out of step; queue exactly what was asked.
- **Zephyr:**
  - The main stack is 512 bytes and the system workqueue 256 bytes. Never print, `snprintf` or write NVS there.
  - Run `CONFIG_STACK_USAGE` before flashing new init code.
- **UF2 bootloaders:** their write limits are undocumented. Read `CURRENT.UF2`, don't guess: 0xEE000 and 0xED000 were both wrong.
- **MCUboot:**
  - Build it for the DK board (the tracker defconfigs set tracker options it does not have).
  - Each board's layout is written three times (`pm_static/mcuboot_*.yml`, `sysbuild/mcuboot*.overlay`, `ota*.overlay`), and they must agree.
  - The XIAO board names its UF2 bootloader `boot_partition`, so its overlays replace the whole table.
- **Receiver console:**
  - It echoes every line, so status polls cut log lines. Read results from `ota status`.
  - Streaming data through it jammed it (fixed by HID OUT; it needed a replug).
  - `parse_args` rejects a full argv.
- **Git:**
  - A branch can be checked out in only one worktree.
  - WSL git cannot read a worktree made by Windows git.
  - `git stash` round trips can turn LF into CRLF; keep a byte copy of uncommitted user files.
  - `git subtree add` needs full hashes.
  - The auto-mode classifier blocked `git clean` in the main folder.
- **Shell and Python on Windows:**
  - Git Bash converts `/home/...` arguments for `wsl.exe` (`MSYS_NO_PATHCONV=1`).
  - Heredocs through the Bash tool lose tabs and turn `\r`, `\b`, `\\n` into real control characters: edit C and Makefiles with the Edit tool, or write edit scripts to files.
  - Windows Python `open()` defaults to cp1252. A script once emptied README.md (a "Slime View" section was lost for good). Always pass `encoding="utf-8"`.
  - `core.autocrlf` turned `.sh` files to CRLF, which broke scripts in WSL.
  - PowerShell: `"$port:"` in a string is a parse error (use `${port}`); `Add-Type -AssemblyName System.Media` errors, but the sounds still play.
  - A COM port held by another program gives "Access is denied".
  - WSL shares the host network: UDP 6969 is busy while SlimeVR runs.
  - `/tmp` in Git Bash is not visible to Windows Python: use the session scratchpad.
- **ESP-IDF** writes options into the ignored `sdkconfig` on reconfigure, so `git restore` doesn't undo them. Never commit the hub's `sdkconfig`: it holds Wi-Fi credentials.
- **Board identity:** identify boards by `info` device address, never COM port. When all boards are plugged in, cache the InstanceId → address mapping.

## 16. Open items

**OTA:**
- Battery test: a resting tracker on battery should hold off sleep during an update.
- Untested: another protocol version, a corrupted transfer, power loss during the swap.
- No watchdog. No version check: it installs older images.
- A resting tracker can take a minute to confirm a new image; if switched off before, it rolls back. Lowering `TRACKER_OTA_CONFIRM_ACKS` would fix that.

**Link quality:**
- Measure all six moving on the current firmware (0.0.3 or later); never done.
- Options left: 80 Hz, cutting the info packets from 100 ms to 1 s (the user chose "1 and 3"; only 3 was tried), TDMA, a second dongle.
- A fair XIAO vs ProMicro antenna test (both on USB, side by side).
- Antenna fixes for the two antenna-less boards. Confirm which ones they are.
- Run Quest spec v2 Step 0 (section 13).

**Untested on hardware:**
- The charge estimate: flash one tracker, plug it in, watch % climb about 0.8%/min.
- The XIAO receiver image (`wired`).
- Full-wired chains.

**Repository:**
- Decide on pushing `main` and `wired` to GitHub (force push; push `wired` first).
- Restore the `hub-mode-ok` tag (section 6).
- Delete `charge-estimate`, `SlimeVR-Hub-git-backup\`, and the empty `SlimeVR-Hub-clean\tools` folder.
- Commit or drop the user's uncommitted `tools/slime_view.py` change and the two Quest spec docs.
- README.md does not mention OTA or `flash_usb.py`.

**`wired` branch:**
- The `pair` bug.
- The rearm fix 65e46228 never flashed.
- Receiver diag not ported to hub mode.
- Old ankle firmware on the ankles.

**Earlier ideas parked:**
- Batteryless / power over a third wire.
- `.gitattributes` for LF `.sh`.
- The receiver "useless code" cleanup list from Oct 9.

## 17. Sources

Claude Code transcripts in `C:\Users\Morgan\.claude\projects\C--Users-Morgan-Lab-SlimeVR-Hub\`:

| File (prefix) | Covers |
|---|---|
| c61fd35d (+ forks af820ace, ba2bde6a, 58a53190, 922b5df5, 8c28367b) | Oct 9 02:04-06:19: receiver logging, hub minimizing, relay fixes, full-wired chains |
| 28af97f0, 644e3ae1 | Oct 9 06:20-Oct 10 01:33: BLE question, diagnostics, 1 Mbps, clock fix, diag via hub |
| cdcf6e0e | Oct 9 23:16: Quest 3S charging, USB hubs, charge dock |
| b9396045 (+ forks 8406ff16, ebee3e65) | Oct 10 01:34-08:19: hip relay, hub mode, boot loops, repo merge, BLE plan |
| 05fd2d56 | Oct 10 06:20-: dongle, diagnostics, clean branch, versions, OTA, this file (main session) |
| 326f79e1 | Oct 10 11:28-15:18: BLE vs ESB power, 1 Mbps, slime_view auto source, charge estimate, Quest spec v2 |

- Also: the Oct 7 session "Quest Slime Proxy" in `C:\Users\Morgan\.claude\projects\C--Users-Morgan-Downloads\19752556-....jsonl`.
- Old scratchpads under `C:\Users\Morgan\AppData\Local\Temp\claude\C--Users-Morgan-Lab-SlimeVR-Hub\<session>\scratchpad\`:
  - `diag1..8*.log`
  - `relay*.csv`, `fix.csv`, `single.csv`
  - `ota_run.log`
  - `memory_notes/`: the per-session notes this file was built from.
- The user's own logs `diag.csv` / `diag2.csv` were in the repo root (ignored as `*.csv`).
