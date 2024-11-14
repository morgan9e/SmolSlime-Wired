#ifndef SLIMENRF_ESB
#define SLIMENRF_ESB

#include <esb.h>

void event_handler(struct esb_evt const *event);
int clocks_start(void);
int esb_initialize(bool);

void esb_set_addr_discovery(void);
void esb_set_addr_paired(void);

void esb_pair(void);
void esb_write_sync(uint16_t led_clock);
void esb_receive(void);

#endif