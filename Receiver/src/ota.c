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
#include "ota.h"
#include "ota_proto.h"
#include "connection/esb.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

// The PC streams the image into this window (HID OUT reports, ota_hid_data) while the tracker
// fetches it. The tracker only ever asks for the offset after the last chunk it has, so the window
// needs to hold only what lies between that offset and what the PC has sent; the PC reads where
// the tracker is from the status and never runs more than WINDOW bytes ahead of it.
#define WINDOW 16384
#define STALL_TIMEOUT_MS 10000

static uint8_t window[WINDOW];

enum { OTA_IDLE, OTA_OFFERING, OTA_SENDING };
static const char *const state_names[] = {"idle", "offering", "sending"};

static atomic_t state = ATOMIC_INIT(OTA_IDLE);
bool ota_busy(void) { return atomic_get(&state) != OTA_IDLE; }
bool ota_sending(void) { return atomic_get(&state) == OTA_SENDING; }
static uint32_t img_size, img_crc, img_board;
static uint8_t target;
static uint8_t session; // 1 - OTA_SESSIONS, new with every offer
static volatile uint32_t received; // bytes the PC has sent
static volatile uint32_t asked;    // offset the tracker asked for last: everything below it is done
static uint32_t next_log;
static int64_t start_time;

// How the last update ended, logged and kept for the status: a log line can be lost on the
// console (cut by the echo of a command), the status is asked for again
static char outcome[112];
#define OUTCOME(level, ...)                                         \
	do {                                                        \
		snprintk(outcome, sizeof(outcome), __VA_ARGS__);    \
		LOG_##level("ota: %s", outcome);                    \
	} while (0)

static void stall_fn(struct k_work *work)
{
	int s = atomic_get(&state);
	if (s == OTA_SENDING && atomic_cas(&state, OTA_SENDING, OTA_IDLE))
		OUTCOME(WRN, "tracker %u stopped asking at %u of %u bytes", target, asked, img_size);
	else if (s == OTA_OFFERING && atomic_cas(&state, OTA_OFFERING, OTA_IDLE))
		OUTCOME(WRN, "tracker %u did not answer the offer (off, not paired, or firmware without OTA protocol %u)",
			target, OTA_PROTOCOL);
}
static K_WORK_DELAYABLE_DEFINE(stall_work, stall_fn);

/* ---- radio (ESB event handler, interrupt context) ---- */

void ota_esb_tracker_frame(void)
{
	if (atomic_get(&state) != OTA_OFFERING)
		return;
	// Rides on the ACK of the next frame on pipe 1, from whichever tracker sends next; one is
	// queued per frame, as each ACK uses up the payload of the one before.
	struct esb_payload p = {.pipe = 1, .length = OTA_OFFER_LEN};
	p.data[0] = OTA_MSG_OFFER;
	p.data[1] = target;
	p.data[2] = OTA_PROTOCOL;
	p.data[3] = session;
	ota_put_u32(&p.data[4], img_size);
	ota_put_u32(&p.data[8], img_crc);
	ota_put_u32(&p.data[12], img_board);
	esb_write_payload(&p);
}

void ota_esb_rx(const struct esb_payload *payload)
{
	const uint8_t *d = payload->data;
	int s = atomic_get(&state);
	if (payload->length == OTA_REQ_LEN && d[0] == OTA_MSG_REQ) {
		if ((s != OTA_OFFERING && s != OTA_SENDING) || d[1] != target)
			return;
		uint32_t position = ota_get_u24(&d[2]);
		if (position >> OTA_OFFSET_BITS != session)
			return; // left over from an earlier update
		uint32_t offset = position & OTA_OFFSET_MASK;
		if (s == OTA_OFFERING) {
			atomic_set(&state, OTA_SENDING);
			start_time = k_uptime_get();
			next_log = img_size / 10;
			LOG_INF("ota: tracker %u accepted, sending %u bytes", target, img_size);
		}
		k_work_reschedule(&stall_work, K_MSEC(STALL_TIMEOUT_MS));
		if (offset >= img_size)
			return;
		asked = offset;
		if (offset >= next_log) {
			LOG_INF("ota: tracker %u at %u%%", target, (uint32_t)((uint64_t)offset * 100 / img_size));
			next_log += img_size / 10;
		}
		// Queue exactly the chunk asked for. It rides on the ACK of the tracker's next frame (the
		// ACK of this one is already out), and the tracker asks again at once whenever an ACK did
		// not bring its chunk: two frames per chunk, and nothing to get out of step.
		uint32_t n = MIN(OTA_CHUNK_MAX, img_size - offset);
		if (offset + n > received)
			return; // the PC has not sent it yet: the tracker asks again
		compiler_barrier(); // the window holds everything below received (ota_hid_data)
		struct esb_payload p = {.pipe = OTA_PIPE, .length = OTA_CHUNK_HEADER + n};
		p.data[0] = OTA_MSG_CHUNK;
		ota_put_u24(&p.data[1], position);
		for (uint32_t i = 0; i < n; i++)
			p.data[OTA_CHUNK_HEADER + i] = window[(offset + i) % WINDOW];
		esb_write_payload(&p);
		return;
	}
	if (payload->length == OTA_DONE_LEN && d[0] == OTA_MSG_DONE && d[1] == target &&
	    (s == OTA_OFFERING || s == OTA_SENDING)) {
		atomic_set(&state, OTA_IDLE);
		k_work_cancel_delayable(&stall_work);
		if (d[2] == OTA_STATUS_INSTALLED)
			OUTCOME(INF, "tracker %u received the image in %u s and reboots into it", target,
				(uint32_t)((k_uptime_get() - start_time) / 1000));
		else if (s == OTA_OFFERING)
			OUTCOME(WRN, "tracker %u refused the update: %s (its OTA protocol %u, this receiver's %u)",
				target, ota_status_name(d[2]), d[3], OTA_PROTOCOL);
		else
			OUTCOME(WRN, "tracker %u failed: %s, it keeps its firmware", target, ota_status_name(d[2]));
	}
}

/* ---- image data from the PC (HID OUT reports) ---- */

void ota_hid_data(const uint8_t *report, int len)
{
	int s = atomic_get(&state);
	if ((s != OTA_OFFERING && s != OTA_SENDING) || len < OTA_HID_HEADER)
		return;
	uint32_t offset = ota_get_u24(&report[1]);
	uint32_t n = MIN(report[4], len - OTA_HID_HEADER);
	// Only the next part, only inside the window, never past the end: anything else is dropped,
	// and the PC goes on from what the status says
	if (n == 0 || offset != received || offset + n > img_size || offset + n > asked + WINDOW)
		return;
	for (uint32_t i = 0; i < n; i++)
		window[(offset + i) % WINDOW] = report[OTA_HID_HEADER + i];
	compiler_barrier(); // the data is in the window before the radio interrupt sees it counted
	received = offset + n;
}

/* ---- console ---- */

static void print_status(void)
{
	int s = atomic_get(&state);
	printk("ota: %s", state_names[s]);
	if (s != OTA_IDLE)
		printk(", tracker %u, %u bytes, received %u, asked %u", target, img_size, received, asked);
	else if (outcome[0])
		printk(", last: %s", outcome);
	printk("\n");
}

void ota_console(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";
	if (strcmp(cmd, "begin") == 0 && (ota_busy() || !esb_radio_update_allowed())) {
		printk("ota: error: update or radio mode change already in progress\n");
		return;
	}
	if (strcmp(cmd, "begin") == 0 && argc == 6) {
		// ota begin <size> <crc32 hex> <board crc32 hex> <tracker id>: offer at once, data follows
		atomic_set(&state, OTA_IDLE);
		k_work_cancel_delayable(&stall_work);
		img_size = strtoul(argv[2], NULL, 0);
		img_crc = strtoul(argv[3], NULL, 16);
		img_board = strtoul(argv[4], NULL, 16);
		target = strtoul(argv[5], NULL, 0);
		received = 0;
		asked = 0;
		if (img_size == 0 || img_size > OTA_OFFSET_MASK) {
			printk("ota: error: size %u\n", img_size);
			return;
		}
		session = session % OTA_SESSIONS + 1;
		outcome[0] = '\0';
		atomic_set(&state, OTA_OFFERING);
		k_work_reschedule(&stall_work, K_MSEC(STALL_TIMEOUT_MS)); // no answer to the offer
		printk("ota: begin %u, offering to tracker %u, protocol %u\n", img_size, target, OTA_PROTOCOL);
	} else if (strcmp(cmd, "stop") == 0) {
		if (atomic_set(&state, OTA_IDLE) != OTA_IDLE)
			snprintk(outcome, sizeof(outcome), "stopped from the console");
		k_work_cancel_delayable(&stall_work);
		print_status();
	} else if (strcmp(cmd, "status") == 0) {
		print_status();
	} else {
		printk("Usage: ota begin <size> <crc32 hex> <board crc32 hex> <tracker id> | ota stop | ota status\n"
		       "       (tools/ota.py does all of it; the image comes as HID OUT reports)\n");
	}
}
