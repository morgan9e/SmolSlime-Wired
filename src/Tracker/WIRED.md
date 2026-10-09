# Wired link

Trackers can pass their data over short UART cables instead of their own radio. Each board
has two headers, and the `mode` setting makes each one an input, an output or unused. A
tracker with an output keeps its radio off and sends everything upstream on the wire; a
tracker without one relays its inputs over ESB, together with its own frames. The receiver,
the ESP32 hub and SlimeVR see every wired tracker as a normal tracker with its own id.

The intended setup is **hub mode**: ankle → thigh on each leg by wire, both thighs → hip by
wire, and the hip takes the nRF receiver's place. It receives the chest over ESB and sends all
six trackers to the ESP32 hub on a wire, in the receiver's UART mirror format, so the ESP32 hub
firmware does not change and no separate receiver is needed.

```
L ankle ─wire→ L thigh ─wire→ ┐
                              ├→ hip (mode hub) ─wire, D0→ ESP32 ─Wi-Fi→ SlimeVR
R ankle ─wire→ R thigh ─wire→ ┘        ↑
                             chest ─ESB┘
```

The other way, `mode wired in both`, keeps the receiver: the hip relays the legs over ESB to it.

## Firmware and modes

One image for every tracker: `promicro_uf2/nrf52840/stackedspiwired`
(`dist/SlimeNRF_ProMicro_StackedSPI_Wired_Tracker.uf2`). Set the role on each tracker's USB
console; it is stored and the tracker reboots:

| Command | Radio | Used for |
| --- | --- | --- |
| `mode wireless` | on | chest, or any tracker on its own |
| `mode wired out right` | off | ankle (or an ankle still on the older firmware: it sends on D8) |
| `mode wired in left out right` | off | thigh: ankle in on the left, upstream on the right |
| `mode wired in left` | on | a thigh relaying its ankle by radio (the older setup) |
| `mode wired in both` | on | hip: both legs in, everything relayed by radio to the receiver |
| `mode hub` | receives | hip in place of the receiver: both legs in, the chest by radio, all to the ESP32 on D0 |

Any combination works (`in right out left`, `out left`, ...): at most one output, and a
header cannot be both. `mode` alone prints the current mode, `wired` the full status. The
setting is `wired_mode` (bits: 1 in left, 2 in right, 4 out left, 8 out right); `-1`, the
default, follows the older `connection_over_uart` setting: on = `in left out right`, off =
`in left`. The flash layout is the same as `stackedspi`, so calibration and pairing survive.

## Headers and cables

| Header | Data | Ground reference | UART |
| --- | --- | --- | --- |
| Left | D8 (P1.04) | D9 (P1.06) | uart1 |
| Right | D16 (P0.10) | D10 (P0.09) | uart0 |

A cable joins the two data pins and the two ground pins, so any straight cable works between
any two headers: D8/D16 to D8/D16, D9/D10 to D9/D10. 460800 baud 8N1, upstream only. The left
header is the same as in the older wired ankle firmware (data D8, ground D9, 460800 baud), so
ankles still running it plug into a thigh's left header as before.

- Both trackers keep their own battery. The ground pins are driven low on both headers from
  boot; they carry only the UART's return current, so a GPIO is enough. Do not share power.
- A 470R-1k resistor in series in the data line makes a misconfiguration harmless (two outputs
  on one cable would otherwise drive the line against each other).
- D10 and D16 are the nRF52840's NFC pins, used as GPIOs. The first boot after flashing writes
  this to the UICR and restarts once.
- All these pins are "low frequency I/O only" on the nRF52840 (D0-D7 and D14 are taken on the
  SmolSlime stack). Nordic advises against UART traffic on them while the radio runs. Wired
  trackers have the radio off; the hip sits a few cm from the receiver, where it does not matter.
- Each header's UART receives by default; for an output header the firmware swaps in transmit
  pins (`pinctrl_update_states()`) before starting it.

## Hub mode

`mode hub` on the hip. Both headers are inputs and stay on, the radio listens like the receiver
(same discovery address, pairing exchange and frame checks, on pipe addresses derived from the
hip's own device address), and the left header's UART also sends on **D0 (P0.08)**, a full-speed pin (the radio is listening at the same time). Wire D0 to
the ESP32's RX pin (XIAO D7) and the hip's GND to the ESP32's GND. The output shares the left
header's UART and so runs at 460800 baud: build the ESP32 hub with `CONFIG_HUB_UART_BAUD=460800`
(the nRF receiver uses 1,000,000, so switch the ESP32 together with the hip). The output is the receiver's
mirror format byte for byte (`src/connection/hub.h`): every tracker packet with its id, and a
type 255 packet per id every 100 ms that tells the ESP32 hub the tracker's address.

- Ids: the hip gives each address an id the first time it sees it, wired trackers at once and
  wireless ones when they pair; the table (16 ids, the ESP32 hub's limit) is stored. SlimeVR knows
  trackers by address, so their body assignments survive the switch from the receiver.
- Wireless trackers (the chest) pair with the hip as with a receiver: `pair` on the hip (or 3
  button presses) opens pairing for 60 s, then pair the chest as usual. `clear` forgets them.
  Their pipe addresses come from the hip, so a chest paired with the old receiver pairs once more.
- The hip never sleeps and stays at full rate while in hub mode; the radio's HF clock stays on.
- `wired` (or `link`) shows the inputs, the ids with their frame counts, RSSI of the wireless
  trackers, and the packets sent to the ESP32.
- The hip sends RSSI 0 for itself and the wired trackers (byte 15, except in types 1 and 4).

## Pairing (radio relay, `mode wired in left` / `in both`)

Wired trackers never pair by radio. The radio relay (the hip) pairs them by address, once:

1. The hip itself must already be paired with the receiver.
2. Plug in the chains. The hip's console names each new tracker:
   `Wired tracker <address> on the left header, not paired: run pair_wired`.
3. Put the receiver in pairing mode (its button, or `pair` on its console).
4. On the hip: `pair_wired`, or press its button 6 times. It pairs every wired tracker heard
   in the last 3 s that has no id yet: `Wired tracker <address> paired as id <n>`.
5. Leave pairing mode on the receiver.

A tracker that was paired before (by radio, or through a thigh) gets its old id back, so
SlimeVR keeps its body assignment. The hip remembers 8 addresses; `forget_wired` clears them.
Trackers with an output need no pairing: they send their address in every frame.

While the receiver does not answer, a tracker asks every 3 s whether a receiver in pairing
mode has a new address or id for it. After a receiver `clear` every tracker re-pairs with a
fresh id on its own, and a relay that moves to a new receiver re-pairs its wired trackers.

## Behaviour

- An input's UART is off until the first edge on its data pin, and turns off again after 1 s
  without valid frames, so an unused input costs no power.
- While frames arrive on an input, the tracker stays awake at full rate and keeps its radio's
  HF clock running between frames (relay).
- Chain trackers forward everything from their input unchanged, after checking its CRC; an
  older ankle's DATA frames (no address) are wrapped with the address from its HELLO.
- The relay stamps each frame with the receiver id paired for its address, recomputes the CRC
  and keeps the tracker's sequence byte. It queues 32 frames (keeping the newest) and sends up
  to 8 per pass of the connection thread, so its own frames are never starved.
- The relay waits for the previous ESB frame to finish instead of flushing it, and releases the
  HF clock only when the radio is idle, so relayed frames are not cut off.
- `CONFIG_WIRED_RELAY_OWN_RATE_DIVIDER=2` halves a relay's own rate while relaying, if airtime
  becomes a problem.

## Console

| Command | Effect |
| --- | --- |
| `mode ...` | set the role (see above), stored, reboots |
| `wired` | mode, per header counters (frames, bad frames, line errors, wakes), the trackers heard and their ids |
| `pair_wired` / `forget_wired` | pair the wired trackers heard / clear the table |
| `link` | radio ACKs per id, the tracker's own and the relayed ones |
| `debug on` / `off` | every `LOG_DBG` message |
| `trace on` / `<n>` / `off` | every (or every nth) frame handed to a transport, decoded |

Trace routes: `radio`, `radio not ready, dropped`, `relay` (a wired tracker's frame with its
receiver id), `cable` (sent upstream; id 255 until a relay stamps it), `skipped (relay rate
divider)`, `usb hid`.

## Wire format

```
COBS( [type][body] ) + 0x00          460800 baud 8N1, upstream only
0x02 HELLO: FICR device address[6], fw major, minor, patch                     (1 per second)
0x03 ADATA: address[6], packet[16] (id byte 0xFF), CRC32-K/4.2 LE over address + packet, sequence
0x01 DATA : legacy, the 21-byte ESB frame without address (older ankle firmware)
```

## Tests

`tools/wired_test/run.sh <zephyr dir>` (or `make test-tracker`) compiles
`src/connection/wired.c` on a PC with minimal Zephyr stand-ins and checks: the mode words and
their validation, the older setting's mapping, an ankle's HELLO/ADATA encoding and TX queue, a
thigh forwarding its input byte for byte (and wrapping legacy DATA), and a hip with two inputs
and four trackers: unpaired drops, pairing every tracker heard, the table's NVS round trip, id
stamping with a valid CRC per input, queue overflow, corruption and one input dropping out; and hub mode: inputs that listen at once and stay on, every wired frame handed to hub.c
by address (legacy DATA by its input's HELLO), the hub output's TX events, `pair_wired` opening
radio pairing. hub.c itself (radio and output) is not host-tested.
