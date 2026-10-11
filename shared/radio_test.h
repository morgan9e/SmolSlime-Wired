/* SPDX-License-Identifier: MIT */
#ifndef SLIME_RADIO_TEST_H
#define SLIME_RADIO_TEST_H
#include <stdint.h>
#include <stdbool.h>
#define RFT_TYPE 0xe1
#define RFT_VERSION 1
#define RFT_CONFIRMED 0x08
#define RFT_IMU_WORKLOAD 0x10
/* 21 bytes: type,id,version,kind|flags,generated32,value32,value24,RSSI,CRC32,seq8.
 * Kind 0: uptime us, PRNG; 1: generator skips, TDMA skips;
 * 2: submitted, replaced; 3: acknowledged, failed; 4: API errors, target Hz.
 * Values are cumulative, the 24-bit values wrap. Kind snapshots arrive separately.
 */
static inline void rft_put(uint8_t *p, uint32_t v, unsigned n)
{ for (unsigned i=0;i<n;i++) p[i]=(uint8_t)(v>>(8*i)); }
static inline uint32_t rft_get(const uint8_t *p, unsigned n)
{ uint32_t v=0;for(unsigned i=0;i<n;i++) v|=(uint32_t)p[i]<<(8*i);return v; }
static inline bool rft_due(int64_t now, int64_t *next, uint32_t period, uint32_t *skipped)
{
    if (now < *next) return false;
    uint32_t missed=(uint32_t)((now-*next)/period);
    *skipped+=missed;
    *next+=(int64_t)(missed+1)*period;
    return true; /* newest deadline only; never send catch-up bursts */
}
static inline void rft_encode(uint8_t *p,uint8_t id,uint8_t kind,uint8_t flags,
                              uint32_t generated,uint32_t a,uint32_t b)
{
    p[0]=RFT_TYPE;p[1]=id;p[2]=RFT_VERSION;p[3]=kind|flags;
    rft_put(p+4,generated,4);rft_put(p+8,a,4);rft_put(p+12,b,3);p[15]=0;
}
static inline bool rft_valid(const uint8_t *p)
{ return p[0]==RFT_TYPE && p[1]<8 && p[2]==RFT_VERSION && (p[3]&7)<5 && !(p[3]&0xe0); }
#endif
