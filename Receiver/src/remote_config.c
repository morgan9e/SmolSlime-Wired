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
#include "remote_config.h"
#include "connection/ble.h"
#include "ota.h"
#include "ota_proto.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(remote_config, LOG_LEVEL_INF);

#define ANSWER_TIMEOUT_MS 3000

static atomic_t waiting;
static uint8_t request[CFG_MSG_LEN];
static uint8_t seq;

// How the last request ended, logged and kept for "cfg status" (a log line can be cut on the
// console by the echo of a command)
static char result[96];
#define RESULT(level, ...)                                       \
	do {                                                     \
		snprintk(result, sizeof(result), __VA_ARGS__);   \
		LOG_##level("cfg: %s", result);                  \
	} while (0)

static void timeout_fn(struct k_work *work)
{
	if (atomic_cas(&waiting, 1, 0))
		RESULT(WRN, "tracker %u did not answer (off, not paired, or firmware without settings over the radio)",
		       request[1]);
}
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_fn);

bool remote_config_busy(void)
{
	return atomic_get(&waiting);
}

void remote_config_cancel(void)
{
	if (atomic_cas(&waiting, 1, 0)) {
		k_work_cancel_delayable(&timeout_work);
		RESULT(WRN, "tracker %u: stopped, a firmware update started", request[1]);
	}
}

/* ---- radio (ESB event handler, interrupt context) ---- */

void remote_config_esb_tracker_frame(void)
{
	if (!atomic_get(&waiting))
		return;
	// As an update offer: rides on the ACK of the next frame on pipe 1, from whichever tracker
	// sends next; only the addressed one takes it
	struct esb_payload p = {.pipe = 1, .length = CFG_MSG_LEN};
	memcpy(p.data, request, CFG_MSG_LEN);
	esb_write_payload(&p);
}

void remote_config_esb_rx(const struct esb_payload *payload)
{
	remote_config_ble_rx(payload->data, payload->length);
}

void remote_config_ble_rx(const uint8_t *d, uint16_t length)
{
	if (length != CFG_MSG_LEN || d[0] != CFG_MSG_RESP || d[1] != request[1] || d[2] != request[2])
		return; // not an answer, or one to an earlier request
	if (!atomic_cas(&waiting, 1, 0))
		return; // answered already: the tracker repeats its answer
	k_work_cancel_delayable(&timeout_work);
	uint16_t id = d[4] | d[5] << 8;
	int32_t value = (int32_t)ota_get_u32(&d[6]);
	if (d[3] == CFG_STATUS_OK)
		RESULT(INF, "tracker %u setting %u = %d", d[1], id, value);
	else
		RESULT(WRN, "tracker %u setting %u: %s", d[1], id, cfg_status_name(d[3]));
}

/* ---- console ---- */

static void print_status(void)
{
	if (atomic_get(&waiting))
		printk("cfg: waiting for tracker %u\n", request[1]);
	else
		printk("cfg: idle%s%s\n", result[0] ? ", last: " : "", result);
}

void remote_config_console(int argc, char **argv)
{
	uint8_t op = 0xFF;
	if (argc >= 4) {
		if (strcmp(argv[2], "get") == 0 && argc == 4)
			op = CFG_OP_READ;
		else if (strcmp(argv[2], "set") == 0 && argc == 5)
			op = CFG_OP_WRITE;
		else if (strcmp(argv[2], "reset") == 0 && argc == 4)
			op = CFG_OP_RESET;
	}
	if (op == 0xFF) {
		if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0))
			print_status();
		else
			printk("Usage: cfg <tracker id> get|reset <setting id> | cfg <tracker id> set <setting id> <value> | cfg status\n"
			       "       (setting ids as the tracker's list_config; tools/remote_config.py takes names)\n");
		return;
	}
	if (ota_busy()) {
		printk("cfg: error: a firmware update is running\n");
		return;
	}
	unsigned long tracker = strtoul(argv[1], NULL, 0);
	unsigned long id = strtoul(argv[3], NULL, 0);
	long value = op == CFG_OP_WRITE ? strtol(argv[4], NULL, 0) : 0;
	if (tracker > 255 || id > 0xFFFF) {
		printk("cfg: error: tracker %lu, setting %lu\n", tracker, id);
		return;
	}
	atomic_set(&waiting, 0);
	k_work_cancel_delayable(&timeout_work);
	seq++;
	request[0] = CFG_MSG_REQ;
	request[1] = tracker;
	request[2] = seq;
	request[3] = op;
	request[4] = id;
	request[5] = id >> 8;
	ota_put_u32(&request[6], (uint32_t)value);
	result[0] = '\0';
	atomic_set(&waiting, 1);
	k_work_reschedule(&timeout_work, K_MSEC(ANSWER_TIMEOUT_MS));
	if (ble_mode()) // straight to the tracker's settings characteristic, no ACKs to ride on
	{
		int err = ble_write_config(tracker, request, CFG_MSG_LEN);
		if (err)
		{
			atomic_set(&waiting, 0);
			k_work_cancel_delayable(&timeout_work);
			RESULT(WRN, "tracker %lu is not connected over BLE (%d)", tracker, err);
			printk("cfg: error: %s\n", result);
			return;
		}
	}
	printk("cfg: sent to tracker %lu\n", tracker);
}
