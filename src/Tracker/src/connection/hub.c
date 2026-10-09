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
// Hub mode, see hub.h. The ESB part follows the receiver firmware (SlimeVR-Tracker-nRF-Receiver
// src/connection/esb.c): the same discovery address, pairing exchange, pipe addresses derived
// from this tracker's device address, CRC32 and sequence checks. The output follows its
// src/mirror.c byte for byte, so the ESP32 hub cannot tell the difference.
#include "globals.h"
#include "system/system.h"
#include "connection.h"
#include "esb.h"
#include "wired.h"
#include "hub.h"

#if CONFIG_WIRED_LINK

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(hub, LOG_LEVEL_INF);

#define PKT_LEN 16
#define FRAME_MAX 20 // COBS(18 bytes) is at most 19, plus the delimiter
#define BATCH_FRAMES 16
#define ADDR_PERIOD_MS 100
#define PAIR_WINDOW_MS 60000

enum { KIND_SELF, KIND_WIRED, KIND_RADIO };
static const char *const kind_names[] = {"this tracker", "wired", "wireless"};

// Ids are positions in this table; the ESP32 hub learns id -> address from the 255 packets and
// SlimeVR knows trackers by address, so ids only need to stay stable (stored in NVS).
static struct hub_entry {
	uint8_t addr[6];
	uint8_t used;
	uint8_t kind;
} ids[HUB_MAX_IDS];

static struct hub_stat {
	uint32_t frames, dropped_seq;
	int64_t last;
	uint8_t last_seq, rssi;
	int64_t last_seq_time;
} stats[HUB_MAX_IDS];

static int self_id = -1;
static atomic_t started;
static atomic_t pairing;
static uint32_t st_sent, st_qdrop, st_full, st_bad_crc, st_unknown_id, st_pair_requests;

static void persist_work_fn(struct k_work *work)
{
	sys_write(HUB_IDS_ID, NULL, ids, sizeof(ids));
}
static K_WORK_DEFINE(persist_work, persist_work_fn);

static int find_id(const uint8_t *addr)
{
	for (int i = 0; i < HUB_MAX_IDS; i++)
		if (ids[i].used && memcmp(ids[i].addr, addr, 6) == 0)
			return i;
	return -1;
}

static uint64_t addr_u64(const uint8_t *addr)
{
	uint64_t v = 0;
	memcpy(&v, addr, 6);
	return v;
}

// Returns the id for addr, adding it if it is new. Safe from interrupts (NVS write deferred).
static int get_id(const uint8_t *addr, uint8_t kind)
{
	unsigned int key = irq_lock();
	int id = find_id(addr);
	if (id < 0) {
		for (int i = 0; i < HUB_MAX_IDS && id < 0; i++)
			if (!ids[i].used)
				id = i;
		if (id >= 0) {
			memcpy(ids[id].addr, addr, 6);
			ids[id].kind = kind;
			ids[id].used = 1;
			memset(&stats[id], 0, sizeof(stats[id]));
		}
		irq_unlock(key);
		if (id >= 0) {
			LOG_INF("Tracker %012llX (%s) is id %d", addr_u64(addr), kind_names[kind], id);
			k_work_submit(&persist_work);
		} else {
			st_full++;
		}
		return id;
	}
	if (ids[id].kind != kind && kind != KIND_SELF) {
		ids[id].kind = kind; // e.g. a tracker that moved from radio to a cable
		k_work_submit(&persist_work);
	}
	irq_unlock(key);
	return id;
}

/* ------------------------------------------------------------------------------------- */
/* Output to the ESP32 hub                                                                */
/* ------------------------------------------------------------------------------------- */

K_MSGQ_DEFINE(hub_q, PKT_LEN, 128, 4);
static K_SEM_DEFINE(hub_go, 0, 1);
static K_SEM_DEFINE(tx_idle, 1, 1);
static uint8_t tx_buf[1 + BATCH_FRAMES * FRAME_MAX];

// Queues a packet whose id byte is set. Byte 15 is the RSSI, except in types 1 and 4.
static void put(uint8_t *pkt, int id, uint8_t rssi)
{
	if (!atomic_get(&started))
		return;
	pkt[1] = (uint8_t)id;
	if (pkt[0] != 1 && pkt[0] != 4)
		pkt[15] = rssi;
	stats[id].frames++;
	stats[id].last = k_uptime_get();
	stats[id].rssi = rssi;
	if (k_msgq_put(&hub_q, pkt, K_NO_WAIT) != 0)
		st_qdrop++;
}

void hub_put_own(const uint8_t *frame)
{
	if (self_id < 0)
		return;
	uint8_t pkt[PKT_LEN];
	memcpy(pkt, frame, PKT_LEN);
	put(pkt, self_id, 0);
}

void hub_put_wired(const uint8_t addr[6], const uint8_t *packet)
{
	int id = get_id(addr, KIND_WIRED);
	if (id < 0)
		return;
	uint8_t pkt[PKT_LEN];
	memcpy(pkt, packet, PKT_LEN);
	put(pkt, id, 0);
}

void hub_tx_done(void)
{
	k_sem_give(&tx_idle);
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

static void hub_thread(void)
{
	k_sem_take(&hub_go, K_FOREVER);
	int64_t next_addr = k_uptime_get();
	int addr_cursor = 0;
	uint8_t pkt[PKT_LEN];
	while (1) {
		int64_t now = k_uptime_get();
		bool have = k_msgq_get(&hub_q, pkt, next_addr > now ? K_MSEC(next_addr - now) : K_NO_WAIT) == 0;

		// 16 frames take about 3.2 ms at 1 Mbaud, so 50 ms means the UART is stuck.
		if (k_sem_take(&tx_idle, K_MSEC(50)) != 0)
			LOG_WRN("Hub output stalled");

		// A leading delimiter lets the hub resynchronise at once; it ignores empty frames.
		tx_buf[0] = 0;
		size_t len = 1;
		int frames = 0;
		if (have) {
			len += frame_packet(pkt, tx_buf + len);
			frames++;
		}
		while (frames < BATCH_FRAMES && k_msgq_get(&hub_q, pkt, K_NO_WAIT) == 0) {
			len += frame_packet(pkt, tx_buf + len);
			frames++;
		}
		now = k_uptime_get();
		if (now >= next_addr) {
			next_addr = now + ADDR_PERIOD_MS;
			// One 255 packet per id: associates the id with the tracker's address.
			for (int n = 0; n < HUB_MAX_IDS && frames < BATCH_FRAMES; n++, addr_cursor = (addr_cursor + 1) % HUB_MAX_IDS) {
				if (!ids[addr_cursor].used)
					continue;
				memset(pkt, 0, sizeof(pkt));
				pkt[0] = 255;
				pkt[1] = addr_cursor;
				memcpy(&pkt[2], ids[addr_cursor].addr, 6);
				len += frame_packet(pkt, tx_buf + len);
				frames++;
			}
		}
		if (frames > 0 && wired_hub_tx(tx_buf, len) == 0)
			st_sent += frames;
		else
			k_sem_give(&tx_idle); // nothing in flight
	}
}

// Priority 7: below the radio, like the receiver's mirror thread.
K_THREAD_DEFINE(hub_thread_id, 1024, hub_thread, NULL, NULL, NULL, 7, 0, 0);

/* ------------------------------------------------------------------------------------- */
/* Wireless trackers: ESB reception, as on the receiver                                   */
/* ------------------------------------------------------------------------------------- */

static const uint8_t discovery_base_addr_0[4] = {0x62, 0x39, 0x8A, 0xF2};
static const uint8_t discovery_addr_prefix[8] = {0xFE, 0xFF, 0x29, 0x27, 0x09, 0x02, 0xB2, 0xD6};

static uint8_t radio_addr[6]; // trackers derive their pipe addresses from it (device address)
static struct esb_payload rx_payload;
static struct esb_payload reply = ESB_CREATE_PAYLOAD(0, 0, 0, 0, 0, 0, 0, 0, 0);

static void flush_work_fn(struct k_work *work)
{
	esb_flush_tx(); // drop an unclaimed pairing reply before the next request
}
static K_WORK_DELAYABLE_DEFINE(flush_work, flush_work_fn);

static void pair_close_fn(struct k_work *work)
{
	atomic_clear(&pairing);
	set_led(SYS_LED_PATTERN_OFF, SYS_LED_PRIORITY_PAIR);
	LOG_INF("Pairing closed");
}
static K_WORK_DELAYABLE_DEFINE(pair_close_work, pair_close_fn);

// A pairing request: [crc8 of address][0 request / 1 fetch][device address 6].
// The reply goes out with the ACK of the next packet on pipe 0: [crc8][id][hub address 6].
static void handle_pair_request(const uint8_t *d)
{
	if (d[1] != 0 || !atomic_get(&pairing))
		return; // only the first packet of a burst asks; the second fetches the reply
	st_pair_requests++;
	uint8_t checksum = crc8_ccitt(0x07, &d[2], 6);
	if (checksum == 0)
		checksum = 8;
	if (checksum != d[0])
		return;
	int id = get_id(&d[2], KIND_RADIO);
	if (id < 0)
		return;
	reply.pipe = 0;
	reply.noack = false;
	reply.data[0] = checksum;
	reply.data[1] = (uint8_t)id;
	memcpy(&reply.data[2], radio_addr, 6);
	esb_write_payload(&reply);
	k_work_reschedule(&flush_work, K_MSEC(10));
}

static void handle_frame(const uint8_t *d, uint8_t rssi)
{
	uint32_t crc;
	memcpy(&crc, &d[16], sizeof(crc));
	if (crc != crc32_k_4_2_update(0x93a409eb, d, 16)) {
		st_bad_crc++;
		return;
	}
	uint8_t id = d[1];
	if (id >= HUB_MAX_IDS || !ids[id].used || ids[id].kind != KIND_RADIO) {
		st_unknown_id++;
		return;
	}
	// The receiver's sequence check: drop repeats (missed ACK) and older frames.
	struct hub_stat *s = &stats[id];
	uint8_t seq = d[20], next = s->last_seq + 1;
	int64_t now = k_uptime_get();
	if (seq != 0 && s->last_seq != 0 && next != seq && now - s->last_seq_time < 100) {
		bool older = next < 128 ? (seq < next || seq >= next + 128) : (seq < next && seq >= next - 128);
		if (seq == s->last_seq || older) {
			s->dropped_seq++;
			return;
		}
	}
	s->last_seq = seq;
	s->last_seq_time = now;
	uint8_t pkt[PKT_LEN];
	memcpy(pkt, d, PKT_LEN);
	put(pkt, id, rssi);
}

static void hub_esb_handler(struct esb_evt const *event)
{
	if (event->evt_id != ESB_EVENT_RX_RECEIVED)
		return;
	while (esb_read_rx_payload(&rx_payload) == 0) {
		if (rx_payload.pipe == 0) {
			if (rx_payload.length == 8)
				handle_pair_request(rx_payload.data);
		} else if (rx_payload.length == WIRED_FRAME_LEN) {
			handle_frame(rx_payload.data, rx_payload.rssi);
		}
	}
}

static int hub_esb_start(void)
{
	// Pipe addresses from the device address, as the receiver derives them from its own.
	uint8_t buf[16];
	for (int i = 0; i < 4; i++) {
		buf[i] = radio_addr[i];
		buf[i + 4] = radio_addr[i] + radio_addr[4];
	}
	for (int i = 0; i < 8; i++)
		buf[i + 8] = radio_addr[5] + i;
	for (int i = 0; i < 16; i++)
		if (buf[i] == 0x00 || buf[i] == 0x55 || buf[i] == 0xAA) // invalid addresses (see datasheet)
			buf[i] += 8;

	struct esb_config config = ESB_DEFAULT_CONFIG;
	config.mode = ESB_MODE_PRX;
	config.event_handler = hub_esb_handler;
	config.selective_auto_ack = true;
	config.tx_output_power = CONFIG_2_SETTINGS_READ(CONFIG_2_RADIO_TX_POWER);
	if (CONFIG_0_SETTINGS_READ(CONFIG_0_RADIO_1MBPS))
		config.bitrate = ESB_BITRATE_1MBPS; // trackers must match (radio_1mbps)
	clocks_start(); // the receiver listens all the time: keep the HFXO on
	int err = esb_init(&config);
	if (!err)
		err = esb_set_base_address_0(discovery_base_addr_0);
	if (!err)
		err = esb_set_base_address_1(&buf[4]);
	if (!err)
		err = esb_set_prefixes(discovery_addr_prefix, ARRAY_SIZE(discovery_addr_prefix));
	if (!err)
		err = esb_start_rx();
	return err;
}

void hub_pair_open(void)
{
	if (!atomic_get(&started))
		return;
	atomic_set(&pairing, 1);
	set_led(SYS_LED_PATTERN_SHORT, SYS_LED_PRIORITY_PAIR);
	k_work_reschedule(&pair_close_work, K_MSEC(PAIR_WINDOW_MS));
	LOG_INF("Pairing open for %d s: pair the wireless trackers now", PAIR_WINDOW_MS / 1000);
}

void hub_forget_radio(void)
{
	unsigned int key = irq_lock();
	for (int i = 0; i < HUB_MAX_IDS; i++)
		if (ids[i].used && ids[i].kind == KIND_RADIO)
			ids[i].used = 0;
	irq_unlock(key);
	k_work_submit(&persist_work);
	LOG_INF("Wireless trackers forgotten; pair them again (pair)");
}

void hub_print_status(void)
{
	printk("Hub: radio address %012llX, %s; output to the ESP32 on D0: %u packets sent, %u dropped\n",
	       addr_u64(radio_addr), atomic_get(&pairing) ? "pairing open" : "pairing closed", st_sent, st_qdrop);
	printk("Radio: %u bad CRC, %u unknown id, %u pairing requests; id table full %u times\n", st_bad_crc,
	       st_unknown_id, st_pair_requests, st_full);
	int64_t now = k_uptime_get();
	for (int i = 0; i < HUB_MAX_IDS; i++) {
		if (!ids[i].used)
			continue;
		printk("  id %2d  %012llX  %-12s %u frames", i, addr_u64(ids[i].addr), kind_names[ids[i].kind],
		       stats[i].frames);
		if (stats[i].frames)
			printk(", last %lld ms ago", now - stats[i].last);
		if (ids[i].kind == KIND_RADIO)
			printk(", RSSI -%u, %u out of sequence", stats[i].rssi, stats[i].dropped_seq);
		printk("\n");
	}
}

void hub_start(void)
{
	sys_read(HUB_IDS_ID, ids, sizeof(ids));
	memcpy(radio_addr, (const void *)NRF_FICR->DEVICEADDR, 6);
	self_id = get_id(radio_addr, KIND_SELF);
	atomic_set(&started, 1);
	int err = hub_esb_start();
	if (err)
		LOG_ERR("Hub radio failed to start: %d", err);
	else
		LOG_INF("Hub: receiving on radio address %012llX, this tracker is id %d", addr_u64(radio_addr), self_id);
	k_sem_give(&hub_go);
}

#endif
