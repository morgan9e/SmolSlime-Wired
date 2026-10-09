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
// Wired link between trackers, over two headers. The wired_mode setting ("mode" on the console)
// makes each header an input, an output or unused:
//   mode wireless                  no wired link, radio only
//   mode wired out right           radio off, own frames upstream on the right header (ankle)
//   mode wired in left out right   radio off, own frames and the left header's upstream (thigh)
//   mode wired in left             radio on: the left header's trackers are relayed over ESB
//   mode wired in both             radio on: both headers relayed over ESB (hip)
//   mode hub                       both headers in, radio receives, all to the ESP32 on D0 (hip)
// Left header: D8 data, D9 ground reference. Right header: D16 data, D10 ground reference.
// With an output the radio stays off and the chain ends at a radio relay (or the ESP32 hub),
// which knows each tracker by the address in its frames. Without one, frames from the inputs
// are stamped with the receiver id paired for their address (pair_wired) and sent over ESB.
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
#include <stddef.h>
#include <stdint.h>

#define WIRED_FRAME_LEN 21          // ESB frame: packet[16] + CRC32[4] + sequence[1]
#define WIRED_ID_PLACEHOLDER 0xFF   // tracker id sent over the wire; the relay replaces it

// wired_mode bits
#define WIRED_IN_LEFT 0x1
#define WIRED_IN_RIGHT 0x2
#define WIRED_OUT_LEFT 0x4
#define WIRED_OUT_RIGHT 0x8
// Hub: takes the receiver's place. Both headers in, the radio receives (wireless trackers such as
// the chest pair with this tracker), and everything goes to the ESP32 hub on D0 in the receiver's
// UART mirror format (hub.c). "mode hub" = WIRED_HUB | WIRED_IN_LEFT | WIRED_IN_RIGHT.
#define WIRED_HUB 0x10
#define WIRED_MODE_HUB (WIRED_HUB | WIRED_IN_LEFT | WIRED_IN_RIGHT)
#define WIRED_MODE_LEGACY (-1) // from connection_over_uart: on = in left out right, off = in left

// Valid: at most one output, no header both input and output; the hub has no header output.
static inline bool wired_mode_valid(int mode)
{
	if (mode < 0 || mode > 31)
		return false;
	int in = mode & 3, out = (mode >> 2) & 3;
	if (mode & WIRED_HUB)
		return mode == WIRED_MODE_HUB;
	return out != 3 && (in & out) == 0;
}

// Parses "wireless", "hub" or "wired [in left|right|both] [out left|right]" (argv without "mode").
// Returns the mode, or -1 if the words do not form a valid one.
int wired_mode_parse(int argc, char **argv);

// Writes the mode in the form wired_mode_parse() takes, e.g. "wired in left out right".
void wired_mode_format(int mode, char *buf, size_t len);

#if CONFIG_WIRED_LINK
// The mode in use since boot (the setting, resolved from connection_over_uart if -1).
int wired_mode(void);

// True if this tracker has a wired output: its radio stays off. Fixed from boot.
// (Named for the first wired tracker, the ankle.)
bool wired_is_ankle(void);

// True in hub mode (fixed from boot): the radio receives for hub.c instead of sending.
bool wired_is_hub(void);

// Hub mode: sends buf on the hub output (D0). The UART reports the end with hub_tx_done().
int wired_hub_tx(const uint8_t *buf, size_t len);

// Sends one ESB frame upstream as ADATA with this tracker's address.
// Never blocks; the frame is dropped only if the TX queue is full.
void wired_write_data(const uint8_t frame[WIRED_FRAME_LEN]);

// True while frames from a wired input are arriving (relaying or forwarding).
bool wired_relaying(void);

// Radio relay: pops one stamped frame for ESB. Returns false if none is queued.
bool wired_relay_get(uint8_t frame[WIRED_FRAME_LEN]);

// Pairs every wired tracker seen in the last few seconds that has no receiver id yet, through
// this relay. Runs in its own thread; returns immediately. The receiver must be in pairing mode.
void wired_request_pair_ankle(void);

void wired_print_status(void);
void wired_forget_ankles(void);
#else
static inline int wired_mode(void) { return 0; }
static inline bool wired_is_ankle(void) { return false; }
static inline bool wired_is_hub(void) { return false; }
static inline bool wired_relaying(void) { return false; }
#endif

#endif
