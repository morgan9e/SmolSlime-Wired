// Slime Hub: reads the SlimeVR nRF receiver's UART mirror and presents each tracker
// to the SlimeVR Server over Wi-Fi as a stock UDP tracker.
#include "board.h"
#include "console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hub.h"
#include "hub_config.h"
#include "net.h"
#include "nvs_flash.h"
#include "slime.h"
#include "uart_rx.h"

void app_main(void)
{
	board_init(); // antenna switch etc., before the radio starts

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	hub_config_t cfg;
	hub_config_load(&cfg);
	net_start(&cfg);

	QueueHandle_t q = xQueueCreate(HUB_SAMPLE_QUEUE_LEN, sizeof(hub_sample_t));
	configASSERT(q);
	slime_start(q, cfg.server_ip);
	uart_rx_start(q);
	console_start();
}
