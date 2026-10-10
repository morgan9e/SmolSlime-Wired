# Plan: tracker firmware updates over the radio (OTA)

Status: phase 1 in progress (MCUboot build: `make tracker-ota`). Decided: MCUboot with rollback,
unsigned images (MCUboot still checks the SHA-256), the tracker pauses tracking during an update. Goal: after one last USB flash, update every tracker from
the PC through the nRF52840 Dongle, one tracker at a time, while the others keep tracking.

## Facts this plan rests on

| | ProMicro (stacked SPI) | XIAO nRF52840 Sense | Dongle (receiver) |
|---|---|---|---|
| Bootloader | Adafruit UF2 at 0xF4000 | Adafruit UF2 0.6.1 at 0xF4000 | Nordic open bootloader |
| Application | 0x1000 - 0xEE000 (948 KB) | 0x27000 - 0xEE000 (796 KB) | 0x1000 - |
| Settings (NVS: pairing, calibration) | 0xEE000 - 0xF4000 | 0xEE000 - 0xF4000 | |
| Image today | 235 KB | 219 KB | 112 KB, 64 KB RAM of 256 |

- ESB at 2 Mbit/s: one 21-byte frame with its ACK takes about 420 us, so one link moves at
  most about 2,400 frames/s; payloads (and ACK payloads) are limited to 32 bytes
  (`CONFIG_ESB_MAX_PAYLOAD_LENGTH`, both sides).
- The receiver is the ESB PRX: it never transmits on its own, it can only attach data to the ACK
  of a frame a tracker sent (ACK payload, queued per pipe).
- All trackers send on pipe 1, so an ACK payload queued on pipe 1 goes to whichever tracker
  sends next. The tracker already reads ACK payloads: 8 bytes while pairing, a 4-byte LED sync
  (unused). Neither collides with a new message type.
- The dongle's USB has the HID IN endpoint (tracker data for the server), a HID OUT endpoint
  (only logged today) and the CDC console.
- `slime_link.py` already shows each tracker's firmware version over the air.

## Design

### 1. Tracker flash layout: MCUboot between the UF2 bootloader and the app

MCUboot goes where the app starts today; the app moves into two slots after it. The UF2
bootloader and the settings stay where they are, so pairing and calibration survive.

ProMicro (`Tracker/pm_static/mcuboot_nrf52840_uf2.yml`, built: MCUboot 24 KB, app 242 KB):

| Region | Range | Size |
|---|---|---|
| MBR | 0x0 - 0x1000 | 4 KB |
| MCUboot | 0x1000 - 0xD000 | 48 KB |
| Slot 0 (running app) | 0xD000 - 0x7C000 | 444 KB |
| Slot 1 (incoming image) | 0x7C000 - 0xEA000 | 440 KB |
| (unused) | 0xEA000 - 0xEE000 | 16 KB |
| NVS settings | 0xEE000 - 0xF4000 | 24 KB, unchanged |
| UF2 bootloader | 0xF4000 - 0x100000 | unchanged |

swap-using-move needs slot 0 at least one page larger than slot 1. The UF2 bootloader (nice!nano
0.11.0) writes only 0x1000 - 0xEA000 (the 40 KB below it are kept for app data; its CURRENT.UF2
shows the range), so slot 1 ends there: a test image for it can be flashed over USB as well
(`make tracker-ota-slot1`).

XIAO: the same from 0x27000 (MCUboot 0x27000 - 0x33000, two slots of 374 KB each).

- Fixed with a `pm_static.yml` per board (the build uses the Partition Manager), MCUboot via
  sysbuild (`SB_CONFIG_BOOTLOADER_MCUBOOT`), swap with rollback (`swap-using-move`): power-fail
  safe, and an image that does not confirm itself is swapped back on the next boot.
- Images signed with a project key (imgtool, ECDSA P-256). The private key stays out of git
  (path in an environment variable); without it no image is accepted.
- `make tracker` then also writes a merged UF2 (MCUboot + app) for USB flashing and recovery: the
  UF2 bootloader stays, so double-tap reset and a USB flash always works, even if MCUboot or an
  image is broken.

### 2. PC to dongle: store the whole image on the dongle first

The image does not fit in the dongle's RAM next to the firmware (235 KB, about 190 KB free), but
its flash is mostly empty. Uploading first decouples USB from the radio, and one upload can update
several trackers.

- Dongle flash area "OTA store" (256 KB) after its application.
- Over HID OUT (64-byte reports, binary): `BEGIN` (size, SHA-256, board id, version), `DATA`
  (offset + up to 56 bytes), `END`. The dongle writes, then checks the SHA-256 of the store.
- Status back over the CDC console as text lines (`ota: stored 0.0.4 for promicro, 235436 bytes`),
  so the HID IN path to SlimeVR Server stays exactly as it is.

### 3. Dongle to tracker: the tracker fetches chunks through ACK payloads

1. **Invite:** `ota <id>` on the dongle console (or from the PC tool). The dongle attaches an
   `OTA_OFFER` (tracker id, board id, version, size, hash prefix) to ACKs on pipe 1 until the
   addressed tracker answers; other trackers ignore offers for another id.
2. **Check:** the tracker accepts only its own board id and a version different from its own,
   and only while it is not in the middle of something else (calibration, pairing).
3. **Own pipe:** the accepting tracker switches its data frames to pipe 7 for the transfer, so
   the ACK payloads queued on pipe 7 reach only it. One update at a time.
4. **Fetch:** the tracker sends `OTA_REQ(offset)`; the ACK carries the chunk for that offset
   (27 data bytes per 32-byte payload). The dongle queues the next chunk ahead, so a clean link
   moves one chunk per transaction; a lost chunk is simply requested again (the tracker owns the
   offset, so retries and resume are trivial).
5. **Write:** the tracker erases each slot 1 page just before writing into it (a full erase up
   front would stall it for about 10 s) and keeps sending a low-rate orientation frame so it
   does not show as lost.
6. **Install:** after the last chunk the tracker checks the SHA-256 of slot 1, marks it for a test
   swap and reboots. MCUboot checks the signature and swaps.
7. **Confirm:** the new app confirms itself once the IMU is found and the receiver has ACKed its
   frames for a few seconds. Otherwise the next reboot swaps the old image back.

Expected time: 235 KB in 27-byte chunks is about 8,700 transactions, about 4 s at the link's
limit; with the other trackers on the channel, retries and flash writes, 20-60 s per tracker.
Raising the payload limit to 252 bytes on both sides would cut the transaction count about 9x
(phase 5, only if the time matters).

### 4. PC tool: `tools/ota.py`

    python tools/ota.py dist/SlimeNRF_ProMicro_StackedSPI_Tracker.signed.bin --tracker 3
    python tools/ota.py <image> --board promicro --all     # every tracker on that board

Uploads to the dongle (or skips it when the same image is stored), starts the update, shows
progress from the console, and checks the new version over the air (slime_link's decoder).

## Failure cases

| What happens | Result |
|---|---|
| Radio drops, tracker out of range, battery off during the transfer | Slot 1 incomplete, slot 0 untouched: the tracker keeps running the old version; retry resumes |
| Image for the other board, or corrupted | Rejected at the offer (board id) or after the transfer (SHA-256, then MCUboot's signature) |
| Power lost during the swap | `swap-using-move` resumes the swap on the next boot |
| New image crashes or cannot reach the receiver | Not confirmed: MCUboot swaps back on the next boot (the crash reset is that reboot) |
| MCUboot or both slots broken | UF2 bootloader: double-tap reset, flash the merged UF2 over USB |
| Settings | NVS area unchanged by every step: pairing and calibration survive |

## Phases (each one tested before the next)

1. **MCUboot on one ProMicro**, over USB (merged UF2): boots, tracks, pairing kept, a test swap
   and a rollback work (images put into slot 1 over USB with the console for now).
2. **Dongle OTA store:** upload over HID OUT, SHA-256 check, survives a reboot.
3. **Radio transfer to that one tracker**, then a rollback test (an image that never confirms).
4. **`tools/ota.py`**, then the XIAO layout.
5. Roll out: one last USB flash of the merged UF2 on every tracker; from then on, OTA. Optional:
   252-byte payloads.

## Costs and open questions

- Diverges further from upstream: MCUboot configuration and two `pm_static.yml` in `Tracker`, an
  OTA module of a few hundred lines in each firmware, and the PC tool. Upstream merges
  (`git subtree pull`) touch other files, so they should stay easy.
- Every tracker needs one more USB flash (the merged UF2). Afterwards a USB flash must always use
  the merged UF2, never the bare app image (that would overwrite MCUboot; recoverable by flashing
  the merged one).
- To check in phase 1: that the Adafruit bootloader starts MCUboot at 0x1000 like any app (it
  checks only its own "app valid" flag, not a checksum of the app, as far as known), and the
  real MCUboot size.
- Decisions for you:
  - MCUboot with rollback (recommended) or a simpler copy-in-place without it (smaller, no
    rollback; a failed copy needs a USB flash).
  - Signing key: a project key outside git (recommended), or unsigned images (any image that
    passes the hash would be installed).
  - Keep tracking (slowly) during an update, as planned, or pause the tracker completely
    (simpler, about 20% faster).
