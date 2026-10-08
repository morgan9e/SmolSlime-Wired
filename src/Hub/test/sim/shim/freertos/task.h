#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
				   unsigned prio, TaskHandle_t *handle, int core);
void vTaskDelay(TickType_t ticks);
