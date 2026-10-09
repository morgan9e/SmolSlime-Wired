#pragma once
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY 0xFFFFFFFFu
#define configASSERT(x) do { if (!(x)) abort(); } while (0)
#define portNUM_PROCESSORS 1
