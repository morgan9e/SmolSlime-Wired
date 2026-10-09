#pragma once
#include "FreeRTOS.h"
typedef struct sim_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned len, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
