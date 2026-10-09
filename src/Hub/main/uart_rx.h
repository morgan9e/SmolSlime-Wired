// Reads the receiver's mirror stream: COBS([16-byte packet][CRC16 LE]) + 0x00 at 1 Mbaud.
// Valid packets are decoded and pushed to the sample queue.
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void uart_rx_start(QueueHandle_t sample_q);
