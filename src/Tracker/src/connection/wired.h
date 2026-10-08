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
// Wired ankle link: an ankle tracker sends its frames over UART to a relay tracker,
// which stamps the ankle's receiver id and forwards the frames over ESB.
// One image does both; the connection_over_uart setting selects the ankle role at boot.
//
// Wire format (ankle -> relay), 460800 baud 8N1:
//     COBS( [type][body] ) + 0x00
//     type 0x01 DATA : the 21-byte ESB frame (packet[16], CRC32 LE, sequence), id byte = 0xFF
//     type 0x02 HELLO: FICR device address[6], fw major, minor, patch      (once per second)
#ifndef SLIMENRF_WIRED
#define SLIMENRF_WIRED

#include <stdbool.h>
#include <stdint.h>

#define WIRED_FRAME_LEN 21          // ESB frame: packet[16] + CRC32[4] + sequence[1]
#define WIRED_ID_PLACEHOLDER 0xFF   // tracker id sent by an ankle; the relay replaces it

#if CONFIG_WIRED_LINK
// True if this tracker is a wired ankle (connection_over_uart). Fixed from boot.
bool wired_is_ankle(void);

// Wired mode: queues one ESB frame (id already set to WIRED_ID_PLACEHOLDER, CRC computed).
// Never blocks; the frame is dropped only if the small TX queue is full.
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
