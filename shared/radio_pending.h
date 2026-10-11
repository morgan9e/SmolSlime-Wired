/* SPDX-License-Identifier: MIT */
#ifndef SLIME_RADIO_PENDING_H
#define SLIME_RADIO_PENDING_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Call under the radio lock. Keep the newest update until delivery is ACKed.
 * An ACK for an older in-flight update must never discard its replacement. */
struct radio_pending {
    uint8_t data[21];
    uint32_t revision, in_flight_revision;
    bool pending, in_flight;
};
static inline void radio_pending_put(struct radio_pending *q, const uint8_t *data)
{
    memcpy(q->data, data, sizeof(q->data));
    q->revision++;
    q->pending = true;
}
static inline void radio_pending_start(struct radio_pending *q)
{
    q->in_flight_revision = q->revision;
    q->in_flight = true;
}
static inline void radio_pending_done(struct radio_pending *q, bool acked)
{
    if (acked && q->in_flight && q->in_flight_revision == q->revision)
        q->pending = false;
    q->in_flight = false;
}
#endif
