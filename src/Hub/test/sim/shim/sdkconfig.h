// Host simulation values for the hub's Kconfig options.
#pragma once
#define CONFIG_HUB_MAX_TRACKERS 16
#define CONFIG_HUB_SERVER_PORT 6969
#define CONFIG_HUB_LOCAL_PORT_BASE 6980
#define CONFIG_HUB_REPORT_BOARD 4
#define CONFIG_HUB_SERVER_TIMEOUT_MS 5000
#ifndef CONFIG_HUB_DROP_MS
#define CONFIG_HUB_DROP_MS 30000
#endif
#define CONFIG_HUB_GAP_COUNT_MS 20
#define CONFIG_HUB_LED_GPIO -1
