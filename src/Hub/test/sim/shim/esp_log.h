#pragma once
#include <stdio.h>
#include "esp_timer.h"
#define SIM_LOG(l, tag, fmt, ...) fprintf(stderr, "%c (%lld) %s: " fmt "\n", l, (long long)(esp_timer_get_time() / 1000), tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) SIM_LOG('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) SIM_LOG('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) SIM_LOG('I', tag, fmt, ##__VA_ARGS__)
