#include <assert.h>
#include <stdio.h>
#include "radio_test.h"
int main(void)
{
    int64_t next=10000;uint32_t skipped=0;
    assert(!rft_due(9999,&next,10000,&skipped));
    assert(rft_due(10000,&next,10000,&skipped) && next==20000 && skipped==0);
    assert(rft_due(45999,&next,10000,&skipped) && next==50000 && skipped==2);
    assert(!rft_due(45999,&next,10000,&skipped)); /* no catch-up burst */
    next=UINT32_MAX;
    assert(rft_due((int64_t)UINT32_MAX+30001,&next,10000,&skipped));
    assert(next==(int64_t)UINT32_MAX+40000 && skipped==5);
    uint8_t p[21]={0};
    rft_encode(p,7,3,RFT_CONFIRMED,0x87654321,0xfedcba98,0x12ffffff);
    assert(rft_valid(p) && rft_get(p+4,4)==0x87654321 && rft_get(p+8,4)==0xfedcba98);
    assert(rft_get(p+12,3)==0xffffff && p[15]==0);
    p[1]=8;assert(!rft_valid(p));p[1]=0;
    p[2]=2;assert(!rft_valid(p));p[2]=1;
    p[3]=7;assert(rft_valid(p));p[3]=0x80;assert(!rft_valid(p));
    puts("Radio test: deadlines, no bursts, long uptime, wire format and invalid packets passed");
}
