#ifndef SLIMENRF_GLOBALS
#define SLIMENRF_GLOBALS

#include <zephyr/logging/log.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define MAX_TRACKERS 256
#define DETECTION_THRESHOLD 16

extern uint8_t stored_trackers;
extern uint64_t stored_tracker_addr[MAX_TRACKERS];

extern uint8_t discovered_trackers[256];

extern uint8_t pairing_buf[8];

extern uint8_t reports[256*sizeof(report)];
extern uint8_t report_count;
extern uint8_t report_sent;

extern int blink ;

#endif