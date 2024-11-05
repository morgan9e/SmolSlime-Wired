#ifndef SLIMENRF_GLOBALS
#define SLIMENRF_GLOBALS

#include "retained.h"

#include <zephyr/logging/log.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define MAX_TRACKERS 256
#define DETECTION_THRESHOLD 16

// TODO: move to esb
extern uint8_t stored_trackers;
extern uint64_t stored_tracker_addr[MAX_TRACKERS];

#endif