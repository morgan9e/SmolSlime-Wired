#include "net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define BACKOFF_MIN_MS 500
#define BACKOFF_MAX_MS 10000

static volatile bool s_up;
static bool s_have_ssid;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
	esp_wifi_connect();
}

static void schedule_retry(void)
{
	esp_timer_stop(s_retry_timer); // not running is fine
	esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000);
	s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		if (s_have_ssid)
			esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
		// Power saving adds tens of ms of latency per packet; keep it off.
		esp_wifi_set_ps(WIFI_PS_NONE);
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		const wifi_event_sta_disconnected_t *d = data;
		s_up = false;
		ESP_LOGW(TAG, "disconnected, reason %d, retry in %lu ms", d->reason, (unsigned long)s_backoff_ms);
		if (s_have_ssid)
			schedule_retry();
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *e = data;
		ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
		s_backoff_ms = BACKOFF_MIN_MS;
		s_up = true;
	}
}

void net_start(const hub_config_t *cfg)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t *netif = esp_netif_create_default_wifi_sta();
	esp_netif_set_hostname(netif, "slime-hub");

	const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
	ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

	wifi_config_t wc = {0};
	strlcpy((char *)wc.sta.ssid, cfg->ssid, sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, cfg->pass, sizeof(wc.sta.password));
	wc.sta.threshold.authmode = cfg->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
	wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
	wc.sta.pmf_cfg.capable = true;
	s_have_ssid = cfg->ssid[0] != 0;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
	ESP_ERROR_CHECK(esp_wifi_start());
	esp_wifi_set_ps(WIFI_PS_NONE);
#if CONFIG_HUB_WIFI_MAX_TX_POWER_DBM > 0
	esp_wifi_set_max_tx_power(CONFIG_HUB_WIFI_MAX_TX_POWER_DBM * 4);
#endif
	if (s_have_ssid)
		ESP_LOGI(TAG, "connecting to \"%s\"", cfg->ssid);
	else
		ESP_LOGW(TAG, "no Wi-Fi configured; use the console: wifi <ssid> <password>");
}

bool net_is_up(void)
{
	return s_up;
}

int net_rssi(void)
{
	wifi_ap_record_t ap;
	if (!s_up || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
		return 0;
	return ap.rssi;
}
