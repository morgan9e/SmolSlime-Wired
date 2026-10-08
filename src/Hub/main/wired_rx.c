#include "wired_rx.h"

#include "sdkconfig.h"

#if CONFIG_HUB_INPUT_WIRED

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hub.h"
#include "soc/soc_caps.h"
#include "wired_link.h"

static const char *TAG = "wired_rx";

#define RX_RING_SIZE 2048
#define EVENT_QUEUE_LEN 32
#define ADDR_REPEAT_US 1000000 // re-send each id's address sample, in case the queue dropped one

typedef struct {
	uart_port_t port;
	int gpio;
	QueueHandle_t evq;
	wl_deframer_t df;
	uint32_t frames, hellos, bad, overflows;
} chain_t;

typedef struct {
	int64_t last_us;
	uint32_t frames;
	uint8_t fw[3];
	uint8_t chain;
	uint8_t last_seq;
	uint32_t seq_gaps; // frames missing between consecutive sequence numbers
	bool have_seq;
} trk_stat_t;

// Chain n uses UART port s_ports[n]; UART1 first, so a single chain keeps UART0 free.
#if SOC_UART_HP_NUM > 2
static const uart_port_t s_ports[] = {UART_NUM_1, UART_NUM_2, UART_NUM_0};
#else
static const uart_port_t s_ports[] = {UART_NUM_1, UART_NUM_0};
#endif
static const int s_gpios[] = {
	CONFIG_HUB_WIRED_CHAIN1_RX_GPIO,
	CONFIG_HUB_WIRED_CHAIN2_RX_GPIO,
	CONFIG_HUB_WIRED_CHAIN3_RX_GPIO,
};
#define NCHAINS (CONFIG_HUB_WIRED_CHAINS < (int)(sizeof(s_ports) / sizeof(s_ports[0])) \
			 ? CONFIG_HUB_WIRED_CHAINS                                     \
			 : (int)(sizeof(s_ports) / sizeof(s_ports[0])))

static chain_t s_chain[3];
static wl_table_t s_table; // touched only by the wired task (wired_rx_print reads it racily)
static trk_stat_t s_trk[WL_MAX_IDS];
static QueueHandle_t s_sample_q;
static QueueSetHandle_t s_set;

static void push_addr(int id, int64_t now)
{
	uint8_t pkt[HUB_PKT_LEN] = {255, (uint8_t)id};
	memcpy(&pkt[2], s_table.addr[id], 6);
	hub_sample_t s;
	if (decoder_parse(pkt, &s)) {
		s.t_us = now;
		xQueueSend(s_sample_q, &s, 0);
	}
}

static void on_frame(int ci, wl_result_t r, wl_frame_t *f, int64_t now)
{
	chain_t *c = &s_chain[ci];
	if (r == WL_BAD) {
		c->bad++;
		return;
	}
	bool added;
	int id = wl_table_id(&s_table, f->addr, &added);
	if (id < 0 || id >= HUB_MAX_TRACKERS) {
		if (added || id < 0)
			ESP_LOGW(TAG, "chain %d: tracker %02X%02X%02X%02X%02X%02X ignored, ids are full", ci + 1, f->addr[5],
				 f->addr[4], f->addr[3], f->addr[2], f->addr[1], f->addr[0]);
		return;
	}
	trk_stat_t *t = &s_trk[id];
	if (added) {
		ESP_LOGI(TAG, "chain %d: tracker %02X%02X%02X%02X%02X%02X is id %d", ci + 1, f->addr[5], f->addr[4],
			 f->addr[3], f->addr[2], f->addr[1], f->addr[0], id);
		push_addr(id, now);
	}
	t->chain = (uint8_t)ci;
	t->last_us = now;
	if (r == WL_HELLO) {
		c->hellos++;
		memcpy(t->fw, f->fw, 3);
		return;
	}
	c->frames++;
	t->frames++;
	if (t->have_seq)
		t->seq_gaps += (uint8_t)(f->seq - t->last_seq - 1);
	t->last_seq = f->seq;
	t->have_seq = true;

	f->pkt[1] = (uint8_t)id; // trackers send 0xFF here
	hub_sample_t s;
	if (!decoder_parse(f->pkt, &s))
		return;
	s.flags &= (uint16_t)~HS_RSSI; // byte 15 is the receiver's RSSI slot; a cable has none
	if (!s.flags)
		return;
	s.t_us = now;
	xQueueSend(s_sample_q, &s, 0); // dropped if the queue is full
}

static void read_chain(int ci)
{
	chain_t *c = &s_chain[ci];
	uart_event_t ev;
	if (xQueueReceive(c->evq, &ev, 0) != pdTRUE)
		return;
	switch (ev.type) {
	case UART_DATA: {
		uint8_t buf[256];
		int n;
		while ((n = uart_read_bytes(c->port, buf, sizeof(buf), 0)) > 0) {
			int64_t now = esp_timer_get_time();
			wl_frame_t f;
			for (int i = 0; i < n; i++) {
				wl_result_t r = wl_push(&c->df, buf[i], &f);
				if (r != WL_NONE)
					on_frame(ci, r, &f, now);
			}
		}
		break;
	}
	case UART_FIFO_OVF:
	case UART_BUFFER_FULL:
		c->overflows++;
		uart_flush_input(c->port);
		wl_reset(&c->df);
		break;
	default:
		break;
	}
}

static void wired_task(void *arg)
{
	int64_t next_addr = esp_timer_get_time() + ADDR_REPEAT_US;
	for (;;) {
		QueueSetMemberHandle_t m = xQueueSelectFromSet(s_set, pdMS_TO_TICKS(100));
		for (int ci = 0; m && ci < NCHAINS; ci++)
			if (m == s_chain[ci].evq)
				read_chain(ci);
		int64_t now = esp_timer_get_time();
		if (now >= next_addr) {
			next_addr = now + ADDR_REPEAT_US;
			for (int id = 0; id < s_table.count && id < HUB_MAX_TRACKERS; id++)
				push_addr(id, now);
		}
	}
}

static void start_chain(int ci)
{
	chain_t *c = &s_chain[ci];
	c->port = s_ports[ci];
	c->gpio = s_gpios[ci];
	wl_reset(&c->df);
	const uart_config_t cfg = {
		.baud_rate = CONFIG_HUB_WIRED_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(c->port, RX_RING_SIZE, 0, EVENT_QUEUE_LEN, &c->evq, 0));
	ESP_ERROR_CHECK(uart_param_config(c->port, &cfg));
	ESP_ERROR_CHECK(uart_set_pin(c->port, UART_PIN_NO_CHANGE, c->gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
	gpio_pullup_en(c->gpio); // idle-high with nothing plugged in, instead of noise
	uart_set_rx_timeout(c->port, 2);
	uart_set_rx_full_threshold(c->port, 32);
	ESP_ERROR_CHECK(xQueueAddToSet(c->evq, s_set));
	ESP_LOGI(TAG, "chain %d: UART%d RX on GPIO%d at %d baud", ci + 1, c->port, c->gpio, CONFIG_HUB_WIRED_BAUD);
}

void wired_rx_start(QueueHandle_t sample_q)
{
	s_sample_q = sample_q;
	s_set = xQueueCreateSet(EVENT_QUEUE_LEN * NCHAINS);
	configASSERT(s_set);
	for (int ci = 0; ci < NCHAINS; ci++)
		start_chain(ci);
	// On dual-core chips, core 1 keeps the parser away from the Wi-Fi stack on core 0.
	xTaskCreatePinnedToCore(wired_task, "wired_rx", 4096, NULL, 15, NULL, portNUM_PROCESSORS > 1 ? 1 : 0);
}

void wired_rx_print(void)
{
	int64_t now = esp_timer_get_time();
	for (int ci = 0; ci < NCHAINS; ci++) {
		chain_t *c = &s_chain[ci];
		printf("chain %d (GPIO%d): %lu frames, %lu hellos, %lu bad, %lu overflows\n", ci + 1, c->gpio,
		       (unsigned long)c->frames, (unsigned long)c->hellos, (unsigned long)c->bad,
		       (unsigned long)c->overflows);
	}
	if (!s_table.count)
		printf("no trackers seen yet\n");
	for (int id = 0; id < s_table.count; id++) {
		const uint8_t *a = s_table.addr[id];
		trk_stat_t *t = &s_trk[id];
		printf("id %d: %02X%02X%02X%02X%02X%02X on chain %d, fw %u.%u.%u, %lu frames, %lu missing, last %lld ms ago\n",
		       id, a[5], a[4], a[3], a[2], a[1], a[0], t->chain + 1, t->fw[0], t->fw[1], t->fw[2],
		       (unsigned long)t->frames, (unsigned long)t->seq_gaps, (now - t->last_us) / 1000);
	}
}

#endif // CONFIG_HUB_INPUT_WIRED
