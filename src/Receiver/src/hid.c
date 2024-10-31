
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>

static struct k_work report_send;

static struct tracker_report {
	uint8_t data[16];
} __packed report = {
	.data = {0}
};;

static bool configured;
static const struct device *hdev;
static ATOMIC_DEFINE(hid_ep_in_busy, 1);

#define HID_EP_BUSY_FLAG	0
//#define REPORT_PERIOD		K_SECONDS(5)
#define REPORT_PERIOD		K_MSEC(1) // streaming reports

static void report_event_handler(struct k_timer *dummy);
static K_TIMER_DEFINE(event_timer, report_event_handler, NULL);

static const uint8_t hid_report_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_UNDEFINED),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),
		HID_USAGE(HID_USAGE_GEN_DESKTOP_UNDEFINED),
		HID_REPORT_SIZE(8),
		HID_REPORT_COUNT(64),
		HID_INPUT(0x02),
	HID_END_COLLECTION,
};

uint16_t sent_device_addr = 0;
bool usb_enabled = false;
int64_t last_registration_sent = 0;

static void send_report(struct k_work *work)
{
	if (!usb_enabled) return;
	if (!stored_trackers) return;
	if (report_count == 0 && k_uptime_get() - 100 < last_registration_sent) return; // send registrations only every 100ms
	int ret, wrote;

	last_registration_sent = k_uptime_get();

	if (!atomic_test_and_set_bit(hid_ep_in_busy, HID_EP_BUSY_FLAG)) {
		// TODO: this really sucks, how can i send as much or as little as i want instead??
//		for (int i = report_count; i < 4; i++) memcpy(&reports[sizeof(report) * (report_sent+i)], &reports[sizeof(report) * report_sent], sizeof(report)); // just duplicate first entry a bunch, this will definitely cause problems
		// cycle through devices and send associated address for server to register
		for (int i = report_count; i < 4; i++) {
			packet_device_addr(&reports[sizeof(report) * (report_sent + i)], sent_device_addr);
			sent_device_addr++;
			sent_device_addr %= stored_trackers;
		}
//		ret = hid_int_ep_write(hdev, &reports, sizeof(report) * report_count, &wrote);
		ret = hid_int_ep_write(hdev, &reports[sizeof(report) * report_sent], sizeof(report) * 4, &wrote);
		if (report_count > 4) {
			LOG_INF("left %u reports on the table", report_count - 4);
		}
		report_sent += report_count;
		report_sent += 3; // this is a hack to make sure the ep isnt reading the same bits as trackers write to
		if (report_sent > 128) report_sent = 0; // an attempt to make ringbuffer so the ep isnt reading the same bits as trackers write to
		report_count = 0;
		if (ret != 0) {
			/*
			 * Do nothing and wait until host has reset the device
			 * and hid_ep_in_busy is cleared.
			 */
			LOG_ERR("Failed to submit report");
		} else {
			//LOG_DBG("Report submitted");
		}
	} else { // busy with what
		//LOG_DBG("HID IN endpoint busy");
	}
}

static void int_in_ready_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	if (!atomic_test_and_clear_bit(hid_ep_in_busy, HID_EP_BUSY_FLAG)) {
		LOG_WRN("IN endpoint callback without preceding buffer write");
	}
}

/*
 * On Idle callback is available here as an example even if actual use is
 * very limited. In contrast to report_event_handler(),
 * report value is not incremented here.
 */
static void on_idle_cb(const struct device *dev, uint16_t report_id)
{
	LOG_DBG("On idle callback");
	k_work_submit(&report_send);
}

static void report_event_handler(struct k_timer *dummy)
{
	if (usb_enabled)
		k_work_submit(&report_send);
}

static void protocol_cb(const struct device *dev, uint8_t protocol)
{
	LOG_INF("New protocol: %s", protocol == HID_PROTOCOL_BOOT ?
		"boot" : "report");
}

static const struct hid_ops ops = {
	.int_in_ready = int_in_ready_cb,
	.on_idle = on_idle_cb,
	.protocol_change = protocol_cb,
};

static void status_cb(enum usb_dc_status_code status, const uint8_t *param)
{
	switch (status) {
	case USB_DC_RESET:
		configured = false;
		break;
	case USB_DC_CONFIGURED:
		if (!configured) {
			int_in_ready_cb(hdev);
			configured = true;
		}
		break;
	case USB_DC_SOF:
		break;
	default:
		LOG_DBG("status %u unhandled", status);
		break;
	}
}

static int composite_pre_init()
{
	hdev = device_get_binding("HID_0");
	if (hdev == NULL) {
		LOG_ERR("Cannot get USB HID Device");
		return -ENODEV;
	}

	LOG_INF("HID Device: dev %p", hdev);

	usb_hid_register_device(hdev, hid_report_desc, sizeof(hid_report_desc),
				&ops);

	atomic_set_bit(hid_ep_in_busy, HID_EP_BUSY_FLAG);
	k_timer_start(&event_timer, REPORT_PERIOD, REPORT_PERIOD);

	if (usb_hid_set_proto_code(hdev, HID_BOOT_IFACE_CODE_NONE)) {
		LOG_WRN("Failed to set Protocol Code");
	}

	return usb_hid_init(hdev);
}

SYS_INIT(composite_pre_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

void usb_init_thread(void)
{
	k_msleep(1000);
	usb_disable();
	k_msleep(1000); // Wait before enabling USB // TODO: why does it need to wait so long
	usb_enable(status_cb);
	k_work_init(&report_send, send_report);
	usb_enabled = true;
}

K_THREAD_DEFINE(usb_init_thread_id, 256, usb_init_thread, NULL, NULL, NULL, 6, 0, 0);
