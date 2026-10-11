/* SPDX-License-Identifier: MIT */
#ifndef SLIME_TDMA_HW_H
#define SLIME_TDMA_HW_H
#include <stdint.h>
#include <stdbool.h>

int tdma_hw_init(void);
int64_t tdma_hw_now(void);
int64_t tdma_hw_rx_time(void);
int64_t tdma_hw_tx_time(void);
int tdma_hw_start_tx_at(int64_t deadline);
void tdma_hw_cancel(void);
void tdma_hw_status(void);
/* SDK adapter: defer only the explicitly scheduled initial transaction. */
bool tdma_hw_defer_start(uint32_t task);
#endif
