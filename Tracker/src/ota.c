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

#if CONFIG_TRACKER_OTA

#include "ota_proto.h"
#include "connection/esb.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

#define FRAME_WAIT_MS 20      // a frame with its retransmits is done well within this
#define STALL_TIMEOUT_MS 5000 // no chunk for this long: give up, keep the running image
#define DONE_REPEATS 5

/* ---- self-confirm of a new image ---- */

static atomic_t acked;
static atomic_t image_confirmed;
bool ota_image_confirmed(void) { return atomic_get(&image_confirmed); }
static K_SEM_DEFINE(confirm_sem, 0, 1);

void ota_tx_acked(void)
{
	if (atomic_inc(&acked) + 1 == CONFIG_TRACKER_OTA_CONFIRM_ACKS)
		k_sem_give(&confirm_sem);
}

// A swapped-in image runs as a test: MCUboot swaps the previous one back on the next reboot
// unless it is confirmed. Confirm only once the receiver has heard this image, so an image that
// boots but cannot track or reach the receiver is rolled back.
static void ota_confirm_thread(void)
{
	if (boot_is_img_confirmed()) {
		atomic_set(&image_confirmed, 1);
		return;
	}
	if (CONFIG_TRACKER_OTA_CONFIRM_ACKS == 0) {
		LOG_WRN("New image, never confirmed (TRACKER_OTA_CONFIRM_ACKS=0): the next reboot rolls back");
		return;
	}
	LOG_INF("New image: confirming after %d frames acknowledged by the receiver",
		CONFIG_TRACKER_OTA_CONFIRM_ACKS);
	k_sem_take(&confirm_sem, K_FOREVER);
	int err = boot_write_img_confirmed();
	if (err)
		LOG_ERR("Confirming the image failed: %d", err);
	else {
		atomic_set(&image_confirmed, 1);
		LOG_INF("Image confirmed");
	}
}

// Its own stack: the flash write needs more than the system workqueue has.
K_THREAD_DEFINE(ota_confirm_thread_id, 1024, ota_confirm_thread, NULL, NULL, NULL, 7, 0, 0);

/* ---- receiving an update ---- */

enum { OTA_IDLE, OTA_OFFERED, OTA_ACTIVE };

static atomic_t state = ATOMIC_INIT(OTA_IDLE);
static uint8_t own_id;
static uint8_t offer[OTA_OFFER_LEN]; // as received; checked by the thread
static uint8_t offer_len;
static uint8_t session;
static uint32_t offer_size, offer_crc;
static uint32_t board_crc;

static volatile uint32_t wanted;       // position (session, offset) of the chunk being asked for
static uint8_t chunk[OTA_CHUNK_MAX];
static volatile uint8_t chunk_len;
static atomic_t chunk_ready;
static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(tx_sem, 0, 1);

static uint32_t received; // bytes written so far, for the status
static int last_result = -1;

bool ota_active(void)
{
	return atomic_get(&state) != OTA_IDLE;
}

void ota_rx_ack(const uint8_t *data, uint8_t len, uint8_t tracker_id)
{
	if (len >= OTA_OFFER_MIN_LEN && data[0] == OTA_MSG_OFFER) {
		// Any version of the protocol starts its offer with [type][tracker id][protocol]: take it
		// for the thread, which checks it and answers what it cannot take
		if (data[1] != tracker_id || atomic_get(&state) != OTA_IDLE)
			return;
		offer_len = MIN(len, sizeof(offer));
		memcpy(offer, data, offer_len);
		own_id = tracker_id;
		atomic_set(&state, OTA_OFFERED);
		k_sem_give(&start_sem);
		return;
	}
	if (len > OTA_CHUNK_HEADER && data[0] == OTA_MSG_CHUNK && atomic_get(&state) == OTA_ACTIVE) {
		if (ota_get_u24(&data[1]) != wanted || atomic_get(&chunk_ready))
			return; // not the one asked for: queued before the last request, or by an earlier update
		chunk_len = MIN(len - OTA_CHUNK_HEADER, OTA_CHUNK_MAX);
		memcpy(chunk, &data[OTA_CHUNK_HEADER], chunk_len);
		atomic_set(&chunk_ready, 1);
	}
}

void ota_tx_done(void)
{
	// The ACK payload of a frame is handled in the same interrupt, right after this: by the time
	// the thread runs, a chunk that came with this ACK is in place.
	if (atomic_get(&state) == OTA_ACTIVE)
		k_sem_give(&tx_sem);
}

static void send_done(uint8_t status)
{
	uint8_t msg[OTA_DONE_LEN] = {OTA_MSG_DONE, own_id, status, OTA_PROTOCOL};
	for (int i = 0; i < DONE_REPEATS; i++) {
		esb_write_ota(msg, sizeof(msg));
		k_msleep(5);
	}
}

// CRC32 of the first size bytes of the running image (slot 0 holds it as the update sent it:
// MCUboot header, image, TLVs)
static uint32_t running_image_crc(uint32_t size)
{
	static uint8_t buf[256];
	const struct flash_area *fa;
	if (flash_area_open(FIXED_PARTITION_ID(slot0_partition), &fa))
		return 0;
	uint32_t crc = 0;
	size = MIN(size, fa->fa_size);
	for (uint32_t off = 0; off < size; off += sizeof(buf)) {
		uint32_t n = MIN(sizeof(buf), size - off);
		if (flash_area_read(fa, off, buf, n)) {
			crc = 0;
			break;
		}
		crc = crc32_ieee_update(crc, buf, n);
	}
	flash_area_close(fa);
	return crc;
}

// The offer in offer[]: -1 to take it, or the status to refuse it with
static int check_offer(void)
{
	if (offer_len != OTA_OFFER_LEN || offer[2] != OTA_PROTOCOL) {
		LOG_WRN("Update offer in OTA protocol %u, this firmware speaks %u", offer[2], OTA_PROTOCOL);
		return OTA_STATUS_PROTOCOL;
	}
	session = offer[3];
	offer_size = ota_get_u32(&offer[4]);
	offer_crc = ota_get_u32(&offer[8]);
	if (ota_get_u32(&offer[12]) != board_crc)
		return OTA_STATUS_WRONG_BOARD;
	if (offer_size == 0 || offer_size > FIXED_PARTITION_SIZE(slot1_partition) - 0x1000 || // room for the trailer
	    offer_size > OTA_OFFSET_MASK)
		return OTA_STATUS_TOO_LARGE;
	// Slot 1 holds the previous image until this one is confirmed: writing it would lose the
	// fallback MCUboot swaps back to
	if (!boot_is_img_confirmed())
		return OTA_STATUS_PENDING;
	if (running_image_crc(offer_size) == offer_crc)
		return OTA_STATUS_SAME_IMAGE;
	return -1;
}

static int receive_image(void)
{
	static struct flash_img_context ctx; // large: not on the stack
	int err = flash_img_init(&ctx);
	if (err) {
		LOG_ERR("Slot 1 not available: %d", err);
		return OTA_STATUS_FLASH_ERROR;
	}
	uint32_t offset = 0, crc = 0;
	uint32_t next_log = offer_size / 10;
	int64_t start = k_uptime_get(), last_chunk = start;
	received = 0;
	while (offset < offer_size) {
		uint8_t req[OTA_REQ_LEN] = {OTA_MSG_REQ, own_id};
		wanted = ota_position(session, offset);
		ota_put_u24(&req[2], wanted);
		atomic_set(&chunk_ready, 0);
		k_sem_reset(&tx_sem);
		esb_write_ota(req, sizeof(req));
		// The receiver queues the chunk asked for, and it comes with the ACK of the next request:
		// ask again as soon as this frame is done until it is there.
		k_sem_take(&tx_sem, K_MSEC(FRAME_WAIT_MS));
		if (!atomic_get(&chunk_ready)) {
			if (k_uptime_get() - last_chunk > STALL_TIMEOUT_MS) {
				LOG_ERR("Update stalled at %u of %u bytes", offset, offer_size);
				return OTA_STATUS_TIMEOUT;
			}
			continue;
		}
		uint32_t n = MIN(chunk_len, offer_size - offset);
		crc = crc32_ieee_update(crc, chunk, n);
		err = flash_img_buffered_write(&ctx, chunk, n, false); // erases each page on the way
		if (err) {
			LOG_ERR("Writing slot 1 failed at %u: %d", offset, err);
			return OTA_STATUS_FLASH_ERROR;
		}
		offset += n;
		received = offset;
		last_chunk = k_uptime_get();
		if (offset >= next_log) {
			LOG_INF("Update: %u%% (%u bytes, %lld s)", (uint32_t)((uint64_t)offset * 100 / offer_size),
				offset, (k_uptime_get() - start) / 1000);
			next_log += offer_size / 10;
		}
	}
	err = flash_img_buffered_write(&ctx, NULL, 0, true);
	if (err) {
		LOG_ERR("Writing slot 1 failed at the end: %d", err);
		return OTA_STATUS_FLASH_ERROR;
	}
	if (crc != offer_crc) {
		LOG_ERR("Update CRC %08x, expected %08x", crc, offer_crc);
		return OTA_STATUS_CRC_ERROR;
	}
	LOG_INF("Update received: %u bytes in %lld s", offer_size, (k_uptime_get() - start) / 1000);
	return OTA_STATUS_INSTALLED;
}

static void ota_thread(void)
{
	board_crc = crc32_ieee((const uint8_t *)CONFIG_BOARD_TARGET, strlen(CONFIG_BOARD_TARGET));
	while (1) {
		k_sem_take(&start_sem, K_FOREVER);
		// OFFERED/ACTIVE already suppress duplicates during a transfer. Once idle,
		// validate and answer again: receiver reboot can reuse a session number,
		// and a failed transfer must remain retryable with the same image.
		int status = check_offer();
		if (status >= 0) {
			LOG_WRN("Update offer refused: %s", ota_status_name(status));
			last_result = status;
			send_done(status);
			atomic_set(&state, OTA_IDLE);
			continue;
		}
		LOG_INF("Update offered: %u bytes; tracking paused while receiving", offer_size);
		atomic_set(&state, OTA_ACTIVE);
		status = receive_image();
		last_result = status;
		if (status == OTA_STATUS_INSTALLED) {
			int err = boot_request_upgrade(BOOT_UPGRADE_TEST);
			if (err) {
				LOG_ERR("Marking slot 1 for the swap failed: %d", err);
				status = OTA_STATUS_FLASH_ERROR;
				last_result = status;
			}
		}
		send_done(status);
		if (status == OTA_STATUS_INSTALLED) {
			LOG_INF("Rebooting into the new image (MCUboot swaps it in, it confirms itself once heard)");
			k_msleep(100);
			sys_reboot(SYS_REBOOT_COLD);
		}
		LOG_WRN("Update failed (%s), tracking again with the running image", ota_status_name(status));
		atomic_set(&state, OTA_IDLE);
	}
}

K_THREAD_DEFINE(ota_thread_id, 2048, ota_thread, NULL, NULL, NULL, 6, 0, 0);

void ota_print_status(void)
{
	// BOOT_SWAP_TYPE_NONE is 1, the others follow (zephyr/dfu/mcuboot.h)
	static const char *const swap_names[] = {"none", "test", "perm", "revert", "fail"};
	int swap = mcuboot_swap_type();
	if (boot_is_img_confirmed())
		printk("Image: confirmed");
	else
		printk("Image: pending (%ld of %d frames acknowledged)", (long)atomic_get(&acked),
		       CONFIG_TRACKER_OTA_CONFIRM_ACKS);
	printk(", next boot: swap %s, OTA protocol %u\n",
	       swap >= BOOT_SWAP_TYPE_NONE && swap <= BOOT_SWAP_TYPE_FAIL ? swap_names[swap - BOOT_SWAP_TYPE_NONE] : "unknown",
	       OTA_PROTOCOL);
	if (ota_active())
		printk("Update: receiving, %u of %u bytes\n", received, offer_size);
	else if (last_result >= 0)
		printk("Update: last one %s (%d)\n", ota_status_name(last_result), last_result);
}

#endif
