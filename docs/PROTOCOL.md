# Protocol reference

[Documentation index](README.md) · Checked against local source on 2026-10-10.

This describes this checkout, not a promise about future upstream versions.
For OTA message layouts, see [OTA](OTA.md). Historical UART/UDP hub formats remain
in the [original memory](archive/MEMORY-2026-10-10.md).

## Sources of truth

- [Tracker packet writers](../Tracker/src/connection/connection.c): actual field offsets.
- [Tracker ESB](../Tracker/src/connection/esb.c): radio configuration and framing.
- [Receiver HID](../Receiver/src/hid.c): host reports and address announcements.
- [Viewer decoder](../tools/slime_view.py): current Python decoding.
- [OTA header](../Tracker/src/ota_proto.h): shared with the receiver.

Some packet-description comments swap the descriptions of types 6 and 7. The
field writes establish that **type 6 has button/sleep only, type 7 also has motion**.

## Transport boundaries

The active radio uses ESB at 2 Mbps, with up to 32 payload bytes. Trackers transmit;
the receiver delivers OTA data through ACK payloads. Normal tracker traffic uses
pipe 1, OTA requests use pipe 7.

A normal sequenced radio frame is 21 bytes: the 16-byte tracker packet, a
little-endian CRC32 using `crc32_k_4_2_update(0x93a409eb, data, 16)`, and one
sequence byte. The receiver removes the radio framing before USB HID. Therefore
host HID gaps are not an exact count of lost radio frames.

Receiver USB identity is `1209:7690`. Its 64-byte HID IN report contains up to four
16-byte records. HID OUT carries the OTA host stream. Tracker USB identity
`1209:7692` is a different device path.

## Tracker records

Offsets are zero-based; multi-byte fields below are little-endian. Byte 0 is the
type and byte 1 the tracker ID. Unless noted, byte 15 holds receiver-supplied RSSI
magnitude; the host displays its negative as dBm.

| Type | Content |
| --- | --- |
| 0 | Battery b2; voltage b3; temperature b4; board b5; MCU b6; IMU b8; magnetometer b9; firmware date u16 at b10; version b12–14 |
| 1 | Quaternion x,y,z,w as four signed Q15 values at b2–9; acceleration as three signed Q7 values at b10–15; no RSSI |
| 2 | Battery/voltage/temperature b2–4; compact quaternion u32 at b5–8; acceleration Q7 at b9–14 |
| 3 | Server status b2; tracker status flags b3 |
| 4 | Quaternion Q15 at b2–9; magnetometer signed Q10 at b10–15; no RSSI |
| 5 | Remaining runtime signed int64 microseconds at b2–9 |
| 6 | Button b2; sleep time u16 milliseconds at b3–4 |
| 7 | Button b2; sleep time u16 at b3–4; compact quaternion u32 at b5–8; acceleration Q7 at b9–14 |
| 255 | Receiver address announcement: tracker ID b1, device address b2–7 in little-endian byte order |

Type 255 is an address announcement, **not generic padding**. Reverse b2–7 for the
displayed device address used by receiver `list` and saved viewer assignments.
An address announcement alone is not proof that a tracker is currently sending.
See the receiver and viewer implementations for report padding/valid-ID handling.

The archived Quest v1 draft's type-6/type-7 sleep width and type-7 motion offsets
are incorrect. Reuse the verified offsets above when implementing any bridge.

## Values and decoding

- Quaternion Q15: divide each signed value by 32768; wire order is x,y,z,w.
- Acceleration Q7: divide signed values by 128.
- Magnetometer Q10: divide signed values by 1024 to get gauss.
- Voltage: `(b3 + 245) / 100` volts.
- Battery: 128 means unavailable; 255 means charged. The current viewer otherwise
  uses `min(100, b2 & 127)`.
- Temperature: zero means unavailable. The tracker encodes
  `((temperature - 25) * 2 + 128.5)` into an integer byte, with saturation;
  the current viewer displays `b4 / 2 - 39`. Quantization matters when comparing
  those expressions; do not treat the byte as an exact physical temperature.
- Firmware date: year `2020 + ((v >> 9) & 127)`, month `(v >> 5) & 15`,
  day `v & 31`.
- A reported worst RSSI of -127 was identified as a missing/invalid sample in the
  historical diagnostics; do not interpret it as a measured usable link margin.

Compact quaternion decoding, as used by the viewer for types 2 and 7:

```text
bits = little_endian_u32(packet[5:9])
v = ((bits & 1023) / 512 - 1,
     ((bits >> 10) & 2047) / 1024 - 1,
     ((bits >> 21) & 2047) / 1024 - 1)
d = dot(v, v)
inv = 1 / sqrt(d + 1e-6)
angle = pi / 2 * d * inv
(x, y, z) = sin(angle) * inv * v
w = cos(angle)
```

The viewer validates the quaternion norm and normalizes accepted rotations.

## External bridges

The current repository does not implement a Quest HID forwarder or an ESP32 USB
host. The old specs' Android behavior, SlimeVR server enums, UDP handshake rules,
OSC limits and platform versions were not revalidated during this cleanup.
Check the exact server/platform sources before implementing a bridge; do not
treat those archived assumptions as a tested interface.

Preserve device-address identity rather than inventing a receiver/ID-derived MAC
if assignments must match the direct USB path. See the
[receiver placement proposal](RECEIVER_PLACEMENT.md).
