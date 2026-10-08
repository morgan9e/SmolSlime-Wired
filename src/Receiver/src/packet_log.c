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
// Brief per tracker log of received orientations, for checking the radio link on the bench:
//     Tracker 2 (3CA1B2C4D5E6): orientation X 12.3 Y -4.5 Z 170.2 deg, 98 pkt/s
// plus packets the ESB handler dropped (a tracker gets its radio ACK for those too):
//     Tracker 1: 35 pkt/s ignored, id 1 is not stored on this receiver (1 stored)
// and every SILENT_LOG_MS the stored trackers nothing arrived from:
//     No packets in 10 s from: 1 (D81D4F94B220)
// X, Y, Z are roll, pitch and yaw (ZYX order) of the tracker's own quaternion, before any
// mounting or reset the server applies.
#include "globals.h"
#include "packet_log.h"

#if CONFIG_PACKET_LOG

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

LOG_MODULE_REGISTER(packet_log, LOG_LEVEL_INF);

#define PKT_LEN 16
#define RAD_TO_DEG 57.29577951f
#define HALF_PI_F 1.57079632679489661923f
#define SILENT_LOG_MS 10000

static struct k_spinlock lock;
static uint8_t last_rot[MAX_TRACKERS][PKT_LEN]; // latest packet carrying a rotation
static uint16_t pkt_count[MAX_TRACKERS]; // all packets since the last log
// Signal strength of those packets, as the radio reports it (magnitude: 60 means -60 dBm)
static uint32_t rssi_sum[MAX_TRACKERS];
static uint8_t rssi_best[MAX_TRACKERS], rssi_worst[MAX_TRACKERS];
static ATOMIC_DEFINE(fresh, MAX_TRACKERS); // last_rot changed since the last log
static ATOMIC_DEFINE(heard, MAX_TRACKERS); // any packet since the last silent check
static uint16_t drops[256][PACKET_DROP_COUNT]; // by the id byte as received, stored or not

void packet_log_put(const uint8_t *packet, uint8_t rssi)
{
	uint8_t id = packet[1];
	if (id >= MAX_TRACKERS)
		return;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (pkt_count[id] == 0 || rssi < rssi_best[id])
		rssi_best[id] = rssi;
	if (pkt_count[id] == 0 || rssi > rssi_worst[id])
		rssi_worst[id] = rssi;
	rssi_sum[id] += rssi;
	pkt_count[id]++;
	atomic_set_bit(heard, id);
	switch (packet[0]) {
	case 1: case 2: case 4: case 7:
		memcpy(last_rot[id], packet, PKT_LEN);
		atomic_set_bit(fresh, id);
		break;
	default:
		break;
	}
	k_spin_unlock(&lock, key);
}

void packet_log_drop(uint8_t id, enum packet_drop reason)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (drops[id][reason] < UINT16_MAX)
		drops[id][reason]++;
	k_spin_unlock(&lock, key);
}

static inline int16_t rd_i16(const uint8_t *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

// Quaternion as x, y, z, w; layouts as in the packet table in hid.c.
static void packet_quat(const uint8_t *p, float q[4])
{
	if (p[0] == 1 || p[0] == 4) { // Q15, wire order x, y, z, w
		for (int i = 0; i < 4; i++)
			q[i] = rd_i16(p + 2 + 2 * i) / 32768.0f;
		return;
	}
	// types 2 and 7: exponential map in q_buf, same maths as the tracker's q_iem()
	uint32_t b = (uint32_t)p[5] | ((uint32_t)p[6] << 8) | ((uint32_t)p[7] << 16) | ((uint32_t)p[8] << 24);
	float v[3] = {
		(float)(b & 1023u) / 1024.0f * 2.0f - 1.0f,
		(float)((b >> 10) & 2047u) / 2048.0f * 2.0f - 1.0f,
		(float)((b >> 21) & 2047u) / 2048.0f * 2.0f - 1.0f,
	};
	float d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	float inv_sqrt_d = 1.0f / sqrtf(d + 1e-6f);
	float a = HALF_PI_F * d * inv_sqrt_d;
	float k = sinf(a) * inv_sqrt_d;
	q[0] = k * v[0];
	q[1] = k * v[1];
	q[2] = k * v[2];
	q[3] = cosf(a);
}

static inline unsigned per_s(uint32_t n)
{
	return n * 1000u / CONFIG_PACKET_LOG_INTERVAL_MS;
}

struct rssi_stat {
	uint32_t sum;
	uint8_t best, worst;
};

static void log_tracker(uint8_t id, const uint8_t *pkt, uint16_t count, struct rssi_stat r)
{
	float q[4];
	packet_quat(pkt, q);
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float sp = 2.0f * (w * y - z * x);
	sp = sp > 1.0f ? 1.0f : (sp < -1.0f ? -1.0f : sp);
	float roll = atan2f(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y)) * RAD_TO_DEG;
	float pitch = asinf(sp) * RAD_TO_DEG;
	float yaw = atan2f(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z)) * RAD_TO_DEG;
	LOG_INF("Tracker %u (%012llX): orientation X %.1f Y %.1f Z %.1f deg, %u pkt/s, RSSI -%u dBm (best -%u, worst -%u)",
		id, stored_tracker_addr[id], (double)roll, (double)pitch, (double)yaw,
		per_s(count), count ? (unsigned)(r.sum / count) : 0, r.best, r.worst);
}

static void log_drops(uint8_t stored)
{
	for (int id = 0; id < 256; id++) {
		uint16_t d[PACKET_DROP_COUNT];
		k_spinlock_key_t key = k_spin_lock(&lock);
		memcpy(d, drops[id], sizeof(d));
		memset(drops[id], 0, sizeof(drops[id]));
		// packets that passed the sequence check; pkt_count is reset later in this interval
		uint32_t passed = d[PACKET_DROP_UNKNOWN_ID] + (id < MAX_TRACKERS ? pkt_count[id] : 0);
		k_spin_unlock(&lock, key);
		// Radio loss gives the odd out-of-order packet. Two devices paired as the same id give two
		// interleaved counters, so about as many packets fail the check as pass it.
		if (d[PACKET_DROP_SEQUENCE] >= 5 && d[PACKET_DROP_SEQUENCE] * 4u >= passed)
			LOG_WRN("Tracker %d: two devices send as id %d (two sequence counters, %u and %u pkt/s); "
				"clearing the receiver does not change a tracker's id, re-pair every tracker",
				id, id, per_s(passed), per_s(d[PACKET_DROP_SEQUENCE]));
		if (d[PACKET_DROP_UNKNOWN_ID])
			LOG_WRN("Tracker %d: %u pkt/s ignored, id %d is not stored on this receiver (%u stored)",
				id, per_s(d[PACKET_DROP_UNKNOWN_ID]), id, stored);
		if (d[PACKET_DROP_DUPLICATE])
			LOG_INF("Tracker %d: %u duplicates dropped (it missed the ACK and sent again)", id,
				d[PACKET_DROP_DUPLICATE]);
		if (d[PACKET_DROP_CHECKSUM] || d[PACKET_DROP_SEQUENCE])
			LOG_WRN("Tracker %d: dropped %u bad checksum, %u out of sequence", id,
				d[PACKET_DROP_CHECKSUM], d[PACKET_DROP_SEQUENCE]);
	}
}

static void log_silent(uint8_t stored)
{
	char buf[128];
	size_t len = 0;
	for (uint16_t id = 0; id < stored && len < sizeof(buf) - 24; id++)
		if (!atomic_test_and_clear_bit(heard, id))
			len += snprintk(buf + len, sizeof(buf) - len, "%s%u (%012llX)", len ? ", " : "", id,
					stored_tracker_addr[id]);
	if (len)
		LOG_WRN("No packets in %u s from: %s", SILENT_LOG_MS / 1000, buf);
}

static void packet_log_thread(void)
{
	uint8_t pkt[PKT_LEN];
	int64_t next_silent = k_uptime_get() + SILENT_LOG_MS;
	while (1) {
		k_msleep(CONFIG_PACKET_LOG_INTERVAL_MS);
		uint8_t count = stored_trackers;
		log_drops(count);
		if (k_uptime_get() >= next_silent) {
			next_silent += SILENT_LOG_MS;
			log_silent(count);
		}
		for (uint16_t id = 0; id < count; id++) {
			k_spinlock_key_t key = k_spin_lock(&lock);
			bool have = atomic_test_and_clear_bit(fresh, id);
			if (have)
				memcpy(pkt, last_rot[id], PKT_LEN);
			uint16_t n = pkt_count[id];
			struct rssi_stat r = {rssi_sum[id], rssi_best[id], rssi_worst[id]};
			pkt_count[id] = 0;
			rssi_sum[id] = 0;
			k_spin_unlock(&lock, key);
			if (have)
				log_tracker(id, pkt, n, r);
		}
	}
}

// Lowest priority of the app threads: logging must never delay radio, HID or the hub mirror.
K_THREAD_DEFINE(packet_log_thread_id, 1024, packet_log_thread, NULL, NULL, NULL, 10, 0, 0);

#endif
