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

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>

LOG_MODULE_REGISTER(ota, LOG_LEVEL_INF);

static atomic_t acked;
static K_SEM_DEFINE(confirm_sem, 0, 1);

void ota_print_status(void)
{
	// BOOT_SWAP_TYPE_NONE is 1, the others follow (zephyr/dfu/mcuboot.h)
	static const char *const swap_names[] = {"none", "test", "perm", "revert", "fail"};
	int swap = mcuboot_swap_type();
	printk("Image: %s (%ld of %d frames acknowledged), next boot: swap %s\n",
	       boot_is_img_confirmed() ? "confirmed" : "pending", (long)atomic_get(&acked),
	       CONFIG_TRACKER_OTA_CONFIRM_ACKS,
	       swap >= BOOT_SWAP_TYPE_NONE && swap <= BOOT_SWAP_TYPE_FAIL ? swap_names[swap - BOOT_SWAP_TYPE_NONE] : "unknown");
}

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
	if (boot_is_img_confirmed())
		return;
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
	else
		LOG_INF("Image confirmed");
}

// Its own stack: the flash write needs more than the system workqueue has.
K_THREAD_DEFINE(ota_confirm_thread_id, 1024, ota_confirm_thread, NULL, NULL, NULL, 7, 0, 0);

#endif
