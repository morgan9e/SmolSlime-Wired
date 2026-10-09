// Settings stored in NVS and changed from the USB console.
#pragma once
#include <stdint.h>

#include "esp_err.h"

typedef struct {
	char ssid[33];
	char pass[65];
	uint32_t server_ip; // network byte order; 0 = discover by broadcast
} hub_config_t;

// Loads settings, falling back to the Kconfig Wi-Fi defaults. Requires nvs_flash_init().
void hub_config_load(hub_config_t *cfg);
esp_err_t hub_config_set_wifi(const char *ssid, const char *pass);
esp_err_t hub_config_set_server(uint32_t server_ip);
