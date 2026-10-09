// Presents each receiver tracker to the SlimeVR Server as a WiFi tracker (one UDP socket each).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// server_ip in network byte order; 0 = discover by broadcast.
void slime_start(QueueHandle_t sample_q, uint32_t server_ip);

// Sends a receiver diagnostic packet unchanged to UDP 6971 on the server's PC, once the
// server is known. Safe to call from the UART task.
void slime_forward_diag(const uint8_t *pkt, size_t len);
