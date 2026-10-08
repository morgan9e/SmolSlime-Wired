// Reads the wired tracker chains (see wired_link.h): one UART per chain. Trackers get hub ids
// in the order they first appear; their data is decoded and pushed to the sample queue like the
// receiver's, with an address sample per id so slime.c needs no changes.
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void wired_rx_start(QueueHandle_t sample_q);

// Prints the chains and the trackers seen on them (console "trackers").
void wired_rx_print(void);
