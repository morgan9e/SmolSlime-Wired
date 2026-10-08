// Wi-Fi station with power saving off and reconnect backoff.
#pragma once
#include <stdbool.h>

#include "hub_config.h"

void net_start(const hub_config_t *cfg);
bool net_is_up(void);
