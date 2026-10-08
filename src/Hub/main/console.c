#include "console.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_config.h"
#include "lwip/inet.h"
#include "wired_rx.h"

static const char *TAG = "console";

static void reboot_soon(void)
{
	printf("saved, rebooting\n");
	fflush(stdout);
	vTaskDelay(pdMS_TO_TICKS(200));
	esp_restart();
}

static int cmd_wifi(int argc, char **argv)
{
	if (argc < 2 || argc > 3) {
		printf("usage: wifi <ssid> [password]   (quote names with spaces)\n");
		return 1;
	}
	if (strlen(argv[1]) > 32 || (argc == 3 && strlen(argv[2]) > 64)) {
		printf("ssid max 32 chars, password max 64\n");
		return 1;
	}
	if (hub_config_set_wifi(argv[1], argc == 3 ? argv[2] : "") != ESP_OK) {
		printf("failed to save\n");
		return 1;
	}
	reboot_soon();
	return 0;
}

static int cmd_server(int argc, char **argv)
{
	if (argc != 2) {
		hub_config_t cfg;
		hub_config_load(&cfg);
		struct in_addr a = {.s_addr = cfg.server_ip};
		printf("usage: server <ip>|auto   (now: %s)\n", cfg.server_ip ? inet_ntoa(a) : "auto");
		return 1;
	}
	uint32_t ip = 0;
	if (strcmp(argv[1], "auto") != 0) {
		struct in_addr a;
		if (inet_aton(argv[1], &a) == 0) {
			printf("bad address\n");
			return 1;
		}
		ip = a.s_addr;
	}
	if (hub_config_set_server(ip) != ESP_OK) {
		printf("failed to save\n");
		return 1;
	}
	reboot_soon();
	return 0;
}

#if CONFIG_HUB_INPUT_WIRED
static int cmd_trackers(int argc, char **argv)
{
	wired_rx_print();
	return 0;
}
#endif

static int cmd_reboot(int argc, char **argv)
{
	esp_restart();
	return 0;
}

void console_start(void)
{
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	rc.prompt = "hub>";
	rc.max_cmdline_length = 160;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
	esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
	esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl);
#elif CONFIG_ESP_CONSOLE_USB_CDC
	esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
	esp_err_t err = esp_console_new_repl_usb_cdc(&hw, &rc, &repl);
#else
	esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
	esp_err_t err = esp_console_new_repl_uart(&hw, &rc, &repl);
#endif
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
		return;
	}

	const esp_console_cmd_t cmds[] = {
		{.command = "wifi", .help = "Store Wi-Fi credentials and reboot: wifi <ssid> [password]", .func = cmd_wifi},
		{.command = "server", .help = "Fix the SlimeVR server IP, or 'auto' to discover: server <ip>|auto", .func = cmd_server},
		{.command = "reboot", .help = "Restart the hub", .func = cmd_reboot},
#if CONFIG_HUB_INPUT_WIRED
		{.command = "trackers", .help = "Show the wired chains and the trackers seen on them", .func = cmd_trackers},
#endif
	};
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		esp_console_cmd_register(&cmds[i]);
	esp_console_register_help_command();
	esp_console_start_repl(repl);
}
