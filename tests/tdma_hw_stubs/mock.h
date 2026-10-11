/* SPDX-License-Identifier: MIT
 * Register/allocator mock for scheduler control tests, not an RF timing model.
 */
#ifndef TDMA_HW_MOCK_H
#define TDMA_HW_MOCK_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#define CONFIG_SOC_NRF52840 1
#define CONFIG_ESB_SYS_TIMER_INSTANCE 2
#define CONFIG_NRFX_TIMER1 0
#define CONFIG_ESB_NEVER_DISABLE_TX 0
#define IS_ENABLED(x) (x)
#define BUILD_ASSERT(x, ...) _Static_assert(x, #x)
#define BIT(x) (1u << (x))
#define __DMB() ((void)0)
#define NRFX_SUCCESS 0
#define TIMER_MODE_MODE_Timer 0
#define TIMER_BITMODE_BITMODE_32Bit 3
typedef unsigned nrf_ppi_channel_t;
typedef unsigned nrf_ppi_channel_group_t;
static struct {
 uint32_t TASKS_CAPTURE[4], CC[4], EVENTS_COMPARE[4];
 uint32_t TASKS_STOP, MODE, BITMODE, PRESCALER, SHORTS, INTENCLR, TASKS_CLEAR, TASKS_START;
} mock_timer;
static struct { uint32_t EVENTS_CRCOK, EVENTS_END; } mock_radio;
static struct { struct { uint32_t DIS; } TASKS_CHG[6]; } mock_ppi;
#define NRF_TIMER1 (&mock_timer)
#define NRF_RADIO (&mock_radio)
#define NRF_PPI (&mock_ppi)
static unsigned allocated, group_members, enabled, allocation_calls;
static int fail_allocation = -1;
static uint32_t event_address[20], task_address[20], fork_address[20];
static unsigned irq_lock(void) { return 0; }
static void irq_unlock(unsigned key) { (void)key; }
static int64_t k_uptime_ticks(void) { return 0; }
static int64_t k_ticks_to_us_floor64(int64_t t) { return t; }
static void printk(const char *fmt, ...) { (void)fmt; }
static int nrfx_ppi_channel_alloc(nrf_ppi_channel_t *ch) {
 if ((int)allocation_calls++ == fail_allocation) return 1;
 for (*ch=0; *ch<20; ++*ch) if (!(allocated & BIT(*ch))) { allocated |= BIT(*ch); return 0; }
 return 1;
}
static void nrfx_ppi_channel_free(nrf_ppi_channel_t ch) { allocated &= ~BIT(ch); }
static int nrfx_ppi_group_alloc(nrf_ppi_channel_group_t *g) { *g=0; return 0; }
static void nrf_ppi_channel_endpoint_setup(void *p, unsigned ch, uint32_t e, uint32_t t) {
 (void)p; event_address[ch]=e; task_address[ch]=t;
}
static void nrf_ppi_fork_endpoint_setup(void *p, unsigned ch, uint32_t t) {
 (void)p; fork_address[ch]=t;
}
static void nrf_ppi_channel_include_in_group(void *p, unsigned ch, unsigned g) {
 (void)p; (void)g; group_members |= BIT(ch);
}
static void nrf_ppi_channel_enable(void *p, unsigned ch) { (void)p; enabled |= BIT(ch); }
static void nrf_ppi_group_enable(void *p, unsigned g) { (void)p; (void)g; enabled |= group_members; }
static void nrf_ppi_group_disable(void *p, unsigned g) { (void)p; (void)g; enabled &= ~group_members; }
int esb_start_tx(void);
#endif
