#pragma once
#include <zephyr/kernel.h>
struct sim_ficr { uint32_t DEVICEADDR[2]; };
extern struct sim_ficr sim_ficr;
#define NRF_FICR (&sim_ficr)
// settings: only the wired mode and the older role bit are used by wired.c
#define CONFIG_0_CONNECTION_OVER_UART 6
#define CONFIG_2_WIRED_MODE 10
extern int sim_role_ankle, sim_wired_mode;
#define CONFIG_0_SETTINGS_READ(id) ((id) == CONFIG_0_CONNECTION_OVER_UART ? sim_role_ankle : 0)
#define CONFIG_2_SETTINGS_READ(id) ((id) == CONFIG_2_WIRED_MODE ? sim_wired_mode : 0)
