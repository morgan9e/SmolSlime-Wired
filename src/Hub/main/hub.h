// Shared definitions for the hub firmware.
#pragma once
#include <stdint.h>

#include "sdkconfig.h"
#include "decoder.h"

#define HUB_MAX_TRACKERS CONFIG_HUB_MAX_TRACKERS
#define HUB_SAMPLE_QUEUE_LEN 128

// Counters written by one task each and read by the console code.
typedef struct {
	volatile uint32_t uart_ok;       // frames with valid COBS, length and CRC16
	volatile uint32_t uart_bad;      // malformed COBS, wrong length or CRC16 mismatch
	volatile uint32_t uart_unknown;  // valid frames with an unknown packet type
	volatile uint32_t uart_ovf;      // driver FIFO or ring buffer overflows
	volatile uint32_t uart_line_err; // framing or parity errors
	volatile uint32_t q_drop;        // samples dropped because the queue was full
	volatile uint32_t udp_tx_err;    // sendto() failures
	volatile uint32_t id_overflow;   // packets for ids >= HUB_MAX_TRACKERS
	volatile uint32_t no_addr_drop;  // data for an id whose address is not known yet
} hub_counters_t;

extern hub_counters_t g_hub;
