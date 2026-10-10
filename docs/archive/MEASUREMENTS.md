# Measurements through 2026-10-10

[Documentation index](../README.md) · [Original notes](MEMORY-2026-10-10.md)

Historical observations, not a fresh hardware test. Firmware versions, power source,
placement and motion matter when comparing these results. No all-six-moving result
for 0.0.3 or later was recorded in the source notes.


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
