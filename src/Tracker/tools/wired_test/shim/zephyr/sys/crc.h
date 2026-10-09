#pragma once
#include <stdint.h>
#include <stddef.h>
uint32_t crc32_k_4_2_update(uint32_t crc, const uint8_t *const data, const size_t len);
