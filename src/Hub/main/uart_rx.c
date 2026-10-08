#include "uart_rx.h"

#include "sdkconfig.h"

#if CONFIG_HUB_INPUT_RECEIVER

#include <string.h>

#include "deframer.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hub.h"

static const char *TAG = "uart_rx";

#define PORT ((uart_port_t)CONFIG_HUB_UART_PORT)
#define RX_RING_SIZE 4096
#define EVENT_QUEUE_LEN 32

static QueueHandle_t s_sample_q;
static QueueHandle_t s_event_q;
static deframer_t s_df;

static void feed(const uint8_t *buf, int n)
{
	int64_t now = esp_timer_get_time();
	uint8_t pkt[HUB_PKT_LEN];
	hub_sample_t s;

	for (int i = 0; i < n; i++) {
		switch (deframer_push(&s_df, buf[i], pkt)) {
		case DF_OK:
			if (!decoder_parse(pkt, &s))
				break; // unknown packet type
			s.t_us = now;
			xQueueSend(s_sample_q, &s, 0); // dropped if the queue is full
			break;
		default: // byte consumed, or a malformed frame
			break;
		}
	}
}

static void uart_task(void *arg)
{
	uint8_t buf[256];
	uart_event_t ev;

	for (;;) {
		if (xQueueReceive(s_event_q, &ev, portMAX_DELAY) != pdTRUE)
			continue;
		switch (ev.type) {
		case UART_DATA: {
			int n;
			while ((n = uart_read_bytes(PORT, buf, sizeof(buf), 0)) > 0)
				feed(buf, n);
			break;
		}
		case UART_FIFO_OVF:
		case UART_BUFFER_FULL:
			uart_flush_input(PORT);
			xQueueReset(s_event_q);
			deframer_reset(&s_df);
			break;
		default:
			break;
		}
	}
}

void uart_rx_start(QueueHandle_t sample_q)
{
	s_sample_q = sample_q;
	deframer_reset(&s_df);

	const uart_config_t cfg = {
		.baud_rate = CONFIG_HUB_UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(PORT, RX_RING_SIZE, 0, EVENT_QUEUE_LEN, &s_event_q, 0));
	ESP_ERROR_CHECK(uart_param_config(PORT, &cfg));
	ESP_ERROR_CHECK(uart_set_pin(PORT, UART_PIN_NO_CHANGE, CONFIG_HUB_UART_RX_GPIO, UART_PIN_NO_CHANGE,
				     UART_PIN_NO_CHANGE));
	// Idle-high when the receiver is unplugged, instead of noise.
	gpio_pullup_en(CONFIG_HUB_UART_RX_GPIO);
	// Wake the task soon after each frame: on 2 idle character times or 32 buffered bytes.
	uart_set_rx_timeout(PORT, 2);
	uart_set_rx_full_threshold(PORT, 32);

	// On dual-core chips, core 1 keeps the parser away from the Wi-Fi stack on core 0.
	xTaskCreatePinnedToCore(uart_task, "uart_rx", 4096, NULL, 15, NULL, portNUM_PROCESSORS > 1 ? 1 : 0);
	ESP_LOGI(TAG, "UART%d RX on GPIO%d at %d baud", CONFIG_HUB_UART_PORT, CONFIG_HUB_UART_RX_GPIO,
		 CONFIG_HUB_UART_BAUD);
}

#endif // CONFIG_HUB_INPUT_RECEIVER
