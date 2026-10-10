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
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

// The image store: flash the receiver firmware does not use, below its settings (0xda000 on the
// nRF52840 Dongle). The Nordic bootloader may overwrite it while updating the receiver itself:
// upload the image again afterwards.
#define STORE_ADDR 0x9a000
#define STORE_SIZE 0x40000
#define PAGE_SIZE 0x1000
#define STALL_TIMEOUT_MS 10000

extern char __rom_region_start[];
extern char _flash_used[];

static const struct device *const flash_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

enum { OTA_EMPTY, OTA_UPLOADING, OTA_READY, OTA_OFFERING, OTA_SERVING };
static const char *const state_names[] = {"empty", "uploading", "stored", "offering", "sending"};

static atomic_t state = ATOMIC_INIT(OTA_EMPTY);
static uint32_t img_size, img_crc, img_board, written;
static uint8_t target;
static int32_t last_queued = -1; // offset of the chunk queued at the previous request
static uint32_t served, next_log;
static int64_t serve_start;

static void stall_fn(struct k_work *work)
{
	if (atomic_cas(&state, OTA_SERVING, OTA_READY))
		LOG_WRN("ota: tracker %u stopped asking at %u of %u bytes", target, served, img_size);
}
static K_WORK_DELAYABLE_DEFINE(stall_work, stall_fn);

/* ---- radio (ESB event handler, interrupt context) ---- */

void ota_esb_tracker_frame(void)
{
	if (atomic_get(&state) != OTA_OFFERING)
		return;
	// Rides on the ACK of the next frame on pipe 1, from whichever tracker sends next; one is
	// queued per frame, as the ACK of each frame uses up the one queued before.
	struct esb_payload p = {.pipe = 1, .length = OTA_OFFER_LEN};
	p.data[0] = OTA_MSG_OFFER;
	p.data[1] = target;
	ota_put_u32(&p.data[2], img_size);
	ota_put_u32(&p.data[6], img_crc);
	ota_put_u32(&p.data[10], img_board);
	esb_write_payload(&p);
}

static void queue_chunk(uint32_t offset)
{
	struct esb_payload p = {.pipe = OTA_PIPE};
	uint32_t n = MIN(OTA_CHUNK_MAX, img_size - offset);
	p.data[0] = OTA_MSG_CHUNK;
	ota_put_u24(&p.data[1], offset);
	memcpy(&p.data[OTA_CHUNK_HEADER], (const uint8_t *)STORE_ADDR + offset, n);
	p.length = OTA_CHUNK_HEADER + n;
	last_queued = esb_write_payload(&p) == 0 ? (int32_t)offset : -1;
}

void ota_esb_rx(const struct esb_payload *payload)
{
	const uint8_t *d = payload->data;
	int s = atomic_get(&state);
	if (payload->length == OTA_REQ_LEN && d[0] == OTA_MSG_REQ) {
		if ((s != OTA_OFFERING && s != OTA_SERVING) || d[1] != target)
			return;
		uint32_t offset = ota_get_u24(&d[2]);
		if (s == OTA_OFFERING) {
			atomic_set(&state, OTA_SERVING);
			serve_start = k_uptime_get();
			next_log = img_size / 10;
			LOG_INF("ota: tracker %u accepted, sending %u bytes", target, img_size);
		}
		k_work_reschedule(&stall_work, K_MSEC(STALL_TIMEOUT_MS));
		if (offset >= img_size)
			return;
		served = MAX(served, offset);
		if (served >= next_log) {
			LOG_INF("ota: tracker %u at %u%%", target, (uint32_t)((uint64_t)served * 100 / img_size));
			next_log += img_size / 10;
		}
		// The ACK to this request carried the chunk queued at the previous one: if that is the
		// one asked for, the tracker has it now and asks for the next one next.
		uint32_t next = last_queued == (int32_t)offset ? offset + OTA_CHUNK_MAX : offset;
		if (next < img_size)
			queue_chunk(next);
		else
			last_queued = -1;
		return;
	}
	if (payload->length == OTA_DONE_LEN && d[0] == OTA_MSG_DONE && d[1] == target && s == OTA_SERVING) {
		atomic_set(&state, OTA_READY);
		k_work_cancel_delayable(&stall_work);
		if (d[2] == OTA_STATUS_INSTALLED)
			LOG_INF("ota: tracker %u received the image in %lld s and reboots into it", target,
				(k_uptime_get() - serve_start) / 1000);
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
	if (s != OTA_EMPTY)
		printk(", %u of %u bytes, crc %08x, board %08x", s == OTA_UPLOADING ? written : img_size, img_size,
		       img_crc, img_board);
	if (s == OTA_OFFERING || s == OTA_SERVING)
		printk(", tracker %u, %u bytes sent", target, served);
	printk("\n");
}

void ota_console(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";
	if (strcmp(cmd, "begin") == 0 && argc == 5) {
		uint32_t size = strtoul(argv[2], NULL, 0);
		uint32_t used_end = (uintptr_t)__rom_region_start + (uintptr_t)_flash_used;
		if (size == 0 || size > STORE_SIZE) {
			printk("ota: error: size %u, the store holds %u\n", size, STORE_SIZE);
			return;
		}
		if (used_end > STORE_ADDR) {
			printk("ota: error: this firmware reaches 0x%x, the store starts at 0x%x\n", used_end, STORE_ADDR);
			return;
		}
		atomic_set(&state, OTA_UPLOADING);
		img_size = size;
		img_crc = strtoul(argv[3], NULL, 16);
		img_board = strtoul(argv[4], NULL, 16);
		written = 0;
		int err = flash_erase(flash_dev, STORE_ADDR, ROUND_UP(size, PAGE_SIZE));
		if (err) {
			atomic_set(&state, OTA_EMPTY);
			printk("ota: error: erase failed (%d)\n", err);
			return;
		}
		printk("ota: begin %u\n", size);
	} else if (strcmp(cmd, "d") == 0 && argc == 4) {
		uint8_t buf[60];
		uint32_t offset = strtoul(argv[2], NULL, 0);
		int n = hex_decode(argv[3], buf, 56);
		if (atomic_get(&state) != OTA_UPLOADING || n <= 0 || offset != written || written + n > img_size) {
			printk("ota: error: data at %u, expected %u\n", offset, written);
			return;
		}
		int len = ROUND_UP(n, 4); // flash writes whole words
		memset(&buf[n], 0xff, len - n);
		int err = flash_write(flash_dev, STORE_ADDR + offset, buf, len);
		if (err) {
			printk("ota: error: write failed at %u (%d)\n", offset, err);
			return;
		}
		written += n;
		printk("ota: ok %u\n", written);
	} else if (strcmp(cmd, "end") == 0) {
		if (atomic_get(&state) != OTA_UPLOADING || written != img_size) {
			printk("ota: error: %u of %u bytes uploaded\n", written, img_size);
			return;
		}
		uint32_t crc = crc32_ieee((const uint8_t *)STORE_ADDR, img_size);
		if (crc != img_crc) {
			atomic_set(&state, OTA_EMPTY);
			printk("ota: error: stored crc %08x, expected %08x\n", crc, img_crc);
			return;
		}
		atomic_set(&state, OTA_READY);
		printk("ota: stored %u bytes, crc %08x\n", img_size, crc);
	} else if (strcmp(cmd, "send") == 0 && argc == 3) {
		int s = atomic_get(&state);
		if (s != OTA_READY && s != OTA_OFFERING) {
			printk("ota: error: no image stored (%s)\n", state_names[s]);
			return;
		}
		target = strtoul(argv[2], NULL, 0);
		last_queued = -1;
		served = 0;
		atomic_set(&state, OTA_OFFERING);
		printk("ota: offering to tracker %u\n", target);
	} else if (strcmp(cmd, "stop") == 0) {
		int s = atomic_get(&state);
		if (s == OTA_OFFERING || s == OTA_SERVING)
			atomic_set(&state, OTA_READY);
		k_work_cancel_delayable(&stall_work);
		print_status();
	} else if (strcmp(cmd, "status") == 0) {
		print_status();
	} else {
		printk("Usage: ota begin <size> <crc32 hex> <board crc32 hex> | ota d <offset> <hex> | ota end |\n"
		       "       ota send <tracker id> | ota stop | ota status\n");
	}
}
