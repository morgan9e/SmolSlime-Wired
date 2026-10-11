/* SPDX-License-Identifier: MIT */
#include "radio_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct rc_receiver fresh(void)
{
	return (struct rc_receiver){.message={.mode=RC_TDMA, .target=RC_TDMA, .token=50}};
}
static void ack(struct rc_receiver *r, unsigned id)
{
	uint8_t p[RC_LEN];
	rc_reply(p, &r->message, id);
	assert(rc_ack(r, p, sizeof(p)));
}
static void all(struct rc_receiver *r)
{
	for (unsigned id=0; id<8; id++) ack(r, id);
}
static void round_trip(void)
{
	struct rc_receiver r=fresh();
	assert(rc_begin(&r,RC_LEGACY,255,100));
	assert(!rc_begin(&r,RC_LEGACY,255,101));
	all(&r); rc_tick(&r,150);
	assert(r.message.mode==RC_LEGACY && r.message.phase==RC_TRIAL);
	all(&r); rc_tick(&r,200);
	assert(r.result==RC_SUCCESS && r.message.phase==RC_IDLE);
	assert(rc_begin(&r,RC_TDMA,255,300));
	all(&r); rc_tick(&r,350); all(&r); rc_tick(&r,400);
	assert(r.message.mode==RC_TDMA && r.result==RC_SUCCESS);
}
static void missing_and_recovery(void)
{
	struct rc_receiver r=fresh();
	assert(rc_begin(&r,RC_LEGACY,255,0));
	for(unsigned i=0;i<7;i++) ack(&r,i); /* ID7 offline: no switch */
	rc_tick(&r,4999); assert(r.message.mode==RC_TDMA && r.message.phase==RC_PREPARE);
	rc_tick(&r,5000); assert(r.result==RC_MISSING && r.message.mode==RC_TDMA);
	assert(rc_begin(&r,RC_LEGACY,255,6000)); all(&r); rc_tick(&r,6001);
	uint8_t stale[RC_LEN]; rc_reply(stale,&r.message,7);
	for(unsigned i=0;i<7;i++) ack(&r,i); /* one device misses transition */
	rc_tick(&r,11001);
	assert(r.result==RC_ROLLED_BACK && r.message.mode==RC_TDMA);
	assert(!rc_ack(&r,stale,sizeof(stale)));
	/* A device that missed transition/rollback learns authoritative mode from
	 * either the next ACK or a recovery beacon; receiver restart also uses TDMA. */
	uint8_t b[RC_LEN]; struct rc_message m;
	rc_encode(b,&r.message); assert(rc_decode(&m,b,sizeof(b)) && m.mode==RC_TDMA);
	r=fresh(); rc_encode(b,&r.message); assert(rc_decode(&m,b,sizeof(b)) && m.mode==RC_TDMA);
}
static void malformed_and_stale(void)
{
	struct rc_receiver r=fresh(); uint8_t b[RC_LEN], bad[RC_LEN]; struct rc_message m;
	assert(!rc_begin(&r,2,255,0)); assert(!rc_begin(&r,RC_LEGACY,0,0));
	assert(!rc_begin(&r,RC_TDMA,255,0));
	assert(rc_begin(&r,RC_LEGACY,3,0));
	rc_encode(b,&r.message);
	for(size_t n=0;n<RC_LEN;n++) assert(!rc_decode(&m,b,n));
	assert(!rc_decode(&m,b,RC_LEN+1));
	for(unsigned i=0;i<8;i++) {
		memcpy(bad,b,RC_LEN); bad[i]=255;
		/* required mask is arbitrary, all other fields reject this value */
		if(i!=6) assert(!rc_decode(&m,bad,RC_LEN));
	}
	rc_reply(b,&r.message,0);
	for(unsigned i=0;i<RC_LEN;i++) {
		memcpy(bad,b,RC_LEN); bad[i]^=128;
		assert(!rc_ack(&r,bad,RC_LEN));
	}
	rc_reply(b,&r.message,2); assert(!rc_ack(&r,b,RC_LEN)); /* not required */
	rc_reply(b,&r.message,0); assert(rc_ack(&r,b,RC_LEN)); assert(rc_ack(&r,b,RC_LEN));
	rc_tick(&r,1); assert(r.message.phase==RC_PREPARE); /* duplicate isn't another ID */
	rc_cancel(&r); assert(r.result==RC_CANCELLED && r.message.mode==RC_TDMA);
	assert(!rc_ack(&r,b,RC_LEN));
	r.message.token=UINT32_MAX;
	assert(rc_begin(&r,RC_LEGACY,1,2)); assert(r.message.token==0);
	ack(&r,0); rc_tick(&r,3); rc_cancel(&r);
	assert(r.message.mode==RC_TDMA && r.result==RC_CANCELLED);
}
int main(void)
{
	round_trip(); missing_and_recovery(); malformed_and_stale();
	puts("Radio control: both modes, missing devices, rollback, cancel, malformed/stale replies passed");
}
