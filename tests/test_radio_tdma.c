/* SPDX-License-Identifier: MIT */
#include "radio_tdma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void protocol(void)
{
	uint8_t b[TDMA_BEACON_LEN];
	struct tdma_sync s = {0};
	tdma_make_beacon(b, UINT32_MAX);
	for (size_t n = 0; n < sizeof(b); n++)
		assert(!tdma_accept_beacon(&s, b, n, 0));
	assert(!tdma_accept_beacon(&s, b, sizeof(b) + 1, 0));
	for (unsigned i = 0; i < 8; i++) {
		b[i] ^= 1;
		assert(!tdma_accept_beacon(&s, b, sizeof(b), 0));
		b[i] ^= 1;
	}
	assert(tdma_accept_beacon(&s, b, sizeof(b), 100));
	s.used = true;
	assert(!tdma_accept_beacon(&s, b, sizeof(b), 200));
	assert(s.used && s.epoch_us == 100); /* duplicate cannot grant a second slot */
	tdma_make_beacon(b, 0);
	assert(tdma_accept_beacon(&s, b, sizeof(b), 10100)); /* wrap */
	assert(!s.used);
	tdma_make_beacon(b, UINT32_MAX);
	assert(!tdma_accept_beacon(&s, b, sizeof(b), 10200)); /* old */
	assert(tdma_accept_beacon(&s, b, sizeof(b), 40100)); /* reboot/reacquire */
}

static void deadlines(void)
{
	struct tdma_sync s = {.valid = true, .epoch_us = INT64_C(6000000000)};
	for (unsigned id = 0; id < TDMA_SLOTS; id++) {
		int64_t t = tdma_slot_start(&s, id);
		assert(tdma_action_at(&s, id, t - TDMA_PREPARE_US - 1) == TDMA_WAIT);
		assert(tdma_action_at(&s, id, t - TDMA_PREPARE_US) == TDMA_PREPARE);
		assert(tdma_action_at(&s, id, t - 1) == TDMA_PREPARE);
		assert(tdma_action_at(&s, id, t) == TDMA_SEND);
		assert(tdma_action_at(&s, id, t + TDMA_START_WINDOW_US) == TDMA_SEND);
		assert(tdma_action_at(&s, id, t + TDMA_START_WINDOW_US + 1) == TDMA_EXPIRED);
		assert(tdma_action_at(&s, id, t + TDMA_FRAME_US) == TDMA_EXPIRED);
		s.used = true;
		assert(tdma_action_at(&s, id, t) == TDMA_WAIT);
		s.used = false;
	}
	for (unsigned id = TDMA_SLOTS; id < 256; id++)
		assert(tdma_action_at(&s, id, tdma_slot_start(&s, id)) == TDMA_WAIT);
	s.valid = false;
	assert(tdma_action_at(&s, 0, tdma_slot_start(&s, 0)) == TDMA_WAIT);
	/* The guards must cover both clock quantization and a modest IRQ skew.
	 * This is a model bound (100 us per endpoint), not hardware validation. */
	assert(TDMA_START_WINDOW_US + TDMA_TRANSACTION_US + 2 * 100 < TDMA_SLOT_US);
}

static void fleet(void)
{
	struct tdma_sync s[TDMA_SLOTS] = {0};
	uint8_t b[TDMA_BEACON_LEN];
	unsigned sent[TDMA_SLOTS] = {0}, skipped = 0;
	int64_t previous_end = -1, beacon = 0;
	for (unsigned frame = 0; frame < 1000; frame++) {
		/* Include receiver delays of several frames: no catch-up transmissions. */
		int64_t submitted = beacon + (frame % 37 == 0 ? 35000 : 0);
		tdma_make_beacon(b, frame);
		for (unsigned id = 0; id < TDMA_SLOTS; id++) {
			if ((frame + id) % 13 == 0) { /* lose beacons independently */
				assert(tdma_action_at(&s[id], id, submitted + 9000) != TDMA_SEND);
				continue;
			}
			assert(tdma_accept_beacon(&s[id], b, sizeof(b), submitted + id * 10));
			int64_t now = tdma_slot_start(&s[id], id) + ((frame + id) % 5) * 50;
			if (tdma_action_at(&s[id], id, now) == TDMA_SEND) {
				assert(now > previous_end);
				previous_end = now + TDMA_TRANSACTION_US;
				s[id].used = true;
				sent[id]++;
				assert(tdma_action_at(&s[id], id, now) != TDMA_SEND);
			} else {
				skipped++;
			}
		}
		int64_t next = tdma_next_beacon(beacon, submitted);
		assert(next - submitted >= TDMA_FRAME_US);
		beacon = next;
	}
	assert(skipped > 0);
	for (unsigned id = 0; id < TDMA_SLOTS; id++) assert(sent[id] > 700);
}

int main(void)
{
	protocol();
	deadlines();
	fleet();
	puts("TDMA protocol, deadline and eight-tracker simulation checks passed");
	return 0;
}
