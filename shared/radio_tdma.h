/* SPDX-License-Identifier: MIT
 * Experimental ESB TDMA v1. Shared wire format and deadline arithmetic.
 * No Zephyr dependencies: the same rules are exercised by host tests.
 */
#ifndef SLIME_RADIO_TDMA_H
#define SLIME_RADIO_TDMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TDMA_BEACON_PIPE 2 /* distinct from tracking (1), pairing (0), OTA (7) */
#define TDMA_BEACON_LEN 12
#define TDMA_SLOTS 8
#define TDMA_FRAME_US 10000
#define TDMA_SLOT_US 1000
#define TDMA_FIRST_SLOT_US 1000
#define TDMA_GUARD_US 200
#define TDMA_START_WINDOW_US 150
#define TDMA_PREPARE_US 350
/* Budget includes ramp-up, 21-byte frame, turnaround and ACK. No retries. */
#define TDMA_TRANSACTION_US 600

_Static_assert(TDMA_GUARD_US + TDMA_START_WINDOW_US + TDMA_TRANSACTION_US < TDMA_SLOT_US,
	       "A transaction must finish inside its slot");
_Static_assert(TDMA_FIRST_SLOT_US + TDMA_SLOTS * TDMA_SLOT_US + TDMA_PREPARE_US < TDMA_FRAME_US,
	       "Leave time to prepare the next beacon");

struct tdma_sync {
	int64_t epoch_us;
	uint32_t serial;
	bool valid;
	bool used;
};

enum tdma_action { TDMA_WAIT, TDMA_PREPARE, TDMA_SEND, TDMA_EXPIRED };

static inline void tdma_make_beacon(uint8_t *b, uint32_t serial)
{
	b[0] = 0xcd;
	b[1] = 0x54;
	b[2] = 1; /* protocol version */
	b[3] = TDMA_SLOTS;
	b[4] = TDMA_FRAME_US & 255;
	b[5] = TDMA_FRAME_US >> 8;
	b[6] = TDMA_SLOT_US & 255;
	b[7] = TDMA_SLOT_US >> 8;
	for (unsigned i = 0; i < 4; i++)
		b[8 + i] = (uint8_t)(serial >> (8 * i));
}

static inline bool tdma_accept_beacon(struct tdma_sync *s, const uint8_t *b,
				     size_t len, int64_t now_us)
{
	if (len != TDMA_BEACON_LEN || b[0] != 0xcd || b[1] != 0x54 || b[2] != 1 ||
	    b[3] != TDMA_SLOTS || (b[4] | (b[5] << 8)) != TDMA_FRAME_US ||
	    (b[6] | (b[7] << 8)) != TDMA_SLOT_US)
		return false;
	uint32_t serial = (uint32_t)b[8] | ((uint32_t)b[9] << 8) |
		((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
	/* Ignore repeats/out-of-order beacons while locked, including serial wrap.
	 * After an outage permit a receiver reboot with a reset counter. */
	if (s->valid && now_us - s->epoch_us < 2 * TDMA_FRAME_US &&
	    ((uint32_t)(serial - s->serial) == 0 ||
	     (uint32_t)(serial - s->serial) >= UINT32_C(0x80000000)))
		return false;
	s->epoch_us = now_us;
	s->serial = serial;
	s->valid = true;
	s->used = false;
	return true;
}

static inline int64_t tdma_slot_start(const struct tdma_sync *s, uint8_t id)
{
	return s->epoch_us + TDMA_FIRST_SLOT_US + id * TDMA_SLOT_US + TDMA_GUARD_US;
}

static inline enum tdma_action tdma_action_at(const struct tdma_sync *s,
					    uint8_t id, int64_t now_us)
{
	if (!s->valid || s->used || id >= TDMA_SLOTS)
		return TDMA_WAIT;
	int64_t start = tdma_slot_start(s, id);
	if (now_us > start + TDMA_START_WINDOW_US)
		return TDMA_EXPIRED; /* no holdover, no catch-up burst */
	if (now_us >= start)
		return TDMA_SEND;
	if (now_us >= start - TDMA_PREPARE_US)
		return TDMA_PREPARE;
	return TDMA_WAIT;
}

/* Advance without emitting catch-up beacons after a delayed thread wakeup. */
static inline int64_t tdma_next_beacon(int64_t scheduled, int64_t submitted)
{
	int64_t next = scheduled + TDMA_FRAME_US;
	return next < submitted + TDMA_FRAME_US ? submitted + TDMA_FRAME_US : next;
}

#endif
