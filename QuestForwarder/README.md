# Quest receiver forwarder — option B

Nordic receiver → Quest USB host → raw reports over Wi-Fi →
`tools/quest_bridge.py` → stock PC SlimeVR server → SteamVR.

## Status (2026-10-11 KST)

Implemented and built a signed debug APK. Ten Python tests cover decoding,
registration, sequence handling, restart/disconnect handling, and actual local UDP
handshake/motion/ping exchanges against a simulated server. The existing seven
link-monitor tests also pass. No Quest was connected for installation. Quest USB,
background survival during Steam Link, actual SlimeVR integration, body assignments,
end-to-end latency, charging, and endurance are **not hardware-validated**.

The last recorded trackers run radio-only diagnostic firmware. That firmware
does not send poses. This work does not change or flash tracker/receiver firmware.
Normal tracking firmware must be restored in a separately authorized deployment
before pose testing.

## Run

1. Start the PC SlimeVR server, then run from the repository root:

   ```powershell
   python tools/quest_bridge.py --quest-ip QUEST_IPV4
   ```

   Replace `QUEST_IPV4` with the headset's local IPv4 address. The bridge listens
   on UDP 6970 and sends to `127.0.0.1:6969`. Allow inbound UDP 6970 on the PC's
   private network if Windows Firewall blocks it. This is an unauthenticated LAN
   transport; do not expose the port to the Internet. `--quest-ip` filters senders.

2. Install `dist/quest-forwarder/QuestSlimeForwarder.apk` with Android platform tools:

   ```text
   adb install -r dist/quest-forwarder/QuestSlimeForwarder.apk
   ```

3. Open **Quest Slime Forwarder** from the headset's sideloaded apps. Enter the
   PC's DNS hostname or LAN IPv4 address and port `6970`, then select **Start forwarding**.
   The hostname must resolve to an IPv4 address from the Quest's network. Resolution
   runs in the network worker at Start; Stop/Start resolves it again. The UI displays
   the resolved address or the lookup error. Windows-only computer-name discovery
   is not implemented; use a router/local-DNS hostname or the IPv4 address.
4. Attach the Nordic `1209:7690` receiver through a USB-C OTG adapter and allow
   USB access. USB debugging normally uses the same port, so install first.
   Use a suitable powered OTG hub if simultaneous charging is needed.
5. Accept any new devices in SlimeVR. Check identity, orientation, battery and
   assignments before calibration. The bridge preserves the actual tracker
   address, reversed from HID wire order. Automatic migration of USB assignments
   is not guaranteed: the server may use different names for HID and UDP trackers.
6. Launch Steam Link. Watch PC output for each tracker's rotation rate and the
   Quest link state. Select **Stop** in the forwarder to release USB and the wake
   lock. Stop before changing destination settings.

The service polls for attachment and retries opening the receiver. Permission
denial requires Stop/Start to ask again. An Android permission grant lasts for the
attachment; do not assume replug is permission-free. A process killed by the OS
requires reopening the app and pressing Start. It intentionally does not promise
automatic recovery from process death or start at boot.

## Background behavior

The app uses an Android `connectedDevice` foreground service, an ongoing
notification, and a partial wake lock. USB acquisition and UDP sending have
separate threads. A 16-report queue discards old reports when congested, and reports
older than 100 ms are not sent. A 500 ms hello reports receiver connection state.

These mechanisms do not establish Quest reliability. Verify at least a two-hour
Steam Link/VRChat session, headset standby/wake, Wi-Fi interruption, and receiver
unplug/replug. When tracker records stop for one second, the PC bridge stops all
traffic for that tracker, including ping replies and sensor announcements, letting
SlimeVR time it out. Receiver address announcements and Quest heartbeats never
keep a missing tracker alive. No pose extrapolation or gap concealment is used.

SlimeVR's displayed ping measures **PC bridge ↔ PC server**, not the Quest Wi-Fi
hop or tracker-to-game latency. Do not use that number as an end-to-end test.

## Build

```powershell
./QuestForwarder/build.ps1
python -m unittest discover -s tools -p test_quest_bridge.py -v
```

The build script defaults to this PC's Unity 2022.3.22f1 Android SDK and OpenJDK.
Override `-AndroidSdk` and `-JavaHome` on another machine. It requires platform
android-32, build-tools 32.0.0 and a JDK. Output and a reusable development signing
key are kept under ignored `dist/quest-forwarder/`. Each build uses a fresh output
directory. This is a sideloaded debug build, minSdk 29 / targetSdk 32; moving to a
new target SDK requires reviewing its foreground-service and permission rules.
The app is plain Java with Android framework APIs and no third-party libraries.

## Wire format

This implementation uses versioned `SNR2` framing, deliberately replacing the old
draft's unversioned `SNRF` header so app restarts and USB reconnections have explicit
session identity. Both included programs must be deployed together.

| Offset | Field |
| --- | --- |
| 0 | ASCII `SNR2` |
| 4 | Kind: 1 = report, 2 = hello |
| 5 | USB connected: 0 or 1 |
| 6 | Payload length, uint16 little-endian |
| 8 | Random session, uint64 little-endian; changes on USB open/close |
| 16 | Sequence, uint32 little-endian; wraps; shared by hello and reports |
| 20 | Payload: exactly 64 raw HID bytes, or at most 128 UTF-8 serial bytes |

A hello establishes a session. Reports without its hello are ignored until the
next hello. Duplicates and backward sequences are rejected; gaps are counted.
Previously seen retired sessions are rejected using bounded history. A different
source endpoint can take over after three seconds of silence. Registration state
is cleared at a session boundary. The bridge requires both address and type-0 info
before opening a virtual device; each MAC has its own server socket.

Supported translation: type 0 info, type 1/2/4/7 quaternion, type 1/2/7 acceleration,
battery, temperature, valid RSSI. Type-7 offsets are 5 for quaternion and 9 for
acceleration. Quaternions stay in sensor coordinates; SlimeVR applies its axis
offset. Handshake advertises protocol 22 and actual board/MCU/IMU metadata.
Ping responses preserve the ping ID but use a new outgoing packet number.

Raw status, sleep duration, runtime, magnetometer vectors and button/tap actions
are not translated in this first version. PC-side reset controls still belong to
SlimeVR; tracker-button parity with direct HID is not claimed. USB OUT, pairing,
receiver console and OTA are not forwarded by this app.

## Protocol references checked

The local receiver and [protocol reference](../docs/PROTOCOL.md) establish HID
record offsets. The following official sources were inspected on 2026-10-11;
upstream `main` is mutable and the installed PC server was not inspected:

- [SlimeVR UDP packets](https://github.com/SlimeVR/SlimeVR-Server/blob/main/server/core/src/main/java/dev/slimevr/tracking/trackers/udp/UDPPacket.kt)
- [SlimeVR UDP parser](https://github.com/SlimeVR/SlimeVR-Server/blob/main/server/core/src/main/java/dev/slimevr/tracking/trackers/udp/UDPProtocolParser.kt)
- [SlimeVR UDP server](https://github.com/SlimeVR/SlimeVR-Server/blob/main/server/core/src/main/java/dev/slimevr/tracking/trackers/udp/TrackersUDPServer.kt)
- [SlimeVR HID handling](https://github.com/SlimeVR/SlimeVR-Server/blob/main/server/core/src/main/java/dev/slimevr/tracking/trackers/hid/HIDCommon.kt)
- [Android USB host](https://developer.android.com/develop/connectivity/usb/host)
- [Android UsbRequest](https://developer.android.com/reference/android/hardware/usb/UsbRequest)
- [Foreground service types](https://developer.android.com/develop/background-work/services/fgs/service-types)

`UsbRequest` handles the interrupt endpoint; the draft's proposed use of
`bulkTransfer` on that endpoint is not used.
