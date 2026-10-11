/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "radio_tdma.h"
#include "radio_pending.h"
#include "radio_gap.h"

static void recovery(void)
{
    uint8_t b[TDMA_BEACON_LEN];
    struct tdma_sync s = {0};
    tdma_make_grid_beacon(b, UINT32_MAX);
    assert(tdma_accept_beacon(&s,b,sizeof(b),10000));
    s.used = true;
    tdma_holdover(&s,20000);
    assert(s.serial == 0 && !s.used && s.beacon_us == 10000);
    assert(tdma_action_at(&s,7,tdma_slot_start(&s,7)) == TDMA_SEND);
    s.used = true;
    tdma_make_grid_beacon(b,0);
    assert(tdma_accept_beacon(&s,b,sizeof(b),20001));
    assert(s.used); /* resync to a predicted frame cannot send it twice */
    assert(!tdma_accept_beacon(&s,b,sizeof(b),20002));
    tdma_holdover(&s,20001 + TDMA_HOLDOVER_US - 1);
    assert(s.valid);
    tdma_holdover(&s,20001 + TDMA_HOLDOVER_US);
    assert(!s.valid);
    assert(tdma_action_at(&s,0,tdma_slot_start(&s,0)) == TDMA_WAIT);
    assert(tdma_accept_beacon(&s,b,sizeof(b),800000)); /* receiver restarted */
    tdma_make_beacon(b,10);
    assert(tdma_accept_beacon(&s,b,sizeof(b),900000));
    int64_t old_epoch = s.epoch_us;
    tdma_holdover(&s,1000000);
    assert(s.epoch_us == old_epoch && !s.grid); /* old receiver: no prediction */

    int64_t next=10000;
    uint32_t serial=UINT32_MAX;
    tdma_grid_advance(&next,&serial,40001);
    assert(next == 50000 && serial == 3); /* skips, does not phase-shift */
    tdma_grid_advance(&next,&serial,50000);
    assert(next == 50000 && serial == 3);
}

static void beacon_blackout(void)
{
    /* Eight clocks spanning +/-100 ppm. Lose all beacons for 690 ms.
     * Continue through the bounded holdover, then listen until resynchronized.
     * Check actual transaction bounds in the receiver's time domain. */
    struct tdma_sync s[8] = {0};
    uint8_t b[TDMA_BEACON_LEN];
    for (unsigned frame=0; frame<90; frame++) {
        double last_end = -1;
        for (unsigned id=0; id<8; id++) {
            double rate=1.0+((int)id*200.0/7-100)/1000000;
            int64_t epoch=1000000+(int64_t)frame*TDMA_FRAME_US;
            if (!frame || frame>=70) {
                tdma_make_grid_beacon(b,frame);
                assert(tdma_accept_beacon(&s[id],b,sizeof(b),(int64_t)(epoch*rate)));
            }
            int64_t now=(int64_t)((epoch+TDMA_FIRST_SLOT_US+id*TDMA_SLOT_US)*rate);
            tdma_holdover(&s[id],now);
            if (frame>=50 && frame<70) { assert(!s[id].valid); continue; }
            assert(s[id].valid);
            int64_t start=tdma_slot_start(&s[id],id);
            assert(tdma_action_at(&s[id],id,start)==TDMA_SEND);
            double actual=start/rate;
            double slot=epoch+TDMA_FIRST_SLOT_US+id*TDMA_SLOT_US;
            assert(actual>slot && actual+TDMA_TRANSACTION_US<slot+TDMA_SLOT_US);
            assert(actual>last_end);
            last_end=actual+TDMA_TRANSACTION_US;
            s[id].used=true;
            assert(tdma_action_at(&s[id],id,start)==TDMA_WAIT);
        }
    }
}

static void latest_update(void)
{
    struct radio_pending q={0};
    uint8_t old[21]={1}, latest[21]={2};
    radio_pending_put(&q,old);
    radio_pending_start(&q);
    radio_pending_done(&q,false);
    assert(q.pending && q.data[0]==1); /* idle after a failed final movement */
    radio_pending_start(&q);
    radio_pending_put(&q,latest);
    radio_pending_done(&q,true);
    assert(q.pending && q.data[0]==2); /* ACK belongs to superseded update */
    radio_pending_start(&q);
    radio_pending_done(&q,true);
    assert(!q.pending);
    radio_pending_put(&q,old);
    radio_pending_done(&q,true); /* unrelated control ACK */
    assert(q.pending);
    q.revision=UINT32_MAX;
    radio_pending_start(&q);
    radio_pending_put(&q,latest);
    radio_pending_done(&q,true);
    assert(q.pending); /* revision wrap does not drop replacement */
}

int main(void)
{
    recovery(); beacon_blackout(); latest_update();
    struct radio_gaps g={0};
    unsigned gaps[]={10,49,50,100,200,400,1000,2390};
    for(unsigned i=0;i<sizeof(gaps)/sizeof(gaps[0]);i++) radio_gap_record(&g,gaps[i]);
    assert(g.ge50==6 && g.ge100==5 && g.ge200==4 && g.ge400==3 && g.ge1000==2 && g.max_ms==2390);
    puts("Recovery: bounded holdover, clock drift, grid skips, ACK ownership and gap tails passed");
}
