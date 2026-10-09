/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2026 SlimeVR Contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
// Hub mode ("mode hub"): this tracker takes the nRF receiver's place next to the ESP32 hub.
// It gathers its own frames, the wired trackers on both headers and the wireless trackers it
// receives over ESB (they pair with it as with a receiver), gives each tracker address an id,
// and sends everything to the ESP32 hub on D15 exactly as the receiver's UART mirror does:
//     COBS( packet[16] + crc16_le[2] ) + 0x00, 1,000,000 baud, each batch preceded by 0x00
//     crc16 = crc16_itu_t(0xFFFF, packet, 16); a type 255 address packet per id every 100 ms
// The ESP32 hub firmware needs no change.
#ifndef SLIMENRF_HUB
#define SLIMENRF_HUB

#include <stdbool.h>
#include <stdint.h>

#define HUB_MAX_IDS 16 // the ESP32 hub's default CONFIG_HUB_MAX_TRACKERS

#if CONFIG_WIRED_LINK
// Starts the hub: id table, ESB reception, output thread. Called by wired.c in hub mode.
void hub_start(void);

// This tracker's own ESB frame (packet[16], CRC, sequence), from the connection thread.
void hub_put_own(const uint8_t *frame);

// A wired tracker's 16-byte packet (id byte 0xFF), by address. Safe from UART callbacks.
void hub_put_wired(const uint8_t addr[6], const uint8_t *packet);

// Called by wired.c when a hub output transfer ended.
void hub_tx_done(void);

// Accept pairing requests from wireless trackers for 60 s ("pair", or the button).
void hub_pair_open(void);

// Forget the wireless trackers ("clear"); they pair again while pairing is open.
void hub_forget_radio(void);

void hub_print_status(void);
#endif

#endif
