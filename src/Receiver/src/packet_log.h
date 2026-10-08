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
#ifndef SLIMENRF_PACKET_LOG
#define SLIMENRF_PACKET_LOG

#include <stdint.h>

// Why the ESB handler threw a tracker packet away. The tracker still got its radio ACK
// for all of these, so from the tracker's side the link looks fine.
enum packet_drop {
	PACKET_DROP_UNKNOWN_ID, // id not stored on this receiver: paired elsewhere, or removed here
	PACKET_DROP_CHECKSUM,
	PACKET_DROP_SEQUENCE,
	PACKET_DROP_DUPLICATE, // repeat of the last frame: the tracker missed the ACK and sent it again
	PACKET_DROP_COUNT
};

// Records one tracker packet; a low priority thread logs each tracker's latest
// orientation and packet rate every CONFIG_PACKET_LOG_INTERVAL_MS, plus drops and silent trackers.
// Never blocks; safe from the ESB event handler. No-op when the log is disabled.
#if CONFIG_PACKET_LOG
void packet_log_put(const uint8_t *packet, uint8_t rssi);
void packet_log_drop(uint8_t id, enum packet_drop reason);
#else
static inline void packet_log_put(const uint8_t *packet, uint8_t rssi) { (void)packet; (void)rssi; }
static inline void packet_log_drop(uint8_t id, enum packet_drop reason) { (void)id; (void)reason; }
#endif

#endif
