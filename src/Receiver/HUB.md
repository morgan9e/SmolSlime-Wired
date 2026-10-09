# Receiver for the ESP32 hub (branch `hub-uart-mirror`)

This branch adds a `stacked` variant of the ProMicro receiver for the on-body hub.
USB HID is unchanged; additionally every tracker packet is mirrored out of a UART to the
ESP32 hub (`src/Hub` in the `SlimeVR-Hub` folder), which forwards the trackers to SlimeVR over Wi-Fi.

Build target: `promicro_uf2/nrf52840/stacked` (stacked ProMicro tracker build used as receiver).

## Wiring

| ProMicro | Hub | Notes |
| --- | --- | --- |
| P0.08 (D0) | XIAO D7 (RX) | UART TX, 1 Mbaud, 3.3 V; the "RX" silkscreen on D0 does not matter |
| GND | GND | |
| B+ / RAW | hub cell + | do not plug USB in while it runs from the cell |
| P0.06 | button to GND | pairing button, internal pull-up |

The IMU, SPI and IMU power pins of the stacked build are not driven.

P0.08 is used because it is a full-speed pin. Nordic marks P0.29 and most other header pins
"low frequency I/O only" and advises against running a UART on them while the radio is
active. Check with a meter that D0 is not bridged to the stacked IMU board.

## Console switches

Two runtime switches on the receiver's USB console, both on again after a reboot:

| Command | Effect |
| --- | --- |
| `hid off` / `hid on` | stops / resumes tracker data over USB HID, so the SlimeVR Server on the PC does not see the trackers twice (USB and hub); the console, logs and hub keep working |
| `mirror off` / `mirror on` | pauses / resumes the UART feed to the hub (tracker and address packets); USB HID is unaffected |

`hid` or `mirror` alone prints the current state.

`diag on` / `diag off` (off at boot) replaces the per tracker orientation lines with link
diagnostics, once a second:

```
diag noise: median -94 dBm, quietest 10% -97, loudest 10% -72, 6.1% of samples above -80 dBm (990 samples)
diag radio: 1180 frames with a good CRC, 35 with a bad CRC (2.9%)
diag id 2: rx 186, lost 4 (2.1%), dup 3, late 0, max gap 21 ms, RSSI -68 (worst -79) |#########..x########|
diag gap: id 2 silent 180 ms, 34 frames lost, RSSI -71 before, -83 after
```

- `noise`: the radio's RSSI sampled 1000 times a second while it listens. Packets fill only a
  small part of the time, so the median is the noise floor, about -95 dBm on a clean channel.
  Every dB above that comes off the range (a USB 3 port or Wi-Fi next to the receiver, for example).
- `radio`: CRC results of everything the radio picked up, all trackers together, counted in
  hardware (RADIO CRCOK/CRCERROR through PPI into TIMER3/TIMER4).
- `lost`: the tracker's sequence numbers that never arrived. A tracker that just sends less
  (still, low rate) shows gaps but no loss. For a wired ankle this includes cable and relay drops.
- `dup`: the frame arrived but the tracker missed the ACK and sent it again.
- Slots are 50 ms: `#` frames arrived, `x` frames arrived after lost ones, `.` nothing arrived.
  Dots in the same columns on every tracker mean the receiver heard nothing at all; dots on
  one tracker only are that tracker's link.
- `gap` lines name each silence of 50 ms or more that lost frames, `outage` each one over 1 s.

### Diagnostics through the hub

`diag hub on` (stored; `diag hub off` stops it) sends the same statistics to the hub once a
second, so they can be watched on the body: run `python slime_diag.py` on the PC the hub
streams to. The hub forwards packet types 224-254 unchanged to UDP 6971 at the server's
address and never decodes them. Same framing as the tracker packets, little-endian:

```
type 253, receiver:  [1] flags: bit 0 HF crystal running, bit 1 1 Mbps, bit 2 CRC counters valid
                     [2-3] good CRC  [4-5] bad CRC  [6] noise median  [7] quietest 10%
                     [8] loudest 10% (RSSI magnitudes)  [9] % of noise samples above -80 dBm
                     [10] stored trackers  [12-13] report number  [14-15] hub UART packets dropped
type 254, tracker:   [1] id  [2-3] frames received  [4-5] lost  [6] repeats  [7] late
                     [8-9] longest silence, ms  [10] RSSI average  [11] RSSI worst (magnitudes)
                     [12-13] report number  [14] silences of 50 ms+ that lost frames  [15] outages of 1 s+
```

Collecting the statistics samples the radio's RSSI 1000 times a second, which very rarely
spoils the RSSI byte of a tracker packet (it reads -127).

## Radio bit rate

`radio 1m` / `radio 2m` stores the ESB bit rate and reboots (default 2 Mbps; `info` shows it).
1 Mbps reaches about 3 dB further but doubles the airtime per frame. Every tracker must use the
same rate: `write_config radio_1mbps 1` (or `0`) and `reboot` on its console, with a tracker
firmware from `hub-wired` that has the setting. Trackers on the other rate get no ACKs at all,
pairing included.

## Pairing

Press the button once to enter pairing mode (LED blinks as with the `pair` command), and
again to leave it (`exit`). The console commands still work over USB.

Pairing replies are now queued before the new tracker is written to flash, so a new tracker
pairs on its first request instead of its second (about one second sooner). This applies to
every board built from this branch.

## Clear and re-pair

`clear` also moves the receiver to a new radio address (a pairing generation stored in NVS;
generation 0 is the stock address, so flashing this firmware changes nothing until the first
clear). Trackers paired before can no longer reach it, so they cannot keep sending with ids the
receiver has since handed to someone else. Trackers built from `hub-wired` notice the missing
ACKs and re-pair by themselves while the receiver is in pairing mode (it is, right after `clear`);
a thigh then re-pairs its ankle too. Run `exit` once `list` shows every tracker.

## Wire format

```
COBS( packet[16] + crc16_le[2] ) + 0x00         1,000,000 baud 8N1, TX only
crc16 = crc16_itu_t(0xFFFF, packet, 16)          CRC-16/CCITT-FALSE, check value 0x29B1
```

Each batch of frames starts with one extra 0x00 so the hub resynchronises immediately.
Packets are the tracker packets passed to `hid_write_packet_n()` (types 0-7, byte 15 = RSSI
except types 1 and 4), sent before the HID FIFO merges reports, plus a type 255 address
packet (`packet_device_addr()` layout) per stored tracker every 100 ms.
The log shows `mirror: N sent, M dropped` every 10 s, and once a second per tracker
`Tracker 2 (3CA1B2C4D5E6): orientation X 12.3 Y -4.5 Z 170.2 deg, 98 pkt/s`
(`CONFIG_PACKET_LOG`, interval `CONFIG_PACKET_LOG_INTERVAL_MS`). The same log warns about packets
the receiver throws away although the tracker got its radio ACK (`Tracker 1: 35 pkt/s ignored,
id 1 is not stored on this receiver`, bad checksum, out of sequence), and every 10 s lists stored
trackers nothing arrived from (`No packets in 10 s from: ...`).

## Build

```
west build --board promicro_uf2/nrf52840/stacked --pristine=always <this repo> \
    -- -DNCS_TOOLCHAIN_VERSION=NONE -DBOARD_ROOT=<this repo>
```

Flash `build/SlimeVR-Tracker-nRF-Receiver/zephyr/zephyr.uf2` by double-tapping reset and
copying it to the UF2 drive. The flash layout (`pm_static_promicro_uf2_nrf52840_stacked.yml`)
is identical to the stock ProMicro receiver, so stored trackers survive switching between
the stock and the hub firmware.

## Changed files

- `boards/nordic/promicro_uf2/`: `stacked` variant (board.yml, dts, defconfig, yaml)
- `pm_static_promicro_uf2_nrf52840_stacked.yml`: copy of the stock layout
- `Kconfig`: `HUB_UART_MIRROR`, `PAIRING_BUTTON`
- `src/mirror.c`, `src/mirror.h`: UART mirror thread
- `src/button.c`: pairing button
- `src/hid.c`: one call each to `mirror_put()` and `packet_log_put()`
- `src/packet_log.c`, `src/packet_log.h`: per tracker orientation log (`PACKET_LOG`)
- `src/connection/esb.c`, `esb.h`: pairing reply before NVS write, `esb_pairing_active()`
