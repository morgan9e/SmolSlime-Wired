#pragma once
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t esp_get_free_heap_size(void) { return 0; }
static inline void esp_restart(void) { exit(0); }
