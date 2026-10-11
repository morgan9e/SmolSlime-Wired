/* SPDX-License-Identifier: MIT */
#ifndef SLIME_RADIO_GAP_H
#define SLIME_RADIO_GAP_H
#include <stdint.h>
struct radio_gaps {
    uint32_t ge50, ge100, ge200, ge400, ge1000, max_ms;
};
static inline void radio_gap_record(struct radio_gaps *s, uint32_t ms)
{
    if (ms > s->max_ms) s->max_ms = ms;
    if (ms >= 50) s->ge50++;
    if (ms >= 100) s->ge100++;
    if (ms >= 200) s->ge200++;
    if (ms >= 400) s->ge400++;
    if (ms >= 1000) s->ge1000++;
}
#endif
