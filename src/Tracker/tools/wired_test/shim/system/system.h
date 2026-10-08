#pragma once
#include <stddef.h>
#include <stdint.h>
#define WIRED_ANKLES_ID 32
void sys_write(uint16_t id, void *ptr, const void *data, size_t len);
void sys_read(uint16_t id, void *data, size_t len);
