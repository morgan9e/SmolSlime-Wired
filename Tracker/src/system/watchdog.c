/*
	Hardware watchdog: resets the tracker when its threads stop running

	Seen on three trackers in BLE mode (2026-10-15), after hours on a draining battery: interrupts
	still ran (the USB console echoed input) but no thread below some high priority got the CPU
	again: no packets, no console answers. The BLE link stayed up (the controller runs in
	interrupts of its own), so the receiver kept a dead tracker connected, and nothing recovered.

	The feed comes from a thread at the lowest preemptible priority, below every other thread:
	whatever keeps it from running for WATCHDOG_TIMEOUT_MS resets the chip. Just before the reset
	(the nRF WDT's interrupt gives two 32 kHz ticks) the thread that was running is noted in RAM
	that survives the reset; the next boot logs it, and `info` shows it (watchdog_print).
*/
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_power.h>

#include "watchdog.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

#define WATCHDOG_TIMEOUT_MS 8000
#define WATCHDOG_FEED_MS 1000
#define WATCHDOG_MAGIC 0x57A7C4D0u

// What was running when the watchdog fired, for the next boot (noinit: kept through the reset)
static __noinit struct {
	uint32_t magic;
	uint32_t uptime_s;
	int priority;
	char thread[24];
} crumb;

static bool reset_by_watchdog; // this boot
static char last[64]; // what `info` shows about it

static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int channel = -1;

// Interrupt context, two 32 kHz ticks before the reset
static void expired(const struct device *dev, int ch)
{
	k_tid_t t = k_current_get();
	const char *name = k_thread_name_get(t);
	crumb.magic = WATCHDOG_MAGIC;
	crumb.uptime_s = k_uptime_get() / 1000;
	crumb.priority = k_thread_priority_get(t);
	strncpy(crumb.thread, name ? name : "?", sizeof(crumb.thread) - 1);
	crumb.thread[sizeof(crumb.thread) - 1] = 0;
}

static int watchdog_init(void)
{
	// RESETREAS is cleared later in main(); the watchdog bit is read here first
	reset_by_watchdog = nrf_power_resetreas_get(NRF_POWER) & NRF_POWER_RESETREAS_DOG_MASK;
	if (reset_by_watchdog && crumb.magic == WATCHDOG_MAGIC)
		snprintk(last, sizeof(last), "after %us, thread %s (priority %d) kept the others from running",
			 crumb.uptime_s, crumb.thread, crumb.priority);
	else if (reset_by_watchdog)
		snprintk(last, sizeof(last), "no record of the thread");
	crumb.magic = 0;

	if (!device_is_ready(wdt))
		return 0;
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0, .max = WATCHDOG_TIMEOUT_MS},
		.callback = expired,
		.flags = WDT_FLAG_RESET_SOC,
	};
	channel = wdt_install_timeout(wdt, &cfg);
	if (channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG))
		channel = -1;
	return 0;
}

SYS_INIT(watchdog_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void watchdog_print(void)
{
	printk("Watchdog: %s", channel >= 0 ? "on" : "off");
	if (reset_by_watchdog)
		printk(", this boot came from a watchdog reset: %s", last);
	printk("\n");
}

static void watchdog_thread(void)
{
	if (reset_by_watchdog)
		LOG_WRN("Restarted by the watchdog: %s", last);
	if (channel < 0)
	{
		LOG_ERR("Watchdog not available");
		return;
	}
	while (1)
	{
		wdt_feed(wdt, channel);
		k_msleep(WATCHDOG_FEED_MS);
	}
}

K_THREAD_DEFINE(watchdog_thread_id, 512, watchdog_thread, NULL, NULL, NULL, CONFIG_NUM_PREEMPT_PRIORITIES - 1, 0, 0);
