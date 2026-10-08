# Wired ankle link (branch `hub-wired`)

Ankles are wired to their thigh tracker instead of using their own radio. The ankle sends
its frames over a short UART cable; the thigh (the "relay") stamps the ankle's receiver id
and forwards the frames over ESB together with its own. The receiver, the ESP32 hub and
SlimeVR see the ankle as a normal tracker.

A tracker in **wired mode** (radio off) sends over the cable; a tracker in **relay mode**
forwards it. Ankles use wired mode, thighs relay mode.

## Firmware

One image for every tracker: `promicro_uf2/nrf52840/stackedspiwired`.
The `connection_over_uart` setting chooses the role at boot:

| Setting | Role | What it does |
| --- | --- | --- |
| `connection_over_uart 1` | wired mode | data out of P1.04 (D8) as UART TX, radio never started |
| `connection_over_uart 0` (default) | relay mode | listens on P1.04 (D8); with nothing plugged in it behaves like `stackedspi` |

Make a tracker an ankle from its USB console:

```
write_config connection_over_uart 1
reboot
```

`wired` on the console shows which mode is active. Thighs, hip and chest keep the default.
Hip and chest simply never see an ankle. The flash layout is the same as `stackedspi`,
so calibration and pairing survive reflashing.

How one image serves both roles: the board has a transmit-only `uart0` and a receive-only `uart1`,
both on P1.04 (D8), both with `zephyr,deferred-init`. At boot,
`src/connection/wired.c` reads the setting and starts only the matching one.

## Cable

Straight-through, the same at both ends:

| Wired mode (ankle) | Relay mode (thigh) | Notes |
| --- | --- | --- |
| D8 (P1.04) | D8 (P1.04) | data: TX in wired mode, RX in relay mode, 460800 baud |
| D9 (P1.06) | D9 (P1.06) | ground reference: both ends drive D9 low (signal return only) |

Both trackers keep their own battery. D9 carries only the UART's return current (well under
1 mA), so a GPIO is enough; do not use it to share power. A 470R-1k resistor in series on D8
makes a misconfiguration harmless (both ends set to wired mode would otherwise drive D8
against each other). With nothing connected, D9 just sits low.

On the SmolSlime stack, D0-D7 and D14 are taken, so no "full-speed" pin is left. P1.04 is
a "low frequency I/O only" pin on the nRF52840: Nordic advises against UART traffic on such
pins while the radio runs, because it can increase packet loss. On the ankle this does not
matter (radio off). On the thigh, check it: compare the thigh's gap statistics in the hub
stats while relaying against not relaying.

The relay's internal pull-up keeps D8 idle when the connector is open.

## Pairing an ankle

The ankle never pairs by radio. It is paired *through* its thigh, once:

1. The thigh must already be paired with the receiver.
2. Plug the ankle into the thigh. The thigh console shows
   `Wired tracker <address> connected, not paired: run pair_wired`.
3. Put the receiver in pairing mode (button on the hub receiver, or `pair` on its console).
4. On the thigh: `pair_wired` in the console, or press its button 6 times
   (2 = calibrate, 3 = pairing reset, 4-5 = DFU, as before).
   The thigh LED blinks while it pairs and flashes 4 times on success:
   `Wired tracker <address> paired as id <n>`.
5. Leave pairing mode on the receiver.

`link` on the thigh shows the radio ACKs per id, its own and the relayed ankle's: an ACK means
the receiver's radio heard the frame. If the ankle's id is ACKed but never shows up, the receiver
log says why (for example an id it has not stored).

While the receiver does not answer, a tracker asks every 3 s whether a receiver in pairing mode
has a new address or id for it, keeping its pairing otherwise. A receiver `clear` changes the
receiver's address, so after a clear every tracker re-pairs with a fresh id on its own; a thigh
that moves to a new receiver address clears its ankle table and re-pairs the plugged-in ankle.

Each thigh remembers up to 4 ankle addresses, so ankles can be swapped between thighs that
have both paired them. `wired` shows the link state and the table; `forget_wired` clears it.

## Behaviour

- The relay's UART is off until the first edge on D8, and turns off again after 1 s without
  valid frames, so a thigh without an ankle uses no extra power.
- While relaying, the thigh stays awake at full rate (it counts as moving) and keeps its
  HF clock running between frames.
- Ankle frames are forwarded at the ankle's rate: each frame wakes the connection thread.
  At most two ankle frames are sent per pass, so the thigh's own frames are never starved.
- The relay waits for the previous ESB frame to finish (up to 4.5 ms with the default
  retransmit settings) instead of flushing it, so ankle frames are not cut off.
- Frames are checked with the ankle's CRC32 before stamping; the id is replaced and the CRC
  recomputed; the ankle's sequence byte is kept.
- `CONFIG_WIRED_RELAY_OWN_RATE_DIVIDER=2` halves the thigh's own rate while relaying, if
  airtime becomes a problem.
- An unplugged ankle keeps running until its normal motion timeout puts it to sleep.
- While not relaying, the relay sends exactly like stock (flushing the previous frame).

## Console log modes

Two independent switches on the USB console, reset on reboot:

| Command | Effect |
| --- | --- |
| `debug on` / `debug off` | every `LOG_DBG` message of the firmware (radio TX results, LED, battery, sensor...) |
| `trace on` / `trace <n>` / `trace off` | every (or every nth) frame handed to a transport, decoded from the bytes sent |

A trace line names the route and decodes the packet, e.g.
`radio id 0 #17 type 2: X 1.4 Y -3.9 Z 85.8 deg, q ..., accel ... m/s2, batt 44% 3850 mV, 27.5 C`.
Routes: `radio`, `radio not ready, dropped` (not paired), `relay` (an ankle frame, with its
receiver id), `cable` (wired mode; id 255 until the relay stamps it), `skipped (relay rate
divider)`, `usb hid`. At full rate the console cannot keep up and Zephyr reports dropped
messages; `trace 10` keeps it readable. `CONFIG_LOG_MODES_DEBUG_DEFAULT` starts with DEBUG on.

## Wire format (ankle to relay)

```
COBS( [type][body] ) + 0x00          460800 baud 8N1
0x01 DATA : 21-byte ESB frame (packet[16], CRC32-K/4.2 LE, sequence), id byte = 0xFF
0x02 HELLO: FICR device address[6], fw major, minor, patch    (1 per second)
```

## Tests

`tools/wired_test/run.sh <zephyr dir>` compiles `src/connection/wired.c` on a PC with minimal
Zephyr stand-ins and checks the ankle encoder against the relay decoder: frame sizes, busy
drop, unpaired drop, pairing table and its NVS round trip, id stamping with a valid CRC,
queue overflow, corruption and RX shutdown.
