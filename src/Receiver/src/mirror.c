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
// UART mirror for the ESP32 hub.
//
// Wire format, one frame per packet, 1,000,000 baud 8N1, TX only:
//     COBS( packet[16] + crc16_le[2] ) + 0x00, each batch of frames preceded by one extra 0x00
//     crc16 = crc16_itu_t(0xFFFF, packet, 16)   (poly 0x1021, init 0xFFFF, not reflected)
// Packets: tracker types 0-7 exactly as passed to the HID path (byte 15 = RSSI except
// types 1 and 4), plus a type 255 address packet per stored tracker every 100 ms
// (same layout as packet_device_addr() in hid.c).
#include "globals.h"
#include "mirror.h"

#if CONFIG_HUB_UART_MIRROR

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(mirror, LOG_LEVEL_INF);

#define HUB_UART_NODE DT_CHOSEN(slimenrf_hub_uart)
BUILD_ASSERT(DT_NODE_HAS_STATUS(HUB_UART_NODE, okay), "slimenrf,hub-uart is not enabled");

#define PKT_LEN 16
#define FRAME_MAX 20 // COBS(18 bytes) is at most 19, plus the delimiter
#define BATCH_FRAMES 16
#define MAX_ADDR_PER_ROUND 32 // cap the 255 burst if a receiver holds many trackers
#define LOG_INTERVAL_MS 10000

static const struct device *const uart_dev = DEVICE_DT_GET(HUB_UART_NODE);

K_MSGQ_DEFINE(mirror_q, PKT_LEN, CONFIG_HUB_UART_MIRROR_QUEUE_LEN, 4);
static K_SEM_DEFINE(tx_done, 1, 1);

static uint8_t tx_buf[1 + BATCH_FRAMES * FRAME_MAX];
static atomic_t mirror_dropped; // since the last log
static atomic_t mirror_dropped_total;
static uint32_t mirror_sent;
static atomic_t mirror_on = ATOMIC_INIT(1);

void mirror_put(const uint8_t *packet)
{
	if (!atomic_get(&mirror_on))
		return;
	if (k_msgq_put(&mirror_q, packet, K_NO_WAIT) != 0) {
		atomic_inc(&mirror_dropped);
		atomic_inc(&mirror_dropped_total);
	}
}

void mirror_set_enabled(bool on)
{
	atomic_set(&mirror_on, on);
	if (!on)
		k_msgq_purge(&mirror_q); // nothing stale goes out when it is turned back on
	LOG_INF("Hub UART mirror %s", on ? "on" : "paused");
}

bool mirror_enabled(void)
{
	return atomic_get(&mirror_on);
}

uint32_t mirror_dropped_count(void)
{
	return (uint32_t)atomic_get(&mirror_dropped_total);
}

static size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
	size_t code_pos = 0, w = 1;
	uint8_t code = 1;
	for (size_t r = 0; r < len; r++) {
		if (in[r] == 0) {
			out[code_pos] = code;
			code_pos = w++;
			code = 1;
			continue;
		}
		out[w++] = in[r];
		if (++code == 0xFF) {
			out[code_pos] = code;
			code_pos = w++;
			code = 1;
		}
	}
	out[code_pos] = code;
	return w;
}

// Appends one framed packet to out. Returns the bytes written (at most FRAME_MAX).
static size_t frame_packet(const uint8_t *pkt, uint8_t *out)
{
	uint8_t raw[PKT_LEN + 2];
	memcpy(raw, pkt, PKT_LEN);
	uint16_t crc = crc16_itu_t(0xFFFF, pkt, PKT_LEN);
	raw[PKT_LEN] = crc & 0xFF;
	raw[PKT_LEN + 1] = crc >> 8;
	size_t n = cobs_encode(raw, sizeof(raw), out);
	out[n++] = 0;
	return n;
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&tx_done);
		break;
	default:
		break;
	}
}

// Waits until the previous transfer has finished, so tx_buf can be refilled.
// 16 frames take about 3.2 ms at 1 Mbaud, so 50 ms means the UART is stuck.
static void wait_tx_idle(void)
{
	if (k_sem_take(&tx_done, K_MSEC(50)) != 0) {
		LOG_WRN("TX stalled, aborting");
		uart_tx_abort(uart_dev);
		k_sem_take(&tx_done, K_MSEC(10));
	}
}

static void mirror_thread(void)
{
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("Hub UART not ready");
		return;
	}
	if (uart_callback_set(uart_dev, uart_cb, NULL) != 0) {
		LOG_ERR("Hub UART has no async API");
		return;
	}
	LOG_INF("Hub UART mirror on %s", uart_dev->name);

	int64_t next_addr = k_uptime_get();
	int64_t next_log = k_uptime_get() + LOG_INTERVAL_MS;
	uint16_t addr_cursor = 0;
	uint8_t pkt[PKT_LEN];

	while (1) {
		int64_t now = k_uptime_get();
		k_timeout_t wait = next_addr > now ? K_MSEC(next_addr - now) : K_NO_WAIT;
		bool have = k_msgq_get(&mirror_q, pkt, wait) == 0;

		wait_tx_idle(); // tx_buf is free from here until uart_tx() below

		// A leading delimiter lets the hub resynchronise at once, e.g. when it started
		// listening mid-frame; it ignores empty frames.
		tx_buf[0] = 0;
		size_t len = 1;
		int frames = 0;
		if (have) {
			len += frame_packet(pkt, tx_buf + len);
			frames++;
		}
		while (frames < BATCH_FRAMES && k_msgq_get(&mirror_q, pkt, K_NO_WAIT) == 0) {
			len += frame_packet(pkt, tx_buf + len);
			frames++;
		}

		now = k_uptime_get();
		if (now >= next_addr) {
			next_addr = now + CONFIG_HUB_UART_MIRROR_ADDR_PERIOD_MS;
			uint8_t count = atomic_get(&mirror_on) ? stored_trackers : 0; // paused: no address packets
			int sent_addr = 0;
			// If the batch fills up, the cursor carries on in the next round.
			while (sent_addr < MAX_ADDR_PER_ROUND && sent_addr < count && frames < BATCH_FRAMES) {
				if (addr_cursor >= count)
					addr_cursor = 0;
				memset(pkt, 0, sizeof(pkt));
				pkt[0] = 255; // receiver packet: associate id and tracker address
				pkt[1] = addr_cursor;
				memcpy(&pkt[2], &stored_tracker_addr[addr_cursor], 6);
				len += frame_packet(pkt, tx_buf + len);
				frames++;
				sent_addr++;
				addr_cursor++;
			}
		}

		if (frames > 0 && uart_tx(uart_dev, tx_buf, len, SYS_FOREVER_US) == 0)
			mirror_sent += frames;
		else
			k_sem_give(&tx_done); // nothing in flight

		if (now >= next_log) {
			next_log = now + LOG_INTERVAL_MS;
			uint32_t dropped = (uint32_t)atomic_set(&mirror_dropped, 0);
			LOG_INF("mirror: %u sent, %u dropped", mirror_sent, dropped);
			mirror_sent = 0;
		}
	}
}

// Priority 7: below the ESB thread (6), so radio work always runs first.
K_THREAD_DEFINE(mirror_thread_id, 1024, mirror_thread, NULL, NULL, NULL, 7, 0, 0);

#endif
