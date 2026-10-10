# Quest-Hosted SlimeNRF Receiver — Implementation Spec

Oct 8, 2026 · @user

## Context and goal

Move the SlimeNRF receiver off the desk and onto the body (hip, wired to the Quest 3S), so every tracker is within 1 m of it with no body shadowing. Ankle trackers currently lose the link, sit 5 minutes in connection error, and deep-sleep; the desk receiver is the common factor.

What exists today:

- 6 stacked ProMicro nRF52840 + LSM6DSV trackers (SlimeVR-Tracker-nRF, stacked SPI build, commit cbfff16ebed1 / CI 2026-10-05) and 2 Chrysalis boards on order. Receiver: a ProMicro board flashed with SlimeVR-Tracker-nRF-Receiver v0.6.9 (same CI run); a Nordic nRF52840 dongle on order.
- Receiver USB identity: VID 0x1209, PID 0x7690, one vendor HID interface, 64-byte reports; each report holds up to four 16-byte tracker packets. The PC SlimeVR Server (v21.1.0) already reads this device directly via hidapi.
- The official SlimeVR Server has an Android build (`server/android` in SlimeVR-Server, CI artifact `SlimeVR-android.apk`) with `AndroidHIDManager`: it opens the same VID/PID over Android USB Host and ingests tracker packets. This is the basis of Option A.
- The Quest streams PC VR via Steam Link; VRChat on PC; SteamVR 2.17.10; PC on Ethernet, headset on 5 GHz.

Two deliverables, in priority order:

1. Option A: try the stock Android server on the Quest with the receiver on USB OTG and OSC trackers to VRChat. Zero new code; one evening to evaluate.
2. Option B: if A fails or SteamVR-wide trackers are needed, build a dumb Quest HID-to-UDP forwarder and a PC-side translator that feeds the PC SlimeVR Server over its existing UDP tracker protocol.

Sources read while writing this: SlimeVR-Tracker-nRF-Receiver `src/hid.c`, `prj.conf`; SlimeVR-Tracker-nRF `src/connection/esb.c`; SlimeVR-Server `server/android/.../AndroidHIDManager.kt`, `HIDCommon.kt`.

## Option A: stock SlimeVR Android server on the Quest (no code)

The official Android build already does the USB-host part: `AndroidHIDManager` claims VID 0x1209 / PID 0x7690 over `UsbManager`, parses the same packets as the desktop server, and the app ships a `ForegroundService` so it survives the headset switching to Steam Link. The web GUI runs in a WebView inside the app (minSdk 26, so Quest's Android 12+ qualifies). Output to VRChat goes over OSC trackers across the LAN; VRChat on PC accepts up to 8 OSC trackers natively.

Steps for the next session:

1. Get `SlimeVR-android.apk` from the SlimeVR-Server CI artifacts (workflow `build.yml`, job "Build Android APK") or a release that includes it. Match the server version to the PC server (v21.1.0) where possible.
2. Sideload to the Quest 3S (developer mode, `adb install`). Grant USB permission when the receiver is attached.
3. Plug the receiver into the headset via a USB-C OTG adapter; for charging at the same time, a PD-capable OTG hub.
4. Open the app, confirm the receiver and trackers appear (6 x LSM6DSV). Assign body parts and enter body proportions in its GUI, same as on PC.
5. Settings -> OSC -> VRChat: enable, set the network address to the PC's LAN IP, port out 9000, port in 9001. OSCQuery is on by default and may auto-discover VRChat; set the IP explicitly anyway.
6. On PC, enable OSC in VRChat (Action Menu -> Options -> OSC). Do not run the PC SlimeVR Server's SteamVR driver at the same time; VRChat must see trackers from one source only.
7. In VRChat, Calibrate FBT; expect 6 spheres.

Controls while playing: full/yaw/mounting resets come from the Android GUI or tap detection on the trackers; there is no SteamVR driver in this path, so resets bound to controller buttons on PC will not reach the headset-side server. Check whether the Android build exposes the OSC reset commands VRChat can send (`/avatar/parameters` based reset triggers) and, if not, rely on tap-to-reset.

Limits of Option A: trackers exist only inside VRChat (OSC), not in SteamVR, so other FBT games get nothing; OSC trackers do not carry tracker battery level to the PC; latency adds one WiFi hop (a few ms). If any of these matter, go to Option B.

## Option B: Quest HID-to-UDP forwarder + PC translator (custom)

Two small programs. The Quest side is deliberately dumb: raw HID reports out over UDP, no parsing. The PC side parses them and speaks the SlimeVR UDP tracker protocol to the PC server on port 6969, so the PC server, its SteamVR driver, resets, OSC and everything else keep working unchanged. No modification to the SlimeVR Server.

### B1. Quest forwarder (Android, Kotlin)

- Target: Quest 3S, minSdk 29 is fine. Sideloaded via adb; no store.
- Permissions: `android.hardware.usb.host` feature; `INTERNET`; `FOREGROUND_SERVICE` (+ `FOREGROUND_SERVICE_CONNECTED_DEVICE` on API 34+). Add a `device_filter.xml` with vendor-id 4617 (0x1209) and product-id 30352 (0x7690) and an `ACTION_USB_DEVICE_ATTACHED` intent filter so attaching the dongle opens the app and skips the permission dialog after the first grant.
- Device open: `UsbManager.getDeviceList()`, filter VID/PID, `requestPermission`, `openDevice`, find the interface with class 3 (HID), `claimInterface(iface, true)`, locate the interrupt-IN endpoint (direction IN, type INTERRUPT, max packet 64). Android does not bind vendor-page HID devices as input, so the claim succeeds without root.
- Read loop in a foreground service thread: `bulkTransfer(epIn, buf, 64, timeoutMs)` works for interrupt endpoints on Android; use `UsbRequest` + `requestWait()` if bulkTransfer shows latency. Each successful read is exactly one 64-byte report.
- Send: one UDP datagram per HID report, 64 bytes payload, prefixed by an 8-byte header: 4-byte magic `SNRF`, 4-byte little-endian sequence counter. Destination: PC LAN IP and port, both set in the app (persist in SharedPreferences); default port 6970. Also send the receiver's USB serial string once per second in a separate 'hello' datagram so the PC can key device identity on it.
- Keep-alive: if no HID report for 500 ms, send an empty heartbeat datagram so the PC side can distinguish 'quiet trackers' from 'forwarder gone'.
- UI: one screen: PC IP, port, connect/disconnect, counters (reports/s, datagrams/s, USB state). Nothing else.
- Power: acquire a partial wake lock in the service; Quest kills background apps aggressively, which is why the foreground service with a persistent notification is required. Test that the service survives launching Steam Link and sitting in VRChat for an hour.
- Expected load: 6 trackers at \~180 packets/s with up to 4 packets per report is 300 to 1100 reports/s, i.e. 20 to 70 KB/s. Trivial for WiFi, but send on a dedicated thread and never block the USB read loop on the socket.

### B2. PC translator (Python 3.11+ or Rust; Python is enough)

- Listen on UDP 6970. Drop datagrams with a bad magic or a sequence number lower than the last seen (late duplicates), log gaps.
- Split each 64-byte report into four 16-byte packets; skip packets whose type byte is 0xFF or whose id byte is 0xFF (padding). Packet layouts and scales are in the Protocol reference section.
- Maintain one virtual SlimeVR device per tracker id (0..7). Identity: a locally administered MAC `02:` + 4 bytes derived from the receiver serial (FNV or CRC32) + 1 byte tracker id. Stable across restarts, so the server remembers assignments and body positions. The server shows a one-time 'new device, accept?' prompt per MAC; accept once.
- Per device, open its own UDP socket to `127.0.0.1:6969` (one source port per device; the server keys connections by source address). Emit:
  1. Handshake (packet 3) on first sight of a type-0 'info' packet, then resend if no server reply within 1 s. Fields: boardType 4 (CUSTOM), imuType 13 (LSM6DSV), mcuType 0, three zero ints, protocolVersion 22, firmware string e.g. `snrf-bridge 0.1`, the 6-byte MAC.
  2. SensorInfo (packet 15): sensorId 0, status 1, sensorType 13.
  3. RotationData (packet 17) per quaternion-bearing HID packet (types 1, 2, 7): sensorId 0, dataType 1, quaternion x,y,z,w as big-endian floats, calibrationInfo 0. Send the raw sensor-frame quaternion; the server applies its own axis offset on both the HID and the UDP path, so do not apply it in the translator.
  4. Acceleration (packet 4) alongside each rotation: x,y,z floats then sensorId byte. With protocolVersion >= 22 the server uses it as-is, matching the HID path.
  5. BatteryLevel (packet 12) on info packets: voltage float, level float (0..1; the server multiplies by 100).
  6. SignalStrength (packet 19): sensorId 0, then the signed dBm byte (negative of the HID rssi byte).
  7. Heartbeat (packet 0) every 500 ms per device when nothing else was sent; the server times a connection out after 1000 ms without any packet.
- Every datagram: 4-byte big-endian packet id, 8-byte big-endian packet number (monotonic per device, starting at 1), then the payload. The server rejects out-of-order numbers.
- Answer PingPong (packet 10) by echoing the datagram back unchanged, so the server's latency display works.
- Translator UI: console log with per-tracker rates and the Quest link state. Expose `--pc-port`, `--server-port`, `--log-level`.

### What this buys over Option A

- Trackers appear in the PC SlimeVR Server exactly like USB-connected SlimeNRF trackers: SteamVR driver, reset keybinds, OSC, VMC, all unchanged.
- The forwarder has no protocol knowledge; firmware packet changes touch only the PC program.
- The same forwarder later runs on any Android phone or a Pi (replace the Android USB code with hidapi), so the receiver can live on the hip without a cable to the head.

## Protocol reference

All values below were read from the firmware and server sources on 2026-10-08 (receiver v0.6.9, server v21.1.0 era `main`). Re-check against the exact versions in use before shipping.

### Receiver USB device

| Item | Value |
| --- | --- |
| VID / PID | 0x1209 / 0x7690 (tracker-on-USB uses PID 0x7692) |
| Interface | HID, vendor usage page, one interrupt-IN endpoint |
| Report | 64 bytes, no report id; holds up to 4 tracker packets of 16 bytes; unused slots padded |
| Report cadence | 1 ms timer; a report is sent when at least one packet is queued |
| RSSI | written into byte 15 of every packet except types 1 and 4; value is positive, server stores `-rssi` dBm |

### HID tracker packet (16 bytes, little-endian)

| Type (b0) | b1 | Payload | Decode |
| --- | --- | --- | --- |
| 0 info | id | b2 batt, b3 batt\_v, b4 temp, b5 brd\_id, b6 mcu\_id, b8 imu\_id, b9 mag\_id, b10-11 fw\_date u16, b12-14 fw major/minor/patch, b15 rssi | level = batt & 127 (128 = unknown); voltage = (batt\_v + 245) / 100 V; temp = temp/2 - 39 C; fw\_date: year = 2020 + (v >> 9 & 127), month = v >> 5 & 15, day = v & 31 |
| 1 quat + accel | id | b2-9 q0..q3 int16 Q15, b10-15 a0..a2 int16 Q7 | q = v / 32768; accel m/s^2 = v / 128 |
| 2 compact quat + accel + info | id | b2 batt, b3 batt\_v, b4 temp, b5-8 q\_buf u32, b9-14 a0..a2 int16 Q7, b15 rssi | exp-map quaternion, see below |
| 3 status | id | b2 server status, b3 raw status, b15 rssi | status maps to server TrackerStatus ids |
| 4 quat + mag | id | b2-9 q0..q3 int16 Q15, b10-15 m0..m2 int16 Q10 | mag mG = v \* 1000 / 1024 |
| 5 runtime | id | b2-9 u64 microseconds, b15 rssi | remaining runtime |
| 6 button + sleep | id | b2 button, b3-6 sleep ms, b15 rssi | button bits 0-1 = tap action |
| 7 compact quat + accel + button | id | b2 button, b3-6 sleep ms, b7-10 q\_buf u32, b11-14? see hid.c, b15 rssi | same q\_buf decode as type 2; check offsets in `hid.c` for the exact accel position |

Exp-map decode for q\_buf (types 2 and 7), matching `HIDCommon.kt`:

```
v0 = (q_buf & 1023) / 1024;  v1 = ((q_buf >> 10) & 2047) / 2048;  v2 = ((q_buf >> 21) & 2047) / 2048
vi = vi * 2 - 1 for each
d = v0^2 + v1^2 + v2^2;  inv = 1 / sqrt(d + 1e-6);  a = (pi/2) * d * inv
s = sin(a);  k = s * inv
q = (w = cos(a), x = k*v0, y = k*v1, z = k*v2)
```

Q15 quaternion order in type 1/4 is x, y, z, w (server builds `Quaternion(q3, q0, q1, q2)` as w, x, y, z). Padding packets have type 0xFF or id 0xFF.

### SlimeVR UDP tracker protocol (what the translator emits to 127.0.0.1:6969)

Every datagram: `int32 packetId` + `int64 packetNumber` (both big-endian), then payload. packetNumber is per source socket, strictly increasing; the server drops out-of-order packets. Quaternions are x, y, z, w floats; vectors x, y, z floats.

| Id | Name | Payload |
| --- | --- | --- |
| 0 | Heartbeat | none |
| 3 | Handshake | int32 boardType, int32 imuType, int32 mcuType, 3 x int32 (ignored), int32 protocolVersion, uint8 len, ASCII firmware string (len bytes), 6-byte MAC. Server replies with a 64-byte datagram; ignore its content |
| 4 | Acceleration | float x, y, z; uint8 sensorId |
| 10 | PingPong | int32 pingId; echo the whole datagram back |
| 12 | BatteryLevel | float voltage, float level (0..1; the server multiplies by 100) |
| 15 | SensorInfo | uint8 sensorId, uint8 status (1 = OK), uint8 sensorType |
| 17 | RotationData | uint8 sensorId, uint8 dataType (1 = normal), float x, y, z, w, uint8 calibrationInfo |
| 19 | SignalStrength | uint8 sensorId, int8 dBm |
| 20 | Temperature | uint8 sensorId, float C |

Enum ids: imuType LSM6DSV = 13, ICM45686 = 16; boardType CUSTOM = 4; mcuType UNKNOWN = 0. Use protocolVersion 22 or higher so acceleration is taken as-is. Server-side timeout: a device with no datagram for 1000 ms is marked timed out.

Sources: SlimeVR-Tracker-nRF-Receiver `src/hid.c`, `src/connection/esb.c`, `prj.conf`; SlimeVR-Server `tracking/trackers/hid/HIDCommon.kt`, `tracking/trackers/udp/UDPPacket.kt`, `UDPProtocolParser.kt`, `TrackersUDPServer.kt`, `FirmwareConstants.kt`.

## Hardware setup

- Receiver: the stock-antenna ProMicro tracker board flashed as receiver (or the Nordic dongle when it arrives). Do not use the board with the lifted antenna pads.
- Mount: hip, on the velcro belt, antenna wire vertical along the thigh, 3 cm or more from the power bank and any metal buckle.
- Cable: USB-C to USB-C (or C-to-A OTG), routed up the back under the shirt to the headset, with a slack loop at the neck. A PD-capable USB-C OTG hub at the headset lets a hip power bank charge the Quest on the same hub; verify the hub keeps the OTG data path alive while charging (many cheap ones do not).
- Headset network: 5 GHz only, PC on Ethernet. The dongle sits centimetres from the Quest's WiFi radio; a 2.4 GHz headset connection would jam its own receiver.
- Trackers: unchanged. Keep the antenna wires straight and away from battery foil; mount ankle trackers on the front of the shin.

## Test plan and acceptance criteria

1. Bench, Option A: Android server sees the receiver and all 6 trackers within 10 s of attach; VRChat shows 6 OSC trackers and calibrates. Pass if a 30-minute session on the body produces no tracker timeouts.
2. Bench, Option B: forwarder reports/s equals the receiver's packet rate (about 180/s per tracker divided by packets per report); translator shows 6 devices in the PC server with correct IMU type and battery; SteamVR shows 6 Vive trackers. Pass if rotation matches the direct-USB receiver within visual jitter.
3. Link quality: with the receiver on the hip, the receiver console's "Sequence missmatch" rate must be under 0.5 per tracker per second over 60 s in the crouch/sit test (desk receiver baseline: 2.7 per tracker per second).
4. Endurance: 2-hour VRChat session, no tracker entering connection error or deep sleep; forwarder service alive throughout (check its notification and counters).
5. Latency (Option B): PingPong round trip shown in the PC server under 15 ms over WiFi.
6. Failure handling: unplug the dongle and replug; forwarder reconnects without app restart; PC server marks devices timed out then OK.

## Open questions and risks

- [ ] Does the Android server APK build run on the Quest 3S (Android 12-based) without crashes, and does its WebView GUI render in the headset? Nothing is known about Quest-specific testing.
- [ ] Does Quest OS keep the foreground service alive while Steam Link is the active immersive app? The Android server relies on this too.
- [ ] Exact byte offsets of the accelerometer in HID packet type 7: confirm in `hid.c` before implementing (the table marks it).
- [ ] Which CI run to take the Android APK from, to stay protocol-compatible with the trackers' receiver build (receiver v0.6.9, 2026-10-05).
- [ ] USB OTG + PD charging on the Quest through a hub: needs a hub known to keep host mode under charge.
- [ ] Reset controls in Option A come only from the headset-side GUI or tap detection; Option B keeps PC keybinds. Decide which matters.
- [ ] Risk: the Quest's own 2.4 GHz WiFi if it falls back from 5 GHz; pin the headset to a 5 GHz-only SSID.
