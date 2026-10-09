// CRC16-CCITT as used on the receiver -> hub UART link.
// Polynomial 0x1021, init 0xFFFF, no reflection, no final XOR ("CRC-16/CCITT-FALSE").
// Check value for "123456789" is 0x29B1.
// On the receiver (Zephyr) this is crc16_itu_t(0xFFFF, data, len).
// Do NOT use Zephyr's crc16_ccitt(), which is the reflected variant.
#pragma once
#include <stddef.h>
#include <stdint.h>

uint16_t hub_crc16(const uint8_t *data, size_t len);
