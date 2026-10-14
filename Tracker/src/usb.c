#include "globals.h"
#include "console.h"
#include "hid.h"
#include "system/status.h"
#include "system/power.h"

#if CONFIG_USB_DEVICE_STACK
#define USB DT_NODELABEL(usbd)
#define USB_EXISTS (DT_NODE_HAS_STATUS(USB, okay) && CONFIG_UART_CONSOLE)
#endif

#if USB_EXISTS
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>

static bool configured;

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

static void usb_init_thread(void);
K_THREAD_DEFINE(usb_init_thread_id, 512, usb_init_thread, NULL, NULL, NULL, USB_INIT_THREAD_PRIORITY, 0, 500); // Wait before enabling USB

static void status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	const struct log_backend *backend = log_backend_get_by_name("log_backend_uart");
	const struct device *const cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	bool use_hid = CONFIG_0_SETTINGS_READ(CONFIG_0_CONNECTION_OVER_HID);
	switch (status)
	{
	case USB_DC_RESET:
		configured = false;
		break;
	case USB_DC_CONNECTED:
		set_status(SYS_STATUS_USB_CONNECTED, true);
		pm_device_action_run(cons, PM_DEVICE_ACTION_RESUME);
		log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_MAX_LEVEL);
		console_thread_create();
		if (use_hid)
			hid_thread_create();
		break;
	case USB_DC_CONFIGURED:
		int configurationIndex = *param;
		if (configurationIndex == 0)
		{
			// from usb_device.c: A configuration index of 0 unconfigures the device.
			configured = false;
		}
		else
		{
			if (!configured)
			{
				hid_int_in_ready();
				configured = true;
			}
		}
		break;
	case USB_DC_DISCONNECTED:
		set_status(SYS_STATUS_USB_CONNECTED, false);
		if (use_hid)
			hid_thread_abort();
		console_thread_abort();
		log_backend_disable(backend);
		pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
		break;
	case USB_DC_SOF:
		break;
	default:
		LOG_DBG("status %u unhandled", status);
		break;
	}
}

// USB self-check: if USB power is present but the host has not configured the device within
// USB_WATCHDOG_MS, reboot, up to USB_WATCHDOG_TRIES times per plug-in. A stalled USB stack
// recovers only by a reboot, and the tracker may be out of reach otherwise (no button, remote PC).
// On a plain charger (power, no host) this costs a few reboots once, then it stops.
#define USB_WATCHDOG_MS 15000
#define USB_WATCHDOG_TRIES 3
#define USB_WATCHDOG_MAGIC 0x55B0D06Eu

static __noinit struct {
	uint32_t magic;
	uint32_t tries;
} usb_watchdog; // kept over a reboot

static int64_t vbus_since;

static void usb_watchdog_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(usb_watchdog_work, usb_watchdog_handler);

static void usb_watchdog_handler(struct k_work *work)
{
	bool vbus = NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk;
	if (usb_watchdog.magic != USB_WATCHDOG_MAGIC)
	{
		usb_watchdog.magic = USB_WATCHDOG_MAGIC;
		usb_watchdog.tries = 0;
	}
	if (!vbus || configured)
	{
		vbus_since = 0;
		usb_watchdog.tries = 0;
	}
	else if (!vbus_since)
	{
		vbus_since = k_uptime_get();
	}
	else if (k_uptime_get() - vbus_since > USB_WATCHDOG_MS && usb_watchdog.tries < USB_WATCHDOG_TRIES)
	{
		usb_watchdog.tries++;
		LOG_WRN("USB powered but not set up by the host in %d s, rebooting (%u/%d)", USB_WATCHDOG_MS / 1000, usb_watchdog.tries, USB_WATCHDOG_TRIES);
		sys_request_system_reboot(false);
		return;
	}
	k_work_schedule(&usb_watchdog_work, K_SECONDS(1));
}

static void usb_init_thread(void)
{
	usb_enable(status_cb);
	k_work_schedule(&usb_watchdog_work, K_SECONDS(1));
}

#endif