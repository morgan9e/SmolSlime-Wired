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
