// The PC link on the ESP32-C6: its USB Serial/JTAG port, text lines only (tools/ble_bridge.py
// --serial forwards the trackers to SlimeVR). Opening or closing the port with DTR/RTS changes
// can reset the chip: ble_bridge.open_receiver keeps both off.
#include "link.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "freertos/semphr.h"

const bool link_hid = false;
static SemaphoreHandle_t write_lock;

void link_init(void)
{
	write_lock = xSemaphoreCreateMutex();
	usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 8192, .rx_buffer_size = 512};
	ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
	usb_serial_jtag_vfs_use_driver(); // ESP_LOG through the driver too
}

void link_write(const char *line, size_t len)
{
	if (!usb_serial_jtag_is_connected())
		return;
	xSemaphoreTake(write_lock, portMAX_DELAY);
	usb_serial_jtag_write_bytes(line, len, 0);
	xSemaphoreGive(write_lock);
}

int link_read(uint8_t *buf, size_t len, TickType_t wait)
{
	return usb_serial_jtag_read_bytes(buf, len, wait);
}

void link_download_mode(void)
{
	// esptool resets this chip into its download mode over USB Serial/JTAG itself
}

void link_hid_packet(const uint8_t packet[16])
{
}

void link_hid_trackers(const uint8_t (*address)[6], int count)
{
}
