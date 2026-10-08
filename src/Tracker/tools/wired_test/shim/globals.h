#pragma once
#include <zephyr/kernel.h>
struct sim_ficr { uint32_t DEVICEADDR[2]; };
extern struct sim_ficr sim_ficr;
#define NRF_FICR (&sim_ficr)
// settings: only the role bit is used by wired.c
#define CONFIG_0_CONNECTION_OVER_UART 6
extern int sim_role_ankle;
#define CONFIG_0_SETTINGS_READ(id) ((id) == CONFIG_0_CONNECTION_OVER_UART ? sim_role_ankle : 0)
