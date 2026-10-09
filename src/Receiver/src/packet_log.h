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

#include <stdbool.h>
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
//
// Link diagnostics ("diag on" on the console, off at boot): packet_log_frame() sees every 21-byte
// ESB data frame before the receiver's own filters. Per tracker and interval it logs frames
// received, sequence numbers that never arrived (lost in the air), repeats (the tracker missed the
// ACK), the longest silence and RSSI, plus a timeline of 50 ms slots; also the radio's CRC
// good/bad counts for all trackers together, and each silence that lost frames.
// "diag hub on" (stored, survives reboots) sends the same statistics to the ESP32 hub once per
// interval, as packet types 253 (receiver) and 254 (per tracker); the hub forwards them unchanged
// to UDP 6971 on the server's PC. Layout in HUB.md.
#if CONFIG_PACKET_LOG
void packet_log_put(const uint8_t *packet, uint8_t rssi);
void packet_log_drop(uint8_t id, enum packet_drop reason);
void packet_log_frame(const uint8_t *frame, uint8_t rssi);
void packet_log_set_diag(bool on);
bool packet_log_diag(void);
void packet_log_set_diag_hub(bool on);
bool packet_log_diag_hub(void);
#else
static inline void packet_log_put(const uint8_t *packet, uint8_t rssi) { (void)packet; (void)rssi; }
static inline void packet_log_drop(uint8_t id, enum packet_drop reason) { (void)id; (void)reason; }
static inline void packet_log_frame(const uint8_t *frame, uint8_t rssi) { (void)frame; (void)rssi; }
static inline void packet_log_set_diag(bool on) { (void)on; }
static inline bool packet_log_diag(void) { return false; }
static inline void packet_log_set_diag_hub(bool on) { (void)on; }
static inline bool packet_log_diag_hub(void) { return false; }
#endif

#endif
