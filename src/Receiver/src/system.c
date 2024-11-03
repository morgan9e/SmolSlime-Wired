#include "globals.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <hal/nrf_gpio.h>

#include "system.h"

static struct nvs_fs fs;

#define NVS_PARTITION		storage_partition
#define NVS_PARTITION_DEVICE	FIXED_PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET	FIXED_PARTITION_OFFSET(NVS_PARTITION)

LOG_MODULE_REGISTER(system, LOG_LEVEL_INF);

#if DT_NODE_HAS_PROP(DT_ALIAS(sw0), gpios) // Alternate button if available to use as "reset key"
#define BUTTON_EXISTS true
static void button_thread(void);
K_THREAD_DEFINE(button_thread_id, 256, button_thread, NULL, NULL, NULL, 6, 0, 0);
#else
#pragma message "Button GPIO does not exist"
#endif

static bool nvs_init = false;

static int sys_nvs_init(void)
{
	if (nvs_init)
		return;
	struct flash_pages_info info;
	fs.flash_device = NVS_PARTITION_DEVICE;
	fs.offset = NVS_PARTITION_OFFSET; // Start NVS FS here
	flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
	fs.sector_size = info.size; // Sector size equal to page size
	fs.sector_count = 4U; // 4 sectors
	nvs_mount(&fs);
	nvs_init = true;
	return 0;
}

SYS_INIT(sys_nvs_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

uint8_t reboot_counter_read(void)
{
	uint8_t reboot_counter
	nvs_read(&fs, RBT_CNT_ID, &reboot_counter, sizeof(reboot_counter));
	return reboot_counter;
}

void reboot_counter_write(uint8_t reboot_counter)
{
	nvs_write(&fs, RBT_CNT_ID, &reboot_counter, sizeof(reboot_counter));
}

// retained not implemented
void sys_write(uint16_t id, void *retained_ptr, const void *data, size_t len)
{
	sys_nvs_init();
	nvs_write(&fs, id, data, len);
}

// reading from nvs
void sys_read(uint16_t id, void *data, size_t len)
{
	sys_nvs_init();
	nvs_read(&fs, id, data, len);
}

#if BUTTON_EXISTS // Alternate button if available to use as "reset key"
static const struct gpio_dt_spec button0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static int64_t press_time;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (button_read())
	{
		press_time = k_uptime_get();
	}
	else
	{
		if (press_time != 0 && k_uptime_get() - press_time > 50) // Debounce
			sys_reboot(SYS_REBOOT_COLD); // treat like pin reset but without pin reset reason
		press_time = 0;
	}
}

static struct gpio_callback button_cb_data;

static int sys_button_init(void)
{
	gpio_pin_configure_dt(&button0, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button0, GPIO_INT_EDGE_BOTH);
	gpio_init_callback(&button_cb_data, button_pressed, BIT(button0.pin));
	gpio_add_callback(button0.port, &button_cb_data);
	return 0;
}

SYS_INIT(sys_button_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif

bool button_read(void)
{
#if BUTTON_EXISTS // Alternate button if available to use as "reset key"
	return gpio_pin_get_dt(&button0);
#else
	return false;
#endif
}

#if BUTTON_EXISTS // Alternate button if available to use as "reset key"
static void button_thread(void)
{
	while (1)
	{
		k_msleep(10);
		if (press_time != 0 && k_uptime_get() - press_time > 50 && button_read()) // Button is being pressed
			sys_reboot(SYS_REBOOT_COLD);
	}
}
#endif
