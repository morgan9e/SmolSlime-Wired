/*
	SlimeVR Code is placed under the MIT license
	Copyright (c) 2026 SlimeVR Contributors

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
// Pairing button: a short press on sw0 enters pairing mode, the next press leaves it.
// Same effect as the "pair" and "exit" console commands, for a receiver with no USB host.
#include "globals.h"
#include "connection/esb.h"

#if CONFIG_PAIRING_BUTTON

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(button, LOG_LEVEL_INF);

#define DEBOUNCE_MS 30

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

static void press_work_fn(struct k_work *work)
{
	if (gpio_pin_get_dt(&button) != 1)
		return; // bounce or released within the debounce time
	if (esb_pairing_active()) {
		LOG_INF("Button: exit pairing");
		esb_finish_pair();
	} else {
		LOG_INF("Button: enter pairing");
		esb_reset_pair();
	}
}

static K_WORK_DELAYABLE_DEFINE(press_work, press_work_fn);

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&press_work, K_MSEC(DEBOUNCE_MS));
}

static int button_init(void)
{
	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("Button GPIO not ready");
		return 0;
	}
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);
	return 0;
}

SYS_INIT(button_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif
