/*
	The PC link on the ESP32-S3 (USB OTG, TinyUSB): the same USB device as the nRF receiver
	(Receiver/src/hid.c), so SlimeVR Server and tools/slime_link.py take it as one.

	- VID/PID 1209:7690; a CDC ACM interface (0-1, the text lines of main.c) and a HID interface
	  (2) with the receiver's report descriptor: 64-byte input and output reports, no report ID.
	- An input report holds four 16-byte packets: tracker packets as they came (tracker id at [1],
	  RSSI at [15]), up to one report per millisecond; free slots carry address packets (type 255:
	  id, device address little endian), round robin over the known trackers. Without tracker
	  packets, a report of address packets every 100 ms; nothing before the first tracker.
	- A tracker packet replaces a queued one from the same tracker (not the next one to go out),
	  as the receiver does.
*/
#include "link.h"

#include <string.h>

#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

const bool link_hid = true;

// Receiver/src/hid.c hid_report_desc
static const uint8_t hid_report_desc[] = {
	0x05, 0x01,       // usage page: generic desktop
	0x09, 0x00,       // usage: undefined
	0xA1, 0x01,       // collection: application
	0x09, 0x00,       //   usage: undefined
	0x75, 0x08,       //   report size 8
	0x95, 0x40,       //   report count 64
	0x81, 0x02,       //   input
	0x09, 0x00,       //   usage: undefined
	0x75, 0x08,       //   report size 8
	0x95, 0x40,       //   report count 64
	0x91, 0x02,       //   output
	0xC0,             // end collection
};

static const tusb_desc_device_t device_desc = {
	.bLength = sizeof(tusb_desc_device_t),
	.bDescriptorType = TUSB_DESC_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = TUSB_CLASS_MISC, // composite with interface association, as the receiver
	.bDeviceSubClass = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor = 0x1209,
	.idProduct = 0x7690,
	.bcdDevice = 0x0100,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_HID, ITF_COUNT };
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t config_desc[] = {
	TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
	TUD_CDC_DESCRIPTOR(ITF_CDC, 4, 0x81, 16, 0x01, 0x82, 64),
	TUD_HID_INOUT_DESCRIPTOR(ITF_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_desc), 0x02, 0x83, 64, 1),
};

static char serial[17]; // 16 hex digits as the receiver's: "53" "33" ("S3") and the chip's MAC
static const char *strings[] = {
	(const char[]){0x09, 0x04}, // English
	"SlimeVR",
	"SlimeNRF Receiver ESP32-S3",
	serial,
	"SlimeNRF Receiver console",
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
	return hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
	return 0; // the receiver has none either
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
	// output reports: the receiver takes OTA data there (0xF5), SlimeVR Server sends none
}

// Text lines (CDC)

static SemaphoreHandle_t write_lock;
static StreamBufferHandle_t rx_stream;

void link_write(const char *line, size_t len)
{
	if (!tud_mounted())
		return;
	xSemaphoreTake(write_lock, portMAX_DELAY);
	if (tud_cdc_n_write_available(ITF_CDC) >= len) // whole lines or none
	{
		tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, len);
		tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
	}
	xSemaphoreGive(write_lock);
}

static void cdc_rx(int itf, cdcacm_event_t *event)
{
	uint8_t buf[64];
	size_t n = 0;
	while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) == ESP_OK && n)
		xStreamBufferSend(rx_stream, buf, n, 0);
}

int link_read(uint8_t *buf, size_t len, TickType_t wait)
{
	return xStreamBufferReceive(rx_stream, buf, len, wait);
}

void link_download_mode(void)
{
	REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT); // the ROM stays in download mode (USB Serial/JTAG)
	esp_restart();
}

// HID reports

#define QUEUE_LEN 32
#define MAX_KNOWN 16
#define ADDRESS_PERIOD_MS 100

static portMUX_TYPE hid_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t queue[QUEUE_LEN][16];
static int queue_head, queue_count;
static uint8_t known[MAX_KNOWN][6];
static int known_count;
static uint32_t dropped;

void link_hid_packet(const uint8_t packet[16])
{
	taskENTER_CRITICAL(&hid_lock);
	for (int k = 1; k < queue_count; k++) // not the head: it may be going out
	{
		uint8_t *q = queue[(queue_head + k) % QUEUE_LEN];
		if (q[1] == packet[1])
		{
			memcpy(q, packet, 16);
			taskEXIT_CRITICAL(&hid_lock);
			return;
		}
	}
	if (queue_count < QUEUE_LEN)
		memcpy(queue[(queue_head + queue_count++) % QUEUE_LEN], packet, 16);
	else
		dropped++;
	taskEXIT_CRITICAL(&hid_lock);
}

void link_hid_trackers(const uint8_t (*address)[6], int count)
{
	taskENTER_CRITICAL(&hid_lock);
	known_count = count < MAX_KNOWN ? count : MAX_KNOWN;
	memcpy(known, address, known_count * 6);
	taskEXIT_CRITICAL(&hid_lock);
}

static void hid_task(void *param)
{
	uint8_t report[64];
	int next_address = 0;
	TickType_t last_sent = 0;
	while (1)
	{
		vTaskDelay(1); // 1 ms ticks (CONFIG_FREERTOS_HZ=1000), the host polls the endpoint every 1 ms
		if (!tud_mounted() || !tud_hid_n_ready(0))
			continue;
		int packets = 0;
		taskENTER_CRITICAL(&hid_lock);
		int count = known_count;
		if (count && (queue_count || xTaskGetTickCount() - last_sent >= pdMS_TO_TICKS(ADDRESS_PERIOD_MS)))
		{
			for (; packets < 4 && queue_count; packets++, queue_count--)
			{
				memcpy(&report[16 * packets], queue[queue_head], 16);
				queue_head = (queue_head + 1) % QUEUE_LEN;
			}
			for (int k = packets; k < 4; k++)
			{
				next_address = (next_address + 1) % count;
				uint8_t *p = &report[16 * k];
				p[0] = 255;
				p[1] = next_address;
				memcpy(&p[2], known[next_address], 6);
				memset(&p[8], 0, 8);
			}
			packets = 4;
		}
		taskEXIT_CRITICAL(&hid_lock);
		if (!packets)
			continue;
		last_sent = xTaskGetTickCount();
		tud_hid_n_report(0, 0, report, sizeof(report));
	}
}

void link_init(void)
{
	write_lock = xSemaphoreCreateMutex();
	rx_stream = xStreamBufferCreate(512, 1);

	uint8_t mac[6];
	esp_efuse_mac_get_default(mac);
	snprintf(serial, sizeof(serial), "5333%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
	usb.descriptor.device = &device_desc;
	usb.descriptor.string = strings;
	usb.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
	usb.descriptor.full_speed_config = config_desc;
	ESP_ERROR_CHECK(tinyusb_driver_install(&usb));

	tinyusb_config_cdcacm_t cdc = {
		.cdc_port = TINYUSB_CDC_ACM_0,
		.callback_rx = cdc_rx,
	};
	ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc));

	xTaskCreate(hid_task, "hid", 3072, NULL, 6, NULL);
}
