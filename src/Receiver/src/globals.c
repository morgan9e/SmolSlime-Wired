// TODO: GET RID OF THIS!!!
#include <stdint.h>
#include "globals.h"

uint8_t stored_trackers = 0;
uint64_t stored_tracker_addr[MAX_TRACKERS] = {0};

uint8_t discovered_trackers[256] = {0};

uint8_t pairing_buf[8] = {0};

uint8_t reports[256*sizeof(report)];
uint8_t report_count = 0;
uint8_t report_sent = 0;

int blink = 0;
