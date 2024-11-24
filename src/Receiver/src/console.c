#include "globals.h"
#include "system.h"
#include "build_defines.h"

#define USB DT_NODELABEL(usbd)
#if DT_NODE_HAS_STATUS(USB, okay)

#include <zephyr/console/console.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log_ctrl.h>

#include <ctype.h>

#define DFU_DBL_RESET_MEM 0x20007F7C
#define DFU_DBL_RESET_APP 0x4ee5677e

uint32_t* dbl_reset_mem = ((uint32_t*) DFU_DBL_RESET_MEM);

LOG_MODULE_REGISTER(console, LOG_LEVEL_INF);

static void console_thread(void);
K_THREAD_DEFINE(console_thread_id, 1024, console_thread, NULL, NULL, NULL, 6, 0, 0);

#define DFU_EXISTS CONFIG_BUILD_OUTPUT_UF2

static void skip_dfu(void)
{
#if DFU_EXISTS // Using Adafruit bootloader
	(*dbl_reset_mem) = DFU_DBL_RESET_APP; // Skip DFU
	ram_range_retain(dbl_reset_mem, sizeof(dbl_reset_mem), true);
#endif
}

static void print_info(void)
{
	printk(CONFIG_USB_DEVICE_MANUFACTURER " " CONFIG_USB_DEVICE_PRODUCT "\n");
	printk(FW_STRING);

	printk("Board configuration: " CONFIG_BOARD "\n");
	printk("SOC: " CONFIG_SOC "\n");

	printk("Device address: %012llX\n", *(uint64_t *)NRF_FICR->DEVICEADDR & 0xFFFFFFFFFFFF);
}

static void print_list(void)
{
	printk("Stored devices:\n");
	for (uint8_t i = 0; i < stored_trackers; i++)
		printk("%012llX\n", stored_tracker_addr[i]);
}

static void console_thread(void)
{
	console_getline_init();
	while (log_data_pending())
		k_usleep(1);
	k_msleep(100);
	printk("*** " CONFIG_USB_DEVICE_MANUFACTURER " " CONFIG_USB_DEVICE_PRODUCT " ***\n");
	printk(FW_STRING);
	printk("info                         Get device information\n");
	printk("list                         Get paired devices\n");
	printk("reboot                       Soft reset the device\n");
	printk("pair                         Enter pairing mode\n");
	printk("clear                        Clear stored devices\n");

	uint8_t command_info[] = "info";
	uint8_t command_list[] = "list";
	uint8_t command_reboot[] = "reboot";
	uint8_t command_pair[] = "pair";
	uint8_t command_clear[] = "clear";

#if DFU_EXISTS
	printk("dfu                          Enter DFU bootloader\n");

	uint8_t command_dfu[] = "dfu";
#endif

	while (1) {
		uint8_t *line = console_getline();
		for (uint8_t *p = line; *p; ++p) {
			*p = tolower(*p);
		}

		if (memcmp(line, command_info, sizeof(command_info)) == 0)
		{
			print_info();
		}
		else if (memcmp(line, command_list, sizeof(command_list)) == 0)
		{
			print_list();
		}
		else if (memcmp(line, command_reboot, sizeof(command_reboot)) == 0)
		{
			skip_dfu();
			sys_reboot(SYS_REBOOT_COLD);
		}
		else if (memcmp(line, command_pair, sizeof(command_pair)) == 0)
		{
			skip_dfu();
			reboot_counter_write(101);
			k_msleep(1);
			sys_reboot(SYS_REBOOT_WARM);
		}
		else if (memcmp(line, command_clear, sizeof(command_clear)) == 0) 
		{
			skip_dfu();
			reboot_counter_write(102);
			k_msleep(1);
			sys_reboot(SYS_REBOOT_WARM);
		}
#if DFU_EXISTS
		else if (memcmp(line, command_dfu, sizeof(command_dfu)) == 0)
		{
			NRF_POWER->GPREGRET = 0x57;
			sys_reboot(SYS_REBOOT_COLD);
		}
#endif
		else
		{
			printk("Unknown command\n");
		}
	}
}

#endif