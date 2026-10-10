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

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

// The PC streams the image into this window while the tracker fetches it. The tracker only ever
// asks for the offset after the last chunk it has, so the window needs to hold only what lies
// between that offset and what the PC has sent; the PC is told where the tracker is in every
// reply and never runs more than WINDOW bytes ahead of it.
#define WINDOW 16384
#define STALL_TIMEOUT_MS 10000

static uint8_t window[WINDOW];

enum { OTA_IDLE, OTA_OFFERING, OTA_SENDING };
static const char *const state_names[] = {"idle", "offering", "sending"};

static atomic_t state = ATOMIC_INIT(OTA_IDLE);
static uint32_t img_size, img_crc, img_board;
static uint8_t target;
static volatile uint32_t received; // bytes the PC has sent
static volatile uint32_t asked;    // offset the tracker asked for last: everything below it is done
static uint32_t next_log;
static int64_t start_time;

static void stall_fn(struct k_work *work)
{
	if (atomic_cas(&state, OTA_SENDING, OTA_IDLE))
		LOG_WRN("ota: tracker %u stopped asking at %u of %u bytes", target, asked, img_size);
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
	ota_put_u32(&p.data[2], img_size);
	ota_put_u32(&p.data[6], img_crc);
	ota_put_u32(&p.data[10], img_board);
	esb_write_payload(&p);
}

void ota_esb_rx(const struct esb_payload *payload)
{
	const uint8_t *d = payload->data;
	int s = atomic_get(&state);
	if (payload->length == OTA_REQ_LEN && d[0] == OTA_MSG_REQ) {
		if ((s != OTA_OFFERING && s != OTA_SENDING) || d[1] != target)
			return;
		uint32_t offset = ota_get_u24(&d[2]);
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
		struct esb_payload p = {.pipe = OTA_PIPE, .length = OTA_CHUNK_HEADER + n};
		p.data[0] = OTA_MSG_CHUNK;
		ota_put_u24(&p.data[1], offset);
		for (uint32_t i = 0; i < n; i++)
			p.data[OTA_CHUNK_HEADER + i] = window[(offset + i) % WINDOW];
		esb_write_payload(&p);
		return;
	}
	if (payload->length == OTA_DONE_LEN && d[0] == OTA_MSG_DONE && d[1] == target && s == OTA_SENDING) {
		atomic_set(&state, OTA_IDLE);
		k_work_cancel_delayable(&stall_work);
		if (d[2] == OTA_STATUS_INSTALLED)
			LOG_INF("ota: tracker %u received the image in %lld s and reboots into it", target,
				(k_uptime_get() - start_time) / 1000);
		else
			LOG_WRN("ota: tracker %u failed (%u), it keeps its firmware", target, d[2]);
	}
}

/* ---- console ---- */

static int hex_decode(const char *hex, uint8_t *out, size_t max)
{
	size_t len = strlen(hex);
	if (len % 2 || len / 2 > max)
		return -1;
	for (size_t i = 0; i < len / 2; i++) {
		char byte[3] = {hex[2 * i], hex[2 * i + 1], 0};
		char *end;
		out[i] = strtoul(byte, &end, 16);
		if (*end)
			return -1;
	}
	return len / 2;
}

static void print_status(void)
{
	int s = atomic_get(&state);
	printk("ota: %s", state_names[s]);
	if (s != OTA_IDLE)
		printk(", tracker %u, %u bytes, %u received from the PC, tracker at %u", target, img_size, received,
		       asked);
	printk("\n");
}

void ota_console(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";
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
		if (img_size == 0 || img_size >= BIT(24)) {
			printk("ota: error: size %u\n", img_size);
			return;
		}
		atomic_set(&state, OTA_OFFERING);
		printk("ota: begin %u, offering to tracker %u\n", img_size, target);
	} else if (strcmp(cmd, "d") == 0 && argc == 4) {
		// ota d <offset> <hex>: the next part of the image; the answer says where the tracker is
		uint8_t buf[64];
		uint32_t offset = strtoul(argv[2], NULL, 0);
		int n = hex_decode(argv[3], buf, sizeof(buf));
		int s = atomic_get(&state);
		if ((s != OTA_OFFERING && s != OTA_SENDING) || n <= 0 || offset != received ||
		    offset + n > img_size) {
			printk("ota: error: data at %u, expected %u\n", offset, received);
			return;
		}
		if (offset + n > asked + WINDOW) {
			printk("ota: full %u %u\n", received, asked); // ahead of the tracker: send it again later
			return;
		}
		for (int i = 0; i < n; i++)
			window[(offset + i) % WINDOW] = buf[i];
		received = offset + n;
		printk("ota: ok %u %u\n", received, asked);
	} else if (strcmp(cmd, "stop") == 0) {
		atomic_set(&state, OTA_IDLE);
		k_work_cancel_delayable(&stall_work);
		print_status();
	} else if (strcmp(cmd, "status") == 0) {
		print_status();
	} else {
		printk("Usage: ota begin <size> <crc32 hex> <board crc32 hex> <tracker id> | ota d <offset> <hex> |\n"
		       "       ota stop | ota status (tools/ota.py does all of it)\n");
	}
}
