# On-Body Hub — Implementation Plan v3

Oct 8, 2026 · @user

## Architecture

Every radio hop on the body is 40 cm or less, and the ESP32-S3 on the hip presents the trackers to SlimeVR directly, so no PC program runs during play. Ankles are wired to thighs, a receiver rides on the hip next to the ESP32, and the SlimeVR Server and SteamVR driver stay stock.

&#91;embedded content: data path · 6 trackers, 4 link types\]

Ankles reach the receiver through their thigh's radio; thighs, hip and chest transmit to it directly. The ESP32 speaks SlimeVR UDP, so the PC runs only the stock server; the optional stats logger sits outside the data path.

Three firmwares are written: a tracker fork (ankle mode and thigh relay), a small receiver fork (UART mirror), and a new ESP32-S3 firmware. An optional PC logger records statistics outside the data path.

### Changes from v2

| v2 | v3 | Why |
| --- | --- | --- |
| PC host program decodes, smooths and feeds SlimeVR | ESP32 decodes and feeds SlimeVR itself; PC logger optional | Removes one program from play and about 25–35 ms of buffering latency |
| `uart_relay` setting on the thigh | Relay turns on automatically when valid frames arrive on UART | One less setting; also tells the thigh to stay awake while relaying |
| Ankle pairs manually (`add` / `set` / `reboot`) and sends its own id | Ankle sends placeholder id 0xFF; thigh stamps the real id and recomputes the CRC32 | Ankle needs no setup; stamping is a byte rewrite per frame |
| Thigh relays one frame after each own frame | Each arriving frame wakes the send thread | A still or sleeping thigh would otherwise starve its ankle (sensor loop drops from 6 ms to 33 ms, then WOM sleep) |
| One UART pin, direction by setting | TX on P0.29, RX on P1.13 on every board | Pinctrl fixes direction at build time; one pin cannot be TX on one board and RX on another |
| Receiver address and id both needed on the ankle | Only the id is needed | The ankle never transmits by radio |

### Hardware in hand

| Role | Board | Count |
| --- | --- | --- |
| Ankle (wired, radio off) | Padless ProMicro nRF52840 | 2 |
| Thigh, hip, chest | Stacked ProMicro build | 4 |
| Receiver | Stacked ProMicro build | 1 |
| Hub | ESP32-S3 | 1 |
| Spares | 2 Chrysalis, Nordic dongle | — |

The design is written so an ESP32-C5 (5 GHz WiFi) can replace the S3 later with a recompile, if 2.4 GHz WiFi beside the receiver turns out to cause loss.

## Protocols

The 16-byte tracker packet is the unit everywhere; each link wraps it differently and unwraps it at the far end. Sources: `SlimeVR-Tracker-nRF` `cbfff16` (Sep 1 2026) and `SlimeVR-Tracker-nRF-Receiver` `e3d66d5` (Apr 4 2026). Pull the receiver repo before forking; it is six months behind.

### Layers

| Layer | Content | Ankle → thigh | Thigh → receiver | Receiver → ESP32 | ESP32 → server |
| --- | --- | --- | --- | --- | --- |
| Application | 16-byte packet, types 0–7; id in byte 1 | id = 0xFF placeholder | id stamped by thigh | byte 15 = RSSI (except types 1, 4) | decoded, re-encoded as SlimeVR UDP |
| Frame integrity | CRC32 bytes 16–19, sequence byte 20 | ankle's CRC, checked by thigh | thigh's CRC after stamping, checked by receiver | CRC16 per frame | UDP checksum |
| Link | — | UART 460800 8N1, COBS | ESB, auto-ACK, retransmit | UART 1 Mbaud, COBS | WiFi, UDP |

### ESB frame (unchanged from stock)

21 bytes: `[0..15]` packet, `[16..19]` CRC32 = `crc32_k_4_2_update(0x93a409eb, data, 16)`, little-endian, `[20]` per-tracker sequence. The receiver drops a frame whose CRC fails or whose sequence is out of order within 100 ms (`Receiver/src/connection/esb.c:150-175`). It keys only on byte 1; it never checks which radio sent the frame.

### Ankle → thigh wire frame

COBS-encoded, 0x00 delimiter, first byte after decoding = frame type.

| Type | Byte 0 | Body | Rate |
| --- | --- | --- | --- |
| DATA | 0x01 | the 21-byte ESB frame, byte 1 = 0xFF | about 190/s |
| HELLO | 0x02 | 6-byte FICR device address, u8 fw major, u8 fw minor, u8 fw patch | 1/s |

Maximum encoded length 25 bytes. A third conductor is wired in the cable for a future downlink (LED sync, commands) but carries nothing in v3.

### Receiver → ESP32 wire frame

COBS(`[16-byte packet][CRC16-CCITT, little-endian]`) + 0x00, 1 Mbaud, TX only, receiver P0.29. Up to 19 bytes encoded. Packet types: 0–7 tracker data (byte 15 = RSSI stamped by the receiver, except types 1 and 4 where byte 15 is data), 255 address packet (byte 1 = id, bytes 2–7 = tracker radio address, built like `packet_device_addr()` in `src/hid.c`). At about 1,140 packets/s this is about 22 KB/s, a fifth of the line rate.

### ESP32 → SlimeVR Server (stock SlimeVR UDP, big-endian)

One UDP socket per tracker to `<server>:6969`, MAC = the tracker's 6-byte radio address, so the server keeps body assignments. Per tracker:

| Packet | When | Content |
| --- | --- | --- |
| 3 Handshake | on first type 0 packet | board 4 (CUSTOM), IMU id from type 0 byte 8, MCU 0, protocol 22, firmware string, MAC |
| 15 SensorInfo | after handshake | sensor 0, status 1, type = IMU id |
| 17 RotationData | every decoded quaternion | sensor 0, data type 1, quaternion, calibration 0 |
| 4 Acceleration | with each rotation | 3 floats |
| 12 BatteryLevel | 1 Hz | voltage, level 0..1 |
| 19 SignalStrength | 1 Hz | RSSI |
| 0 Heartbeat | when idle > 500 ms | — |
| 10 PingPong | on request | echo |

Each socket has its own int64 packet counter, strictly increasing. Keep the raw sensor-frame quaternion; the server applies mounting offsets itself.

### ESP32 stats stream (for the optional PC logger)

One JSON line per second over UDP to port 6971 on the server host: per tracker packets/s, worst gap in ms, sequence gaps, RSSI, battery; per hub UART frames ok/bad, WiFi RSSI, free heap, uptime. The logger appends to a CSV per session. Nothing in the data path depends on it.

## Tracker firmware (fork of SlimeVR-Tracker-nRF)

One image for every tracker. A board with nothing configured behaves exactly like stock; `connection_over_uart` turns it into an ankle; any board relays automatically when frames arrive on its UART RX. New code lives in `src/connection/wired.c` and `wired.h`; changes to existing files are listed per touchpoint.

### Settings

One new bit in the `CONFIG_0` group (`src/config.h`, `src/config.c`), appended after `CONFIG_0_CONNECTION_OVER_HID` and before `CONFIG_0_SETTINGS_END`, name `connection_over_uart` in `config_settings_names[]`, default off via Kconfig `CONFIG_CONNECTION_OVER_UART`. Group 0 is a `uint16_t` bitfield with 6 of 16 bits used, so stored configs and the base64 export on flashed boards stay valid; the new bit takes the Kconfig default until overridden. Set from the console with `write_config connection_over_uart 1` then `reboot`.

### Devicetree (promicro\_uf2\_stackedspi.dts and promicro\_uf2.dts)

- Enable `uart0` with the async API: `CONFIG_UART_ASYNC_API=y`, `CONFIG_UART_0_ASYNC=y`, `CONFIG_UART_0_NRF_HW_ASYNC=y` with a TIMER instance for the RX byte counter, `current-speed = <460800>`.
- Pinctrl: TX `NRF_PSEL(UART_TX, 0, 29)`, RX `NRF_PSEL(UART_RX, 1, 13)` with `bias-pull-up` on RX. Both fixed for every board; the cable crosses them.
- `uart0` shares its instance with `i2c0` on nRF52840. The stacked SPI build deletes `i2c0`, so it is free there. The base and I2C builds enable `i2c0`; on those, use `uart1` instead or leave the overlay off.
- Pin check: the stacked build uses P0.02, P0.06, P0.11, P0.17, P0.20, P0.22, P0.24, P1.00, P1.11, plus P0.15 for the LED; the battery ADC reads `VDDHDIV5`, not an AIN pin. P0.29 and P1.13 are free. P0.29 is the Chrysalis blue LED, so a Chrysalis cannot be an ankle or thigh without a different pin.

### Ankle mode (`connection_over_uart`)

| # | Where | Change |
| --- | --- | --- |
| 1 | `esb_thread()` in `src/connection/esb.c:505` | If the setting is on: never call `esb_pair()` or `esb_initialize()`; set `esb_paired = true` so `esb_ready()` reports true (`sys_WOM()` in `src/system/power.c:296` otherwise delays sleep 30 s per attempt); then the thread exits. `tx_errors` stays 0, so the connection-error status and the 5-minute shutdown never trigger. |
| 2 | `connection_thread()` in `src/connection/connection.c:499` | `esb_write(data_copy)` becomes `if (wired_tx_mode) wired_write_data(data_copy); else esb_write(data_copy);`. `data_copy[1]` is already `tracker_id`, which stays 0; `wired_write_data()` overwrites byte 1 with 0xFF before computing the CRC32, so the ankle's own CRC covers the placeholder. |
| 3 | `wired.c` | `wired_write_data()`: COBS-encode `[0x01][21 bytes]` into a 25-byte buffer, `uart_tx()` async. If the previous TX is still running, drop the new frame (the next sample replaces it, same policy as `esb_flush_tx()`). |
| 4 | `wired.c` | A 1 s timer sends HELLO: `[0x02][6-byte FICR DEVICEADDR][major][minor][patch]`. |
| 5 | `connection.c` | `connection_write_packet_0()` still fills board, MCU and IMU ids; the receiver and ESP32 use them unchanged. |

The ankle has no link-loss detection: a UART write cannot fail. If an unplugged ankle should sleep, add a timeout in a later version.

### Thigh relay (automatic)

| # | Where | Change |
| --- | --- | --- |
| 1 | `wired.c`, boot | Configure RX pin as GPIO input with pull-up and a GPIOTE sense interrupt on falling edge. UART stays off. |
| 2 | `wired.c`, first edge | Start UART async RX with DMA into two 64-byte buffers (`uart_rx_enable`, `UART_RX_BUF_REQUEST` swaps). The first frame is lost; the ankle sends 190/s. |
| 3 | `wired.c`, parser thread | COBS-decode into a 32-byte buffer. Accept type 0x01 only if decoded length is 22 and `crc32_k_4_2_update(0x93a409eb, &buf[1], 16)` matches bytes 17–20. Accept type 0x02 only if length is 10. Count and drop everything else. |
| 4 | `wired.c`, stamping | On HELLO: look the address up in the flash table (below); cache `ankle_id` or `-1`. On DATA: if `ankle_id < 0`, drop and log "ankle not paired" at most once per 10 s. Else set byte 1 = `ankle_id`, recompute CRC32 over bytes 0–15, keep the ankle's sequence byte, push the 21-byte frame to `k_msgq relay_q` (depth 4, `K_NO_WAIT`, purge oldest when full), `k_wakeup(connection_thread_id)`. |
| 5 | `wired.c`, idle | A 1 s `k_work_delayable` restarted on every valid frame. On expiry: `uart_rx_disable()`, re-arm the GPIO sense, clear `relaying`. |
| 6 | `esb.c`, `event_handler()` | Add `K_SEM_DEFINE(esb_tx_done, 0, 1)`; `k_sem_give()` on `ESB_EVENT_TX_SUCCESS` and `ESB_EVENT_TX_FAILED`. |
| 7 | `esb.c`, `esb_write()` | Replace the unconditional `esb_flush_tx()` with `k_sem_take(&esb_tx_done, K_MSEC(4))`; on timeout, flush as before. The default `ESB_DEFAULT_CONFIG` retransmit settings (600 µs, 3 retries) can take about 2.5 ms per failed frame, so 3 ms is too tight. |
| 8 | `esb.c` | While `relaying`, skip `clocks_stop()` in the TX callbacks; keep the HF clock on so each frame does not pay the restart. The thigh is a stacked board on a tracker battery; measure the cost (expected 0.5–1 mA). |
| 9 | `connection.c`, `connection_thread()` | At the top of each loop pass: `while (k_msgq_get(&relay_q, frame, K_NO_WAIT) == 0) esb_write(frame);` before the thigh's own `last_data_time` branch. The thread is woken by step 4, so the relay runs at the ankle's rate. |
| 10 | `src/sensor/sensor.c` | Where `set_update_time_ms(33)` and the low-power mode are chosen (`sensor.c:895-899`), and in the IMU-timeout and active-timeout checks (`sensor.c:598-629`): add `&& !wired_relaying()`. A relaying thigh stays at 6 ms and awake. |

### Ankle table in flash

`struct { uint8_t addr[6]; uint8_t id; } wired_ankles[4]` stored with `sys_write()` under a new NVS id next to `PAIRED_ID` in `src/retained.h`. HELLO addresses are matched against it. Four entries allow swapping ankles between thighs without re-pairing.

### Pairing an ankle through the thigh

Runs only on the console command `pair_ankle` or a 5-press button action on the thigh, and only while a HELLO address is cached that is not in the table. Never automatic, so a flaky cable mid-session cannot stall the thigh's own tracking.

1. Stop the thigh's own frames (`connection_thread` pauses on a flag) and `esb_deinitialize()`.
2. `esb_set_addr_discovery()`, `esb_initialize(true)`, build `tx_payload_pair` with the ankle's address and `crc8_ccitt(0x07, addr, 6)` (0 → 8), exactly as `esb_pair()` does at `esb.c:388-395`.
3. Send packet `[1]=0`, then packet `[1]=1` repeatedly every 1 ms up to 20 times until an 8-byte ACK payload with byte 0 = checksum arrives. This covers the receiver's slow reply (see Receiver). Give up after 10 one-second rounds.
4. Store `{addr, ack[1]}` in the table, log `Ankle <addr> paired as id <n>`.
5. `esb_deinitialize()`, `esb_set_addr_paired()`, `esb_initialize(true)`, resume own frames.

The receiver must be in pairing mode (`pair` in its console, or its button). It stores the ankle's address in the next slot and returns that slot as the id; an address already stored returns the same id (`esb_parse_pair()`, `Receiver/src/connection/esb.c:418-442`).

### Airtime

A relaying thigh sends about 380 frames/s (190 own + 190 relayed), each about 300 µs with ACK at 2 Mbps. Six trackers' worth of traffic on one channel is about 1,140 frames/s, about 35% airtime. If the gap statistics show collisions, cap the thigh's own rate to every second sample while relaying.

## Receiver firmware (fork of SlimeVR-Tracker-nRF-Receiver)

The receiver keeps USB HID exactly as it is and additionally mirrors every tracker packet out of P0.29 at 1 Mbaud. Pull `main` first; this checkout is from April.

### UART mirror

| # | Where | Change |
| --- | --- | --- |
| 1 | `src/hid.c`, start of `hid_write_packet_n()` | After the RSSI byte is written into `report.data[15]` (types other than 1 and 4): `k_msgq_put(&mirror_q, report.data, K_NO_WAIT)`. This runs inside the ESB event handler (`esb.c:187`), so it must never block. On failure increment `mirror_dropped`. |
| 2 | new `src/mirror.c` | `K_MSGQ_DEFINE(mirror_q, 16, 64, 4)`. The HID FIFO merges reports per tracker, the mirror does not, so size for bursts: 64 entries covers 50 ms at full rate. |
| 3 | `mirror.c`, thread priority 7 | Pop, append CRC16-CCITT (poly 0x1021, init 0xFFFF) little-endian, COBS-encode into 20 bytes, append 0x00, `uart_tx()` async. Serialise with a semaphore given in the `UART_TX_DONE` callback. |
| 4 | `mirror.c`, 100 ms timer | For each `id < stored_trackers`, queue a 255 packet built like `packet_device_addr()` (`hid.c:103`). Six trackers add 60 packets/s. |
| 5 | `boards/nordic/promicro_uf2/promicro_uf2.dts` | Enable `uart0`, TX on P0.29, `current-speed = <1000000>`, async API Kconfig as on the tracker. The receiver build has no `i2c0`, so `uart0` is free. |
| 6 | `mirror.c`, 10 s log | `LOG_INF("mirror: %u sent, %u dropped")`. |

With no USB host attached the HID path fails quietly and the mirror is unaffected, because the hook runs before the report FIFO.

### Pairing reply timing fix (optional, recommended)

Stock pairing loses a race: the tracker sends its second packet about 100 µs after the first, but the reply is built in `esb_thread` (priority 6, polling with `k_usleep(1)`) and, for a new address, only after two NVS writes in `esb_add_pair()`. Fix in `src/connection/esb.c`:

1. In `event_handler()`, case pipe 0 with `pairing_buf[1] == 0`: look the address up in `stored_tracker_addr[]`, compute the checksum, fill `tx_payload_pair` and `esb_write_payload()` immediately. If the address is new, use `stored_trackers` as the id and set a flag.
2. In `esb_pair()`'s loop, on the flag: call `esb_add_pair()` (the NVS writes) after the reply is already queued.
3. Stop resetting `pairing_buf[1] = 255` on loop passes that processed nothing.

The thigh's 20-attempt retry (Tracker firmware, pairing step 3) makes ankle pairing work without this fix; the fix helps the stock radio pairing of every tracker.

### Unchanged

ESB receive, sequence and CRC checks, HID reports, console commands, VID 0x1209 / PID 0x7690.

## ESP32-S3 firmware (new, ESP-IDF)

The ESP32 reads the receiver's UART stream, decodes it and presents each tracker to SlimeVR as a stock WiFi tracker. It holds per-tracker state but no configuration; adding a tracker needs no change. Use ESP-IDF rather than Arduino so the same tree builds for an ESP32-C5 later.

### Modules

| Module | Task | Core | Notes |
| --- | --- | --- | --- |
| `uart_rx` | UART1, 1 Mbaud, 2 KB DMA ring, COBS deframer, CRC16 check | 1 | counts `uart_ok`, `uart_bad`; pushes 16-byte packets to a FreeRTOS queue of 128 |
| `decoder` | port of `HIDCommon.kt` for types 0–7 and 255 | 1 | see below |
| `trackers` | per-id state: address, last quaternion, accel, battery, RSSI, last-seen time, output socket | 1 | up to 16 ids; created on first 255 packet |
| `slime_out` | SlimeVR UDP client per tracker, server discovery | 0 | reuse the packet layouts from SlimeVR-Tracker-ESP `network/packets.cpp` |
| `gap_hold` | 100 Hz tick, re-sends the last pose for a quiet tracker | 0 | see below |
| `stats` | 1 Hz JSON line to port 6971 | 0 | optional PC logger |
| `net` | WiFi station, `WIFI_PS_NONE`, credentials in NVS set over USB serial | 0 | reconnect with backoff |

### Decoder (port of `HIDCommon.kt`)

| Type | Fields used | Notes |
| --- | --- | --- |
| 0 | batt `b[2] & 127`, charging `b[2] >> 7`, voltage `(b[3] + 245) / 100` V, temp, board `b[5]`, MCU `b[6]`, IMU `b[8]`, mag `b[9]`, fw date `b[10..11]`, version `b[12..14]`, RSSI `b[15]` | triggers the handshake if not yet sent |
| 1 | quaternion Q15 `b[2..9]` (w, x, y, z order as in the server), accel Q7 `b[10..15]` | no RSSI |
| 2 | batt, voltage, temp, quaternion exp-map `b[5..9]`, accel `b[10..15]`, RSSI `b[15]` | reduced precision |
| 3 | server status `b[2]`, status `b[3]`, RSSI | log only |
| 4 | quaternion Q15, magnetometer `b[10..15]` | no RSSI; mag unused |
| 5 | runtime `b[2..9]` µs, RSSI | log only |
| 6 | button `b[2]`, sleep time `b[3..4]`, RSSI | log only |
| 7 | button, sleep time, quaternion exp-map, accel, RSSI | reduced precision |
| 255 | id `b[1]`, radio address `b[2..7]` | creates or confirms the tracker entry |

Byte 15 is RSSI only for types 0, 2, 3, 5, 6, 7. Keep the raw quaternion; the server applies mounting.

### SlimeVR device emulation

- A tracker is created on its first 255 packet and sends Handshake on its first type 0 packet. Data before that is dropped.
- Server discovery: broadcast Handshake to `255.255.255.255:6969` until a reply; then unicast. Respond to packet 10 PingPong. If no server packet for 5 s, return to discovery.
- Each socket: int64 packet counter, big-endian, strictly increasing.
- A tracker silent for 30 s is dropped; the server shows it as disconnected. Its MAC keeps the body assignment for when it returns.

### Gap hold

Every 10 ms, for each tracker whose last real sample is older than 10 ms and newer than 2 s: re-send the last quaternion and acceleration. The server's `TIMED_OUT` is 2 s; a hold up to 2 s hides radio gaps without inventing motion. When data resumes after a gap longer than 50 ms, slerp from the held pose to the new one over 50 ms so the limb does not snap. Every gap longer than 20 ms is counted per tracker for stats.

### Stats line (1 Hz)

`{"t":<uptime ms>,"uart_ok":n,"uart_bad":n,"wifi_rssi":n,"heap":n,"trk":[{"id":n,"pps":n,"gap_max_ms":n,"gaps":n,"rssi":n,"batt":f}]}` to `<server>:6971`. The PC logger is a 40-line Python script that appends to `session-YYYYMMDD-HHMM.csv`.

### Latency budget (design estimate, not measured)

| Stage | Typical |
| --- | --- |
| Ankle → thigh wire + relay queue | 1–4 ms |
| ESB to receiver | about 1 ms |
| Receiver → ESP32 UART | under 1 ms |
| Decode and UDP send | under 1 ms |
| WiFi | 2–10 ms, occasional spikes |
| **Total** | **5–15 ms** |

## Hardware

The hub is one 102540 cell, a protected charger, the receiver and the ESP32-S3 in a hip pouch; no boost converter is needed because the receiver is powered from the cell and talks UART, not USB.

### Hub power chain

```
102540 LiPo (about 1000 mAh) ─► TP4056 + DW01/8205 protection ─┬─► ESP32-S3 board 3V3 regulator input (VIN/5V pin accepts 3.3–4.2 V on most boards; check yours)
                                                           └─► Receiver ProMicro B+/RAW
100–220 µF low-ESR at the ESP32 supply for WiFi TX peaks of about 350 mA
```

| Item | Value |
| --- | --- |
| Average draw | about 120–130 mA (ESP32 WiFi with power saving off, plus receiver 10–15 mA); measure on the bench |
| Runtime | about 6–7 h at 80% usable capacity |
| Charger | TP4056 module with DW01 + FS8205, 1 A set resistor, micro-USB or USB-C |
| Battery ADC | optional divider to an ESP32 ADC pin for the stats line |

If the ESP32 board's regulator drops out below 3.6 V input, feed it from a small 3.3 V LDO or buck off the cell instead of the VIN pin.

### Wiring

| From | To | Notes |
| --- | --- | --- |
| Receiver P0.29 (UART TX) | ESP32 RX GPIO (UART1) | 3.3 V logic both sides; keep under 10 cm |
| Receiver GND | ESP32 GND | common ground |
| Ankle P0.29 (TX) | Thigh P1.13 (RX) | the cable crosses TX to RX |
| Ankle GND | Thigh GND |  |
| Ankle P1.13 (RX) | Thigh P0.29 (TX) | wired in the cable, unused in v3 |

### Ankle cable

Three conductors (TX, GND, spare), stranded silicone 28 AWG, twisted, about 60 cm, down the outside of the leg with a slack loop at the knee. Magnetic breakaway connector at the ankle (3-pin pogo type). Pull-up on the thigh RX pin is internal (`bias-pull-up`), so an open connector reads idle-high instead of noise.

### Placement and 2.4 GHz

WiFi on the ESP32-S3 and ESB on the receiver share the 2.4 GHz band, a few centimetres apart. This is the one unproven point of the design.

- Mount the receiver with its antenna vertical on one side of the pouch and the ESP32 with its antenna on the far side, battery between them.
- Set the router's 2.4 GHz channel at the opposite end of the band from the ESB channel. Read the ESB channel from `esb_initialize()` in the receiver (`config.rf_channel` or the default) and record it in the logger config.
- Test step 2 measures the effect. If gap statistics for hip and chest are worse than with the receiver on the desk, swap the S3 for an ESP32-C5 and run WiFi on 5 GHz; nothing else changes.

## Build and test order

Each step has a pass criterion measured with the stats logger, so a regression at a later step points at the step that introduced it.

1. **Receiver mirror + ESP32 on the bench.** Receiver on the ESP32's UART, powered from the bench supply, not plugged into the PC. Trackers appear in SlimeVR Server as WiFi devices with the right body assignments; rotation matches the USB HID path side by side; `uart_bad` is 0 over 10 minutes; packets/s per tracker matches the HID rate within 5%.
2. **Hub on the hip, no ankles, 30 minutes of play.** Thighs, hip and chest: worst gap under 50 ms in 99% of seconds, no `TIMED_OUT` in the server log. Compare with the same session on the desk receiver to measure the WiFi–ESB clash.
3. **Tracker fork, nothing enabled, on every board.** Behaves exactly like stock: pairing, sleep, battery, and the same gap statistics as step 2. This is the regression check for the devicetree and the relay hooks.
4. **One ankle and one thigh on the bench.** `connection_over_uart` on the ankle, cable to the thigh, receiver in pairing mode, `pair_ankle` on the thigh. The ankle appears under its own id and address; receiver console shows no CRC, length or sequence errors; the thigh keeps relaying for 5 minutes while lying still; ankle sleep and wake work with the cable attached.
5. **Full set on the body, 2-hour session.** All six trackers meet the step 2 gap target; hub battery voltage and ESP32 stats logged for the whole session; thigh battery drain compared with a non-relaying thigh.

Steps 1–2 need only the receiver fork and the ESP32 firmware. If step 2 already fixes the dropouts, the ankle wiring (steps 3–5) can wait.

## Open items and risks

- [ ] Pull `SlimeVR-Tracker-nRF-Receiver` `main` and re-check `esb.c` and `hid.c` line references; the checkout is from April and the header already sketches a new pairing protocol (`0xCD`, TDMA).
- [ ] Confirm the ESP32-S3 board's VIN regulator accepts 3.3–4.2 V; otherwise add a 3.3 V LDO.
- [ ] Verify `ESB_DEFAULT_CONFIG` retransmit values in the installed NCS (`retransmit_delay`, `retransmit_count`) and adjust the 4 ms TX-done timeout.
- [ ] Measure thigh current with the HF clock held on while relaying; if above 1 mA extra, restore `clocks_stop()` and accept the restart latency.
- [ ] Decide whether the hip and chest trackers should also pair through the stock radio path with the receiver pairing fix, or stay manual.
- [ ] Choose the ESB channel and the router's 2.4 GHz channel before step 2, and record both.
- [ ] After step 5, decide the gap-hold limit (2 s default) and whether to add a slower held-pose fade.
- [ ] Whether to propose the receiver UART mirror and the pairing timing fix upstream.

| Risk | Effect | Mitigation |
| --- | --- | --- |
| 2.4 GHz WiFi beside the ESB receiver | packet loss on all trackers, recreating the dropouts | channel separation, antenna placement, step 2 measurement, ESP32-C5 fallback |
| Thigh pairing code breaks the thigh's own link | thigh stops tracking after a pairing attempt | pairing only on explicit command; `esb_deinitialize()` / re-init path tested on the bench in step 4 |
| Upstream adds a `CONFIG_0` bit 6 | fork's `connection_over_uart` collides with it | rename on merge; configs are per board and re-written from the console |
| Receiver mirror queue overflow in bursts | packets lost before the ESP32 | 64-entry queue, `mirror_dropped` counter in the 10 s log |
| Ankle unplugged never sleeps | ankle battery drains on the shelf | power off by button; add a no-link timeout in a later version |
