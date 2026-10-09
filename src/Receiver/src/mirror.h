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
#ifndef SLIMENRF_MIRROR
#define SLIMENRF_MIRROR

#include <stdbool.h>
#include <stdint.h>

// Queues one 16-byte tracker packet (RSSI already in byte 15) for the hub UART.
// Never blocks; safe from the ESB event handler. No-op when the mirror is disabled.
// mirror_set_enabled(false) pauses the hub feed (the "mirror" console command): no tracker
// packets and no address packets until it is turned on again or the receiver reboots.
#if CONFIG_HUB_UART_MIRROR
void mirror_put(const uint8_t *packet);
void mirror_set_enabled(bool on);
bool mirror_enabled(void);
// Packets dropped because the queue was full, since boot
uint32_t mirror_dropped_count(void);
#else
static inline void mirror_put(const uint8_t *packet) { (void)packet; }
static inline void mirror_set_enabled(bool on) { (void)on; }
static inline bool mirror_enabled(void) { return false; }
static inline uint32_t mirror_dropped_count(void) { return 0; }
#endif

#endif
