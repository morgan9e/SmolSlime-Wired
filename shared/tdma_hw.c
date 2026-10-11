/* SPDX-License-Identifier: MIT
 * nRF52840 TIMER1/PPI scheduling. ESB retains ownership of RADIO, its EGU,
 * protocol timer and ACK state machine. No application radio IRQ handler.
 */
#include "tdma_hw.h"
#include <zephyr/kernel.h>
#include <nrfx_ppi.h>
#include <esb.h>
#include <errno.h>

BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF52840));
BUILD_ASSERT(CONFIG_ESB_SYS_TIMER_INSTANCE != 1);
BUILD_ASSERT(!IS_ENABLED(CONFIG_NRFX_TIMER1), "TIMER1 reserved by TDMA hardware scheduler");
BUILD_ASSERT(!IS_ENABLED(CONFIG_ESB_NEVER_DISABLE_TX));

static bool initialized, requested, armed, rejected;
static nrf_ppi_channel_t rx_capture, tx_capture, start_channel;
static nrf_ppi_channel_group_t start_group;
static int64_t clock_us, requested_time;
static uint32_t previous, scheduled, fired, cancelled, too_late;
static int32_t min_lead = INT32_MAX;

/* Caller excludes interrupts so extension and software CAPTURE cannot race. */
static int64_t now_locked(void)
{
	NRF_TIMER1->TASKS_CAPTURE[2] = 1;
	uint32_t now = NRF_TIMER1->CC[2];
	clock_us += (uint32_t)(now - previous);
	previous = now;
	return clock_us;
}

int64_t tdma_hw_now(void)
{
	if (!initialized) return k_ticks_to_us_floor64(k_uptime_ticks());
	unsigned key = irq_lock();
	int64_t now = now_locked();
	irq_unlock(key);
	return now;
}

static int64_t captured_time(unsigned channel)
{
	unsigned key = irq_lock();
	int64_t now = now_locked();
	int64_t captured = now - (uint32_t)(previous - NRF_TIMER1->CC[channel]);
	irq_unlock(key);
	return captured;
}

int64_t tdma_hw_rx_time(void) { return captured_time(0); }
int64_t tdma_hw_tx_time(void) { return captured_time(3); }

int tdma_hw_init(void)
{
	if (initialized) return 0;
	if (nrfx_ppi_channel_alloc(&rx_capture) != NRFX_SUCCESS) return -ENOMEM;
	if (nrfx_ppi_channel_alloc(&tx_capture) != NRFX_SUCCESS) goto fail_rx;
	if (nrfx_ppi_channel_alloc(&start_channel) != NRFX_SUCCESS) goto fail_tx;
	if (nrfx_ppi_group_alloc(&start_group) != NRFX_SUCCESS) goto fail_start;
	NRF_TIMER1->TASKS_STOP = 1;
	NRF_TIMER1->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER1->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
	NRF_TIMER1->PRESCALER = 4; /* 16 MHz / 16 = 1 MHz */
	NRF_TIMER1->SHORTS = 0;
	NRF_TIMER1->INTENCLR = UINT32_MAX;
	NRF_TIMER1->TASKS_CLEAR = 1;
	previous = 0;
	clock_us = k_ticks_to_us_floor64(k_uptime_ticks());
	/* CRCOK occurs only for reception, so a subsequent transmitted ACK cannot
	 * overwrite the data/beacon timestamp before the ESB callback consumes it. */
	nrfx_ppi_channel_assign(rx_capture, (uint32_t)&NRF_RADIO->EVENTS_CRCOK,
		(uint32_t)&NRF_TIMER1->TASKS_CAPTURE[0]);
	nrfx_ppi_channel_assign(tx_capture, (uint32_t)&NRF_RADIO->EVENTS_END,
		(uint32_t)&NRF_TIMER1->TASKS_CAPTURE[3]);
	nrfx_ppi_channels_include_in_group(BIT(start_channel), start_group);
	NRF_TIMER1->TASKS_START = 1;
	initialized = true;
	nrfx_ppi_channel_enable(rx_capture);
	nrfx_ppi_channel_enable(tx_capture);
	return 0;
fail_start:
	nrfx_ppi_channel_free(start_channel);
fail_tx:
	nrfx_ppi_channel_free(tx_capture);
fail_rx:
	nrfx_ppi_channel_free(rx_capture);
	return -ENOMEM;
}

/* Caller must cancel BEFORE disabling/reinitializing ESB or yielding to OTA. */
void tdma_hw_cancel(void)
{
	if (!initialized) return;
	unsigned key = irq_lock();
	nrfx_ppi_group_disable(start_group);
	if (armed) {
		if (NRF_TIMER1->EVENTS_COMPARE[1]) fired++;
		else cancelled++;
	}
	armed = requested = false;
	NRF_TIMER1->EVENTS_COMPARE[1] = 0;
	irq_unlock(key);
}

bool tdma_hw_defer_start(uint32_t task)
{
	if (!requested) return false; /* ordinary pairing, ACK/RX or OTA */
	requested = false;
	int64_t now = now_locked(); /* tdma_hw_start_tx_at holds irq_lock */
	int64_t lead = requested_time - now;
	/* Never start immediately if setup missed the deadline. The caller resets
	 * the prepared ESB transaction instead, preventing an out-of-slot TX. */
	if (lead < 80 || lead > 1000000) {
		rejected = true;
		too_late++;
		return true;
	}
	if (lead < min_lead) min_lead = lead;
	NRF_TIMER1->CC[1] = previous + (uint32_t)lead;
	NRF_TIMER1->EVENTS_COMPARE[1] = 0;
	nrfx_ppi_channel_assign(start_channel, (uint32_t)&NRF_TIMER1->EVENTS_COMPARE[1], task);
	/* One-shot in hardware: the same compare disables its own PPI group. */
	nrfx_ppi_channel_fork_assign(start_channel,
		(uint32_t)&NRF_PPI->TASKS_CHG[start_group].DIS);
	__DMB();
	armed = true;
	scheduled++;
	nrfx_ppi_group_enable(start_group);
	return true;
}

int tdma_hw_start_tx_at(int64_t deadline)
{
	if (!initialized) return -ENODEV;
	unsigned key = irq_lock();
	if (armed) { irq_unlock(key); return -EBUSY; }
	requested_time = deadline;
	requested = true;
	rejected = false;
	int err = esb_start_tx(); /* prepares ESB, adapter defers its EGU trigger */
	if (!err && (requested || rejected)) err = -ETIME;
	requested = false;
	irq_unlock(key);
	return err;
}

void tdma_hw_status(void)
{
	unsigned key = irq_lock();
	uint32_t f = fired + (armed && NRF_TIMER1->EVENTS_COMPARE[1] ? 1 : 0);
	uint32_t s = scheduled, c = cancelled, l = too_late;
	int32_t lead = min_lead == INT32_MAX ? 0 : min_lead;
	irq_unlock(key);
	printk("tdma hw: timer=1 scheduled=%u fired=%u cancelled=%u rejected=%u min_lead_us=%d\n",
		s, f, c, l, lead);
}
