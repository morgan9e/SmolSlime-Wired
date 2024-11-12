#ifndef SLIMENRF_GLOBALS
#define SLIMENRF_GLOBALS

#include <zephyr/logging/log.h>

#include "retained.h"

#define FW_NAME "SlimeVR-Tracker-nRF-Receiver"

#define MAX_TRACKERS 256
#define DETECTION_THRESHOLD 16

// TODO: move to esb
extern uint8_t stored_trackers;
extern uint64_t stored_tracker_addr[MAX_TRACKERS];

#endif