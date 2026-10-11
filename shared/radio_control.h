/* SPDX-License-Identifier: MIT
 * Volatile receiver-authoritative mode control. No flash writes or Zephyr APIs.
 */
#ifndef SLIME_RADIO_CONTROL_H
#define SLIME_RADIO_CONTROL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RC_LEN 12
#define RC_TIMEOUT_MS 5000
#define RC_ACTIVE_MS 5000
#define RC_LEGACY_BEACON_US 1000000
enum rc_mode { RC_LEGACY, RC_TDMA };
enum rc_phase { RC_IDLE, RC_PREPARE, RC_TRIAL };
enum rc_result { RC_NONE, RC_SUCCESS, RC_MISSING, RC_ROLLED_BACK, RC_CANCELLED };
struct rc_message {
	uint32_t token;
	uint8_t mode, target, phase, required;
};
struct rc_receiver {
	struct rc_message message;
	uint8_t previous, ready, arrived, result;
	int64_t deadline;
};

struct rc_presence {
	uint8_t seen;
	int64_t last[8];
};
static inline void rc_seen(struct rc_presence *p, uint8_t id, int64_t now)
{
	if (id >= 8) return;
	p->seen |= (uint8_t)(1u << id);
	p->last[id] = now;
}
static inline uint8_t rc_active(const struct rc_presence *p, uint8_t paired, int64_t now)
{
	uint8_t active = 0;
	for (unsigned id = 0; id < 8; id++) {
		uint8_t bit = (uint8_t)(1u << id);
		if ((p->seen & paired & bit) && now >= p->last[id] && now - p->last[id] < RC_ACTIVE_MS)
			active |= bit;
	}
	return active;
}

static inline void rc_u32_write(uint8_t *b, uint32_t n)
{
	for (unsigned i = 0; i < 4; i++) b[i] = (uint8_t)(n >> (i * 8));
}
static inline uint32_t rc_u32_read(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static inline void rc_encode(uint8_t *b, const struct rc_message *m)
{
	b[0] = 0xcd; b[1] = 0x43; b[2] = 1;
	b[3] = m->mode; b[4] = m->target; b[5] = m->phase;
	b[6] = m->required; b[7] = 0; rc_u32_write(b + 8, m->token);
}
static inline bool rc_decode(struct rc_message *m, const uint8_t *b, size_t len)
{
	if (len != RC_LEN || b[0] != 0xcd || b[1] != 0x43 || b[2] != 1 ||
	    b[3] > RC_TDMA || b[4] > RC_TDMA || b[5] > RC_TRIAL || b[7]) return false;
	if ((b[5] == RC_PREPARE) == (b[3] == b[4])) return false;
	*m = (struct rc_message){.mode=b[3], .target=b[4], .phase=b[5],
		.required=b[6], .token=rc_u32_read(b+8)};
	return true;
}
static inline void rc_reply(uint8_t *b, const struct rc_message *m, uint8_t id)
{
	rc_encode(b, m); b[1] = 0x41; b[7] = id;
}
static inline bool rc_begin(struct rc_receiver *r, uint8_t mode, uint8_t required, int64_t now)
{
	if (mode > RC_TDMA || !required || r->message.phase != RC_IDLE || mode == r->message.mode)
		return false;
	r->previous = r->message.mode;
	r->message.target = mode;
	r->message.required = required;
	r->message.phase = RC_PREPARE;
	r->message.token++;
	r->ready = r->arrived = 0; r->result = RC_NONE;
	r->deadline = now + RC_TIMEOUT_MS;
	return true;
}
static inline bool rc_ack(struct rc_receiver *r, const uint8_t *b, size_t len)
{
	if (len != RC_LEN || b[0] != 0xcd || b[1] != 0x41 || b[2] != 1 || b[7] >= 8 ||
	    b[3] != r->message.mode || b[4] != r->message.target ||
	    b[5] != r->message.phase || b[6] != r->message.required ||
	    rc_u32_read(b+8) != r->message.token) return false;
	uint8_t bit = (uint8_t)(1u << b[7]);
	if (!(r->message.required & bit)) return false;
	if (r->message.phase == RC_PREPARE) r->ready |= bit;
	if (r->message.phase == RC_TRIAL) r->arrived |= bit;
	return true;
}
static inline void rc_cancel(struct rc_receiver *r)
{
	if (r->message.phase == RC_IDLE) return;
	r->message.mode = r->previous;
	r->message.target = r->previous;
	r->message.phase = RC_IDLE;
	r->message.token++; /* retire queued replies from the cancelled operation */
	r->result = RC_CANCELLED;
}
static inline void rc_tick(struct rc_receiver *r, int64_t now)
{
	if (r->message.phase == RC_PREPARE && r->ready == r->message.required) {
		r->message.mode = r->message.target;
		r->message.phase = RC_TRIAL;
		r->deadline = now + RC_TIMEOUT_MS;
	} else if (r->message.phase == RC_TRIAL && r->arrived == r->message.required) {
		r->message.phase = RC_IDLE;
		r->result = RC_SUCCESS;
	} else if (r->message.phase != RC_IDLE && now >= r->deadline) {
		r->result = r->message.phase == RC_PREPARE ? RC_MISSING : RC_ROLLED_BACK;
		r->message.mode = r->previous;
		r->message.target = r->previous;
		r->message.phase = RC_IDLE;
		r->message.token++;
	}
}
#endif
