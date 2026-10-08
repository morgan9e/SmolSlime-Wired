#include "hub_config.h"

#include <string.h>

#include "nvs.h"
#include "sdkconfig.h"

#define NS "hub"

void hub_config_load(hub_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	strlcpy(cfg->ssid, CONFIG_HUB_WIFI_SSID, sizeof(cfg->ssid));
	strlcpy(cfg->pass, CONFIG_HUB_WIFI_PASSWORD, sizeof(cfg->pass));

	nvs_handle_t h;
	if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
		return;
	size_t len = sizeof(cfg->ssid);
	char ssid[sizeof(cfg->ssid)];
	if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0]) {
		strlcpy(cfg->ssid, ssid, sizeof(cfg->ssid));
		len = sizeof(cfg->pass);
		if (nvs_get_str(h, "pass", cfg->pass, &len) != ESP_OK)
			cfg->pass[0] = 0;
	}
	uint32_t ip;
	if (nvs_get_u32(h, "server", &ip) == ESP_OK)
		cfg->server_ip = ip;
	nvs_close(h);
}

esp_err_t hub_config_set_wifi(const char *ssid, const char *pass)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
	if (err != ESP_OK)
		return err;
	err = nvs_set_str(h, "ssid", ssid);
	if (err == ESP_OK)
		err = nvs_set_str(h, "pass", pass ? pass : "");
	if (err == ESP_OK)
		err = nvs_commit(h);
	nvs_close(h);
	return err;
}

esp_err_t hub_config_set_server(uint32_t server_ip)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
	if (err != ESP_OK)
		return err;
	err = nvs_set_u32(h, "server", server_ip);
	if (err == ESP_OK)
		err = nvs_commit(h);
	nvs_close(h);
	return err;
}
