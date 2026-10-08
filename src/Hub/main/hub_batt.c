#include "hub_batt.h"

#include "sdkconfig.h"

#if CONFIG_HUB_BATT_ADC

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "batt";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static bool s_ok;

void hub_batt_init(void)
{
	adc_unit_t unit;
	if (adc_oneshot_io_to_channel(CONFIG_HUB_BATT_ADC_GPIO, &unit, &s_chan) != ESP_OK) {
		ESP_LOGE(TAG, "GPIO%d is not an ADC pin", CONFIG_HUB_BATT_ADC_GPIO);
		return;
	}
	const adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
	if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK)
		return;
	const adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
	if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK)
		return;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	const adc_cali_curve_fitting_config_t cal = {
		.unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
	if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK)
		return;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
	const adc_cali_line_fitting_config_t cal = {
		.unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
	if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) != ESP_OK)
		return;
#endif
	s_ok = true;
}

float hub_batt_volts(void)
{
	if (!s_ok)
		return -1.0f;
	int sum_mv = 0;
	for (int i = 0; i < 8; i++) {
		int raw, mv;
		if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK)
			return -1.0f;
		sum_mv += mv;
	}
	return (float)sum_mv / 8.0f / 1000.0f * (float)CONFIG_HUB_BATT_DIVIDER_X1000 / 1000.0f;
}

#else

void hub_batt_init(void)
{
}

float hub_batt_volts(void)
{
	return -1.0f;
}

#endif
