#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define XIAO_C6_RF_SWITCH_EN 3 // low = RF switch powered
#define XIAO_C6_ANT_SELECT 14  // low = built-in antenna, high = U.FL

void board_init(void)
{
#if CONFIG_HUB_XIAO_C6_RF_SWITCH
	static const char *TAG = "board";
	const gpio_config_t io = {
		.pin_bit_mask = (1ULL << XIAO_C6_RF_SWITCH_EN) | (1ULL << XIAO_C6_ANT_SELECT),
		.mode = GPIO_MODE_OUTPUT,
	};
	ESP_ERROR_CHECK(gpio_config(&io));
	gpio_set_level(XIAO_C6_RF_SWITCH_EN, 0);
#if CONFIG_HUB_XIAO_C6_EXTERNAL_ANTENNA
	gpio_set_level(XIAO_C6_ANT_SELECT, 1);
	ESP_LOGI(TAG, "XIAO ESP32-C6: external antenna");
#else
	gpio_set_level(XIAO_C6_ANT_SELECT, 0);
	ESP_LOGI(TAG, "XIAO ESP32-C6: built-in antenna");
#endif
#endif
}
