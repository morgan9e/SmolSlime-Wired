// Presents each receiver tracker to the SlimeVR Server as a WiFi tracker (one UDP socket each),
// fills radio gaps by re-sending the last pose, and emits the 1 Hz stats line.
#pragma once
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// server_ip in network byte order; 0 = discover by broadcast.
void slime_start(QueueHandle_t sample_q, uint32_t server_ip);

// Prints a status table to the console. Reads live state without locking; values may be a tick stale.
void slime_print_status(void);
