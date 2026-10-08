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
// Wired link. One image, the connection_over_uart setting picks the mode at boot:
//   off: radio relay. Listens on D8 for a wired ankle, stamps its receiver id and sends its
//        frames over ESB together with its own.
//   on:  wired chain mode, radio off. Sends its own frames upstream on D15 and forwards
//        everything arriving on D8 from downstream, unchanged. A chain ends at the ESP32 hub,
//        which knows each tracker by the address in its frames: no ids, no pairing.
//
// Wire format, 460800 baud 8N1, upstream direction only:
//     COBS( [type][body] ) + 0x00
//     type 0x02 HELLO: FICR device address[6], fw major, minor, patch      (once per second)
//     type 0x03 ADATA: address[6], packet[16] (id byte 0xFF), CRC32 LE, sequence
//                      CRC32 = crc32_k_4_2_update(0x93a409eb, address + packet, 22)
//     type 0x01 DATA : legacy, the 21-byte ESB frame without address (older ankle firmware)
#ifndef SLIMENRF_WIRED
#define SLIMENRF_WIRED

#include <stdbool.h>
#include <stdint.h>

#define WIRED_FRAME_LEN 21          // ESB frame: packet[16] + CRC32[4] + sequence[1]
#define WIRED_ID_PLACEHOLDER 0xFF   // tracker id sent by an ankle; the relay replaces it

#if CONFIG_WIRED_LINK
// True in wired chain mode (connection_over_uart), radio off. Fixed from boot.
bool wired_is_ankle(void);

// Wired chain mode: sends one ESB frame upstream as ADATA with this tracker's address.
// Never blocks; the frame is dropped only if the TX queue is full.
void wired_write_data(const uint8_t frame[WIRED_FRAME_LEN]);

// Relay: true while frames from a paired ankle are arriving.
bool wired_relaying(void);
// Pops one stamped frame for ESB. Returns false if none is queued.
bool wired_relay_get(uint8_t frame[WIRED_FRAME_LEN]);
// Pairs the ankle currently connected (last HELLO) with the receiver, through this relay.
// Runs in its own thread; returns immediately. The receiver must be in pairing mode.
void wired_request_pair_ankle(void);
void wired_print_status(void);
void wired_forget_ankles(void);
#else
static inline bool wired_is_ankle(void) { return false; }
static inline bool wired_relaying(void) { return false; }
#endif

#endif
