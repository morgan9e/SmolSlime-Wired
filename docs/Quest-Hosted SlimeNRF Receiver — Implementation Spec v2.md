# Quest-Hosted SlimeNRF Receiver — Implementation Spec v2

Oct 10, 2026 · @user · revises v1 (Oct 8, same folder)

## What changed since v1

| v1 assumption | What we know now (measured Oct 9–10) | Consequence |
| --- | --- | --- |
| The desk receiver is the common factor in the dropouts | It was the **ProMicro receiver's RF**: about -70 dBm at 30 cm. The **Nordic nRF52840 Dongle** on the desk receives every tracker at -44 to -66 dBm from 0.3 to 3 m, noise floor -90 dBm (25–45 dB of margin) | The main reason to move the receiver is gone. Moving it now has to be justified by a measurement (Step 0) |
| Moving the receiver closer fixes the loss | The remaining loss is **collisions between trackers**: one tracker moving alone loses 7.5%, all six moving lose 40–65% (air busy 44%, bad CRC 33%, loss vs bad CRC correlation 0.92), at the same signal strength | Receiver placement does not fix it. The tracker firmware does: send rate cut (10 ms loop, 120 Hz effective with the gyro at 240 Hz) and a per-tracker retry delay, on branch `clean`; TDMA later if needed |
| Ankles wired to thighs (On-Body Hub plan v3) | **Full wireless**: every tracker on its own radio | The UART mirror, wired ankles and the ProMicro receiver are out. On-Body Hub plan v3 is superseded |
| Receiver: ProMicro with a wire antenna, on the hip | Receiver: the **Nordic dongle** (PCB antenna, USB-A plug). Stock receiver firmware, or the stock + diagnostics build (`patches/receiver-stock`, unchanged HID output) | It plugs straight into a USB-C OTG adapter; it can ride on the headset strap with no cable down the back |
| Only the Quest can host the receiver on the body | The ESP32 hub firmware (`src/Hub` on `main`) already presents receiver packets to the PC SlimeVR Server over Wi-Fi, tested end to end with 6 trackers on Oct 10; an **ESP32-S3** can be a USB host | New **Option C**: dongle into an ESP32-S3 on the hip, no Android app and no PC program |
| ~180 packets/s per tracker | ~120/s per tracker while moving with the `clean` tracker build (~20–30/s still) | Lower forwarder load: 6 trackers ≈ 720 packets/s, 180–720 reports/s |

## Context and goal

Trackers: 6 stacked ProMicro nRF52840 + LSM6DSV, 180 mAh cells, running the `clean` branch tracker
(stock `cbfff16` + send-rate cut + per-tracker retry delay). Receiver: Nordic nRF52840 Dongle
(VID 0x1209 / PID 0x7690, 64-byte HID reports of up to four 16-byte tracker packets; unchanged from v1).
Quest 3S streams PC VR over Steam Link; VRChat on PC; PC on Ethernet, headset on 5 GHz.

Goal: no tracker dropouts you can see during room-scale play, including back turned to the PC and
crouching. Move the receiver onto the body **only if Step 0 shows the desk dongle cannot do that**.

## Step 0 (new): measure before building anything

Two evenings, no new code. Tools: `tools/slime_link.py` (rate, RSSI, gaps per tracker over USB-HID,
runs beside SlimeVR Server) and the diagnostics receiver (`diag on`: frames lost by sequence
number, bad CRCs, noise floor).

1. **Trackers on the `clean` build first**, so collisions are not mistaken for range problems.
   Check: all six moving at 1 m, loss well below the 40–65% of stock.
2. **Desk dongle, real session.** 20 minutes of VRChat with the usual moves, plus 2 minutes each of
   back turned to the PC at 3 m, crouching, lying down. Log `slime_link --csv` and `diag on`.
3. **Dongle on the body, still on the PC.** Same session with the dongle on a 3–5 m USB extension
   cable, (a) on the headset strap, (b) on the hip. Same logs.

Decision:

| Desk dongle result | Next |
| --- | --- |
| No gap > 250 ms outside collisions, worst RSSI above -80 dBm in every pose | **Stop.** Keep the dongle on the desk (a USB extension to raise it helps more than any rework) |
| Gaps or RSSI below -80 dBm only in some poses (body between tracker and PC) | Move the receiver: choose the position from step 3, then Option A, B or C |
| Loss stays high on the body too | It is not placement: back to the tracker side (TDMA) |

## Option A: stock SlimeVR Android server on the Quest (no code)

Unchanged from v1, with the dongle in place of the ProMicro receiver: dongle in a USB-C OTG adapter
on the Quest, `SlimeVR-android.apk` sideloaded, OSC trackers to VRChat on the PC. One evening to try.

Limits, unchanged: VRChat only (OSC, not SteamVR), resets from the headset GUI or taps, no battery
level on the PC.

## Option B: Quest HID-to-UDP forwarder + PC translator (custom)

Design unchanged from v1 (B1 forwarder in Kotlin, raw 64-byte reports over UDP 6970 with an 8-byte
`SNRF` + sequence header; B2 translator feeding the PC server over its UDP tracker protocol).
Changes:

- **Load:** about 180–720 reports/s with the `clean` tracker build, not 300–1100.
- **Translator decode:** reuse the HID packet decoding in `tools/slime_view.py` (`decode_hid_packet`,
  checked against the dongle on Oct 10, address byte order included: the HID packet carries the
  address little-endian; the MAC the server sees must be the reversed, most-significant-first form
  so body assignments match the ones made over USB). `tools/slime_link.py`'s `Monitor` gives the
  per-tracker gap statistics for the console view.
- **Previous code:** the Oct 7 session wrote a forwarder and translator in
  `Downloads\QuestSlimeProxy`; that folder no longer exists. The code is recoverable from that
  session's transcript if wanted, but it predates the address byte-order finding.

## Option C (new): ESP32-S3 USB host on the hip + existing hub firmware

The dongle plugs into an ESP32-S3; the hub firmware reads its HID reports instead of the old UART
mirror and presents every tracker to the PC SlimeVR Server as a Wi-Fi tracker, as it already did
in the Oct 10 test. Trackers appear in SteamVR for every game, PC keybinds and OSC keep working,
and nothing runs on the Quest or as an extra PC program.

- **Firmware:** one new input module in `src/Hub`: ESP-IDF USB host + HID host class driver,
  claim VID 0x1209 / PID 0x7690, read the interrupt-IN endpoint, split each 64-byte report into
  16-byte packets and pass them to the existing decoder (the same packets the UART mirror carried,
  without the COBS/CRC16 framing). Everything after the decoder (SlimeVR UDP, server discovery, gap
  hold, battery, RSSI) is reused. Build target: `make hub-s3`.
- **Board:** XIAO ESP32-S3 (in hand). The C6 has no USB host and cannot do this.
- **Power:** in host mode the S3 does not power the dongle by itself; VBUS needs 5 V. Use a power
  bank with a USB-C splitter, or a small boost converter from the hub's cell onto the VBUS line.
- **Radio:** the S3's Wi-Fi is 2.4 GHz only and sits next to the dongle. On Oct 10 the hub's Wi-Fi
  was ruled out as a cause of loss; keep the router on a channel far from ESB channel 2 (2402 MHz),
  e.g. Wi-Fi channel 11, and keep the S3 antenna away from the dongle.
- **Diagnostics:** with the diagnostics receiver build, `diag on` needs the dongle's console; on the
  hub it is not reachable. Use `slime_link`-style gap statistics on the PC side instead, or
  bring `diag hub` from `main`'s receiver fork over if needed.

### Choosing between A, B and C

| | A | B | C |
| --- | --- | --- | --- |
| New code | none | Android app + PC program | one ESP32 module |
| Trackers in SteamVR (all games) | no, VRChat OSC only | yes | yes |
| Needs the Quest | yes | yes | no |
| Extra hardware on the body | OTG adapter | OTG adapter | ESP32-S3, 5 V source |
| Cable to the head | dongle on the headset: none | same | none |
| Risk | Android server on Quest untested | Quest kills background service | 5 V VBUS, 2.4 GHz next to the dongle |

Recommended order if Step 0 says move: **A** for a one-evening VRChat test; then **C** if trackers
are needed in SteamVR (it reuses tested firmware and needs no software on the headset); **B** only
if the dongle must ride on the headset and SteamVR trackers are needed.

## Hardware setup

- Receiver: the Nordic dongle only. The ProMicro receiver board is retired.
- Headset position (A, B): dongle in a short right-angle USB-C OTG adapter on the back strap, PCB
  antenna pointing up or back, away from the battery. Headset on 5 GHz only.
- Hip position (C, or A/B with a cable): dongle upright on the belt, 3 cm or more from the power
  bank and metal buckles.
- Trackers: unchanged mounting. Antenna wires straight, away from the battery foil. The two boards
  without an antenna (ids 0 and 5 in the Oct 10 tests, -60 to -66 dBm) are the first to watch.

## Test plan and acceptance criteria

1. **Step 0 logs** decide whether to go on at all (see the table above).
2. **Link quality with the chosen placement,** all six moving, 60 s, diagnostics receiver on a USB
   extension: loss under 5% per tracker, no silence over 250 ms outside collisions, worst RSSI above
   -80 dBm. (Replaces v1's "Sequence missmatch" count, which the stock console logs and which also
   counts tracker-side repeats.)
3. **Option A:** VRChat shows 6 OSC trackers and calibrates; 30 minutes without a timeout.
4. **Option B / C:** 6 trackers in the PC server with the right IMU type, battery and body
   assignments (same MACs as over USB); SteamVR shows 6 trackers; rotation matches the direct-USB
   dongle within visual jitter; PingPong under 15 ms.
5. **Endurance:** 2-hour session, no tracker in connection error or deep sleep; forwarder service
   (B) or hub (C) alive throughout.
6. **Replug:** unplug and replug the dongle; the forwarder or hub picks it up again without a
   restart, the server marks trackers timed out, then OK.

## Open questions and risks

- [ ] Step 0: is moving the receiver needed at all with the dongle and the `clean` trackers?
- [ ] Android server APK on the Quest 3S (A), and whether Quest OS keeps the foreground service alive under Steam Link (A, B). Unchanged from v1.
- [ ] ESP32-S3 USB host with a vendor-usage-page HID device (C): ESP-IDF's HID host driver should claim it as a generic interface; confirm on the bench first.
- [ ] 5 V VBUS for the dongle in Option C, from the same cell or a power bank.
- [ ] Type 7 packet accelerometer offsets: `tools/slime_view.py` and `src/Hub/main/decoder.c` both decode type 7; check them against the receiver's `hid.c` before reusing either.
- [ ] Remaining collision loss after the `clean` tracker build: if still visible, TDMA (unfinished upstream: receiver `tdma.h`, sync timer never started).
