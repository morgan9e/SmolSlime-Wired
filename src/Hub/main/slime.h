// Presents each receiver tracker to the SlimeVR Server as a WiFi tracker (one UDP socket each).
#pragma once
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// server_ip in network byte order; 0 = discover by broadcast.
void slime_start(QueueHandle_t sample_q, uint32_t server_ip);
