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
#include "globals.h"
#include "system/system.h"
#include "connection.h"
#include "esb.h"
#include "wired.h"
#include "build_defines.h"

#include <string.h>
#include <zephyr/kernel.h>

#if CONFIG_WIRED_LINK

#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(wired, LOG_LEVEL_DBG);

// Both UARTs use zephyr,deferred-init. Radio relay mode starts only the RX UART (D8);
// wired chain mode starts both: RX from downstream on D8, TX upstream on D15.
static const struct device *const tx_uart = DEVICE_DT_GET(DT_CHOSEN(slimenrf_wired_tx_uart));
static const struct device *const rx_uart = DEVICE_DT_GET(DT_CHOSEN(slimenrf_wired_rx_uart));
static bool is_ankle; // wired chain mode (connection_over_uart); the name predates the chains

bool wired_is_ankle(void)
{
	return is_ankle;
}

#define TYPE_DATA 0x01  // legacy: ESB frame without address (firmware before the chains)
#define TYPE_HELLO 0x02
#define TYPE_ADATA 0x03 // addressed data, see wired.h
#define DATA_LEN (1 + WIRED_FRAME_LEN) // 22
#define HELLO_LEN (1 + 6 + 3)          // 10
#define ADATA_LEN (1 + 6 + 16 + 4 + 1) // 28
#define ADATA_CRC_LEN (6 + 16)         // address and packet
#define MAX_ENCODED 32                 // an ADATA frame encodes to 29 bytes

static size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
	size_t code_pos = 0, w = 1;
	uint8_t code = 1;
	for (size_t r = 0; r < len; r++) {
		if (in[r] == 0) {
			out[code_pos] = code;
			code_pos = w++;
			code = 1;
			continue;
		}
		out[w++] = in[r];
		if (++code == 0xFF) {
			out[code_pos] = code;
			code_pos = w++;
			code = 1;
		}
	}
	out[code_pos] = code;
	return w;
}

/* ------------------------------------------------------------------------------------- */
/* Ankle: send frames                                                                     */
/* ------------------------------------------------------------------------------------- */

// TX queue: the connection thread often sends several packets back to back (rotation, then
// battery/status/button), and a chain tracker also forwards everything from downstream.
// One frame takes about 0.65 ms at 460800 baud.
#define TXQ_LEN 16
static uint8_t txq_buf[TXQ_LEN][MAX_ENCODED];
static uint8_t txq_len[TXQ_LEN];
static uint8_t txq_head, txq_count;
static bool tx_active;
static uint32_t tx_dropped;
static uint32_t tx_frames, tx_hellos, tx_errors;
static int tx_last_err;
static bool uart_ok;

// Starts the next queued frame. Call with interrupts locked, or from the UART callback.
static void txq_kick(void)
{
	while (!tx_active && txq_count) {
		int err = uart_tx(tx_uart, txq_buf[txq_head], txq_len[txq_head], SYS_FOREVER_US);
		if (err == 0) {
			tx_active = true;
			return;
		}
		tx_errors++;
		tx_last_err = err;
		txq_head = (txq_head + 1) % TXQ_LEN; // drop it, try the next one
		txq_count--;
	}
}

static void ankle_uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	if (evt->type == UART_TX_DONE || evt->type == UART_TX_ABORTED) {
		unsigned int key = irq_lock();
		if (tx_active) {
			tx_active = false;
			txq_head = (txq_head + 1) % TXQ_LEN;
			txq_count--;
		}
		txq_kick();
		irq_unlock(key);
	}
}

// Frames raw[0..len) and queues it. Drops the frame if the queue is full.
static bool ankle_send(const uint8_t *raw, size_t len)
{
	if (!uart_ok)
		return false;
	unsigned int key = irq_lock();
	if (txq_count == TXQ_LEN) {
		irq_unlock(key);
		tx_dropped++;
		return false;
	}
	uint8_t slot = (txq_head + txq_count) % TXQ_LEN;
	size_t n = cobs_encode(raw, len, txq_buf[slot]);
	txq_buf[slot][n++] = 0;
	txq_len[slot] = (uint8_t)n;
	txq_count++;
	if (raw[0] == TYPE_HELLO)
		tx_hellos++;
	else
		tx_frames++;
	txq_kick();
	irq_unlock(key);
	return true;
}

// Builds an ADATA frame: [type][addr 6][packet 16][CRC32 LE over addr + packet][sequence].
static void build_adata(uint8_t raw[ADATA_LEN], const uint8_t addr[6], const uint8_t packet[16], uint8_t seq)
{
	raw[0] = TYPE_ADATA;
	memcpy(&raw[1], addr, 6);
	memcpy(&raw[7], packet, 16);
	uint32_t crc = crc32_k_4_2_update(0x93a409eb, &raw[1], ADATA_CRC_LEN);
	memcpy(&raw[23], &crc, sizeof(crc));
	raw[27] = seq;
}

void wired_write_data(const uint8_t frame[WIRED_FRAME_LEN])
{
	uint8_t raw[ADATA_LEN];
	build_adata(raw, (const uint8_t *)NRF_FICR->DEVICEADDR, frame, frame[20]);
	ankle_send(raw, sizeof(raw)); // if the queue is full, the next sample replaces this one
}

static void hello_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(hello_work, hello_work_fn);

static void hello_work_fn(struct k_work *work)
{
	uint8_t raw[HELLO_LEN];
	raw[0] = TYPE_HELLO;
	memcpy(&raw[1], (const void *)NRF_FICR->DEVICEADDR, 6);
	raw[7] = FW_VERSION_MAJOR & 255;
	raw[8] = FW_VERSION_MINOR & 255;
	raw[9] = FW_VERSION_PATCH & 255;
	// Retry soon if the queue was full, otherwise once per second.
	k_work_reschedule(&hello_work, ankle_send(raw, sizeof(raw)) ? K_MSEC(1000) : K_MSEC(3));
}

static void wired_ankle_start(void)
{
	if (device_init(tx_uart) != 0 || !device_is_ready(tx_uart) ||
	    uart_callback_set(tx_uart, ankle_uart_cb, NULL) != 0) {
		LOG_ERR("Wired TX UART not ready");
		return;
	}
	uart_ok = true;
	k_work_reschedule(&hello_work, K_MSEC(100));
	LOG_INF("Wired chain mode: data upstream over %s (D15), radio off", tx_uart->name);
}

/* ------------------------------------------------------------------------------------- */
/* Relay: receive, stamp, queue for ESB                                                   */
/* ------------------------------------------------------------------------------------- */

#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>

#define RX_BUF_SIZE 64
#define PROBE_PERIOD_MS 1000 // fallback: listen briefly this often even without a wake edge
#define PROBE_WINDOW_MS 100 // long enough for one frame at low baud rates
#define RX_TIMEOUT_US 100 // deliver bytes this long after the line goes idle
#define HELLO_VALID_MS 3000
#define UNPAIRED_LOG_MS 10000

static const struct gpio_dt_spec wake_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wired_gpios);
static struct gpio_callback wake_cb;

static uint8_t rx_bufs[2][RX_BUF_SIZE];
static uint8_t rx_next_buf;
static atomic_t rx_on;
static atomic_t relaying;

K_MSGQ_DEFINE(relay_q, WIRED_FRAME_LEN, 4, 1);

// deframer, touched only from the UART callback
static uint8_t enc[MAX_ENCODED];
static size_t enc_len;
static bool enc_over, enc_synced;

struct ankle_entry {
	uint8_t addr[6];
	uint8_t id;
	uint8_t used;
};
static struct ankle_entry ankles[CONFIG_WIRED_ANKLE_TABLE_SIZE];

static uint8_t hello_addr[6];
static uint8_t hello_fw[3];
static int64_t hello_time;
static volatile int ankle_id = -1; // receiver id of the connected ankle, -1 if unknown

static uint32_t st_relayed, st_bad, st_unpaired, st_qdrop, st_line_err;
static uint32_t st_wakes, st_rx_starts, st_rx_bytes, st_rx_enable_err, st_probes, st_probe_found;
static bool session_from_probe, session_found;
static int64_t last_unpaired_log = -UNPAIRED_LOG_MS;

static void start_rx_work_fn(struct k_work *work);
static void idle_work_fn(struct k_work *work);
static void rearm_work_fn(struct k_work *work);
static K_WORK_DEFINE(start_rx_work, start_rx_work_fn);
static K_WORK_DELAYABLE_DEFINE(idle_work, idle_work_fn);
static K_WORK_DEFINE(rearm_work, rearm_work_fn);
static void probe_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(probe_work, probe_work_fn);

bool wired_relaying(void)
{
	return atomic_get(&relaying);
}

bool wired_relay_get(uint8_t frame[WIRED_FRAME_LEN])
{
	return k_msgq_get(&relay_q, frame, K_NO_WAIT) == 0;
}

static int lookup_ankle(const uint8_t *addr)
{
	for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE; i++)
		if (ankles[i].used && memcmp(ankles[i].addr, addr, 6) == 0)
			return ankles[i].id;
	return -1;
}

static uint64_t addr_u64(const uint8_t *addr)
{
	uint64_t v = 0;
	memcpy(&v, addr, 6);
	return v;
}

// Chain mode: passes a checked frame from downstream on, unchanged. Runs in the RX UART's
// callback; ankle_send() is safe there. Dropped (and counted) if the TX queue is full.
static void forward(const uint8_t *raw, size_t len)
{
	if (ankle_send(raw, len))
		st_relayed++;
	else
		st_qdrop++;
	atomic_set(&relaying, 1); // keeps this tracker awake while it carries others
}

static void handle_hello(const uint8_t *d)
{
	bool changed = memcmp(hello_addr, &d[1], 6) != 0 || hello_time == 0;
	memcpy(hello_addr, &d[1], 6);
	memcpy(hello_fw, &d[7], 3);
	hello_time = k_uptime_get();
	if (is_ankle) {
		if (changed)
			LOG_INF("Downstream tracker %012llX connected", addr_u64(hello_addr));
		forward(d, HELLO_LEN);
		return;
	}
	int id = lookup_ankle(hello_addr);
	if (changed || id != ankle_id) {
		if (id >= 0)
			LOG_INF("Wired tracker %012llX connected, id %d", addr_u64(hello_addr), id);
		else
			LOG_WRN("Wired tracker %012llX connected, not paired: run pair_wired", addr_u64(hello_addr));
	}
	ankle_id = id;
}

// Relay mode: queues an ESB frame for the radio with the receiver id stamped in.
static void relay_frame(uint8_t *frame, int id);

// Addressed data from downstream, CRC already checked.
static void handle_adata(const uint8_t *d)
{
	if (is_ankle) {
		forward(d, ADATA_LEN);
		return;
	}
	// A chain-firmware ankle plugged into a radio relay: the address names the ankle.
	int id = lookup_ankle(&d[1]);
	if (id < 0) {
		st_unpaired++;
		if (k_uptime_get() - last_unpaired_log >= UNPAIRED_LOG_MS) {
			last_unpaired_log = k_uptime_get();
			LOG_WRN("Wired tracker %012llX not paired, dropping its data: run pair_wired", addr_u64(&d[1]));
		}
		return;
	}
	uint8_t frame[WIRED_FRAME_LEN];
	memcpy(frame, &d[7], 16);
	frame[20] = d[27];
	relay_frame(frame, id);
}

static void handle_data(uint8_t *d)
{
	uint8_t *frame = &d[1]; // packet[16], CRC32, sequence
	if (is_ankle) {
		// Legacy ankle firmware on a chain: it sends no address, so use the one from its HELLO.
		if (hello_time == 0) {
			st_unpaired++;
			return;
		}
		uint8_t raw[ADATA_LEN];
		build_adata(raw, hello_addr, frame, frame[20]);
		forward(raw, sizeof(raw));
		return;
	}
	int id = ankle_id;
	if (id < 0) {
		st_unpaired++;
		if (k_uptime_get() - last_unpaired_log >= UNPAIRED_LOG_MS) {
			last_unpaired_log = k_uptime_get();
			LOG_WRN("Wired tracker not paired, dropping its data");
		}
		return;
	}
	relay_frame(frame, id);
}

static void relay_frame(uint8_t *frame, int id)
{
	// Stamp the real id and recompute the CRC; the ankle's sequence byte is kept.
	// A frame purged from the queue below shows up as a sequence jump, which the receiver
	// accepts (it only drops out-of-order frames). Do not "repair" the sequence here.
	frame[1] = (uint8_t)id;
	uint32_t crc = crc32_k_4_2_update(0x93a409eb, frame, 16);
	memcpy(&frame[16], &crc, sizeof(crc)); // little-endian, as on ESB
	if (k_msgq_put(&relay_q, frame, K_NO_WAIT) != 0) {
		uint8_t old[WIRED_FRAME_LEN];
		k_msgq_get(&relay_q, old, K_NO_WAIT); // purge the oldest, keep the newest
		k_msgq_put(&relay_q, frame, K_NO_WAIT);
		st_qdrop++;
	}
	st_relayed++;
	atomic_set(&relaying, 1);
	connection_wake();
}

static void frame_done(void)
{
	uint8_t d[MAX_ENCODED];
	size_t r = 0, w = 0;
	bool ok = !enc_over;
	// COBS decode
	while (ok && r < enc_len) {
		uint8_t code = enc[r++];
		for (uint8_t i = 1; ok && i < code; i++) {
			if (r >= enc_len || w >= sizeof(d))
				ok = false;
			else
				d[w++] = enc[r++];
		}
		if (ok && code != 0xFF && r < enc_len) {
			if (w >= sizeof(d))
				ok = false;
			else
				d[w++] = 0;
		}
	}
	if (ok && w == DATA_LEN && d[0] == TYPE_DATA) {
		uint32_t crc;
		memcpy(&crc, &d[17], sizeof(crc));
		if (crc == crc32_k_4_2_update(0x93a409eb, &d[1], 16)) {
			if (session_from_probe && !session_found)
				st_probe_found++;
			session_found = true;
			k_work_reschedule(&idle_work, K_MSEC(CONFIG_WIRED_RELAY_IDLE_MS));
			handle_data(d);
			return;
		}
	} else if (ok && w == ADATA_LEN && d[0] == TYPE_ADATA) {
		uint32_t crc;
		memcpy(&crc, &d[23], sizeof(crc));
		if (crc == crc32_k_4_2_update(0x93a409eb, &d[1], ADATA_CRC_LEN)) {
			if (session_from_probe && !session_found)
				st_probe_found++;
			session_found = true;
			k_work_reschedule(&idle_work, K_MSEC(CONFIG_WIRED_RELAY_IDLE_MS));
			handle_adata(d);
			return;
		}
	} else if (ok && w == HELLO_LEN && d[0] == TYPE_HELLO) {
		if (session_from_probe && !session_found)
			st_probe_found++;
		session_found = true;
		k_work_reschedule(&idle_work, K_MSEC(CONFIG_WIRED_RELAY_IDLE_MS));
		handle_hello(d);
		return;
	}
	st_bad++;
}

static void feed(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t c = buf[i];
		if (c == 0) {
			if (enc_synced && enc_len > 0)
				frame_done();
			enc_synced = true; // the first frame after waking is usually partial
			enc_len = 0;
			enc_over = false;
		} else if (enc_len < sizeof(enc)) {
			enc[enc_len++] = c;
		} else {
			enc_over = true;
		}
	}
}

static void relay_uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	switch (evt->type) {
	case UART_RX_RDY:
		st_rx_bytes += evt->data.rx.len;
		feed(evt->data.rx.buf + evt->data.rx.offset, evt->data.rx.len);
		break;
	case UART_RX_BUF_REQUEST:
		uart_rx_buf_rsp(dev, rx_bufs[rx_next_buf], RX_BUF_SIZE);
		rx_next_buf ^= 1;
		break;
	case UART_RX_STOPPED:
		st_line_err++; // break or framing error, e.g. cable unplugged; RX_DISABLED follows
		break;
	case UART_RX_DISABLED:
		atomic_clear(&rx_on);
		atomic_clear(&relaying);
		k_work_cancel_delayable(&idle_work);
		k_work_submit(&rearm_work);
		break;
	default:
		break;
	}
}

static void wake_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	gpio_pin_interrupt_configure_dt(&wake_pin, GPIO_INT_DISABLE);
	st_wakes++;
	k_work_submit(&start_rx_work);
}

static void rearm_work_fn(struct k_work *work)
{
	// Release the pin: a suspended UARTE leaves it to the GPIO, so the edge detector sees it.
	pm_device_action_run(rx_uart, PM_DEVICE_ACTION_SUSPEND);
	gpio_pin_configure_dt(&wake_pin, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&wake_pin, GPIO_INT_EDGE_TO_ACTIVE);
}

static void start_rx(bool from_probe)
{
	if (!atomic_cas(&rx_on, 0, 1))
		return;
	gpio_pin_interrupt_configure_dt(&wake_pin, GPIO_INT_DISABLE);
	session_from_probe = from_probe;
	session_found = false;
	pm_device_action_run(rx_uart, PM_DEVICE_ACTION_RESUME);
	enc_len = 0;
	enc_over = false;
	enc_synced = false;
	rx_next_buf = 1;
	st_rx_starts++;
	int err = uart_rx_enable(rx_uart, rx_bufs[0], RX_BUF_SIZE, RX_TIMEOUT_US);
	if (err != 0) {
		st_rx_enable_err++;
		LOG_ERR("Wired UART RX enable failed: %d", err);
		atomic_clear(&rx_on);
		k_work_submit(&rearm_work);
		return;
	}
	// Give up again if nothing valid arrives (noise on an open connector, or an empty probe).
	k_work_reschedule(&idle_work, K_MSEC(from_probe ? PROBE_WINDOW_MS : CONFIG_WIRED_RELAY_IDLE_MS));
}

static void start_rx_work_fn(struct k_work *work)
{
	start_rx(false);
}

static void probe_work_fn(struct k_work *work)
{
	if (!atomic_get(&rx_on)) {
		st_probes++;
		start_rx(true);
	}
	k_work_reschedule(&probe_work, K_MSEC(PROBE_PERIOD_MS));
}

static void idle_work_fn(struct k_work *work)
{
	if (atomic_get(&relaying))
		LOG_INF("Wired link idle");
	if (atomic_get(&rx_on) && uart_rx_disable(rx_uart) != 0) {
		// Already stopping or stopped; make sure the edge detector is armed.
		atomic_clear(&rx_on);
		atomic_clear(&relaying);
		k_work_submit(&rearm_work);
	}
}

static void wired_relay_start(void)
{
	sys_read(WIRED_ANKLES_ID, ankles, sizeof(ankles));
	if (device_init(rx_uart) != 0 || !device_is_ready(rx_uart) ||
	    uart_callback_set(rx_uart, relay_uart_cb, NULL) != 0) {
		LOG_ERR("Wired RX UART not ready");
		return;
	}
	if (!gpio_is_ready_dt(&wake_pin)) {
		LOG_ERR("Wired wake pin not ready");
		return;
	}
	gpio_init_callback(&wake_cb, wake_isr, BIT(wake_pin.pin));
	gpio_add_callback(wake_pin.port, &wake_cb);
	rearm_work_fn(NULL);
	k_work_reschedule(&probe_work, K_MSEC(PROBE_PERIOD_MS));
	LOG_INF("%s: listening on %s (D8)", is_ankle ? "Wired chain mode" : "Relay mode", rx_uart->name);
}

/* ---- ankle table and pairing ---- */

static void store_ankle(const uint8_t *addr, uint8_t id)
{
	int slot = -1;
	for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE && slot < 0; i++)
		if (ankles[i].used && memcmp(ankles[i].addr, addr, 6) == 0)
			slot = i;
	for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE && slot < 0; i++)
		if (!ankles[i].used)
			slot = i;
	if (slot < 0) { // full: drop the oldest entry
		memmove(&ankles[0], &ankles[1], sizeof(ankles[0]) * (CONFIG_WIRED_ANKLE_TABLE_SIZE - 1));
		slot = CONFIG_WIRED_ANKLE_TABLE_SIZE - 1;
	}
	memcpy(ankles[slot].addr, addr, 6);
	ankles[slot].id = id;
	ankles[slot].used = 1;
	sys_write(WIRED_ANKLES_ID, NULL, ankles, sizeof(ankles));
}

static K_SEM_DEFINE(pair_sem, 0, 1);

void wired_request_pair_ankle(void)
{
	if (is_ankle) {
		LOG_WRN("Wired chain mode (connection_over_uart) needs no pairing: the hub knows trackers by address");
		return;
	}
	k_sem_give(&pair_sem);
}

static void pair_thread(void)
{
	while (1) {
		k_sem_take(&pair_sem, K_FOREVER);
		if (hello_time == 0 || k_uptime_get() - hello_time > HELLO_VALID_MS) {
			LOG_WRN("No wired tracker connected (no HELLO in the last %d s)", HELLO_VALID_MS / 1000);
			continue;
		}
		uint8_t addr[6];
		unsigned int key = irq_lock();
		memcpy(addr, hello_addr, 6);
		irq_unlock(key);
		LOG_INF("Pairing wired tracker %012llX through this relay, receiver must be in pairing mode", addr_u64(addr));
		uint8_t id;
		int err = esb_pair_other(addr, &id);
		if (err) {
			LOG_WRN("Wired tracker pairing failed: %d", err);
			continue;
		}
		store_ankle(addr, id);
		if (memcmp(addr, hello_addr, 6) == 0)
			ankle_id = id;
		LOG_INF("Wired tracker %012llX paired as id %u", addr_u64(addr), id);
	}
}

K_THREAD_DEFINE(wired_pair_thread_id, 1024, pair_thread, NULL, NULL, NULL, 7, 0, 0);

void wired_forget_ankles(void)
{
	memset(ankles, 0, sizeof(ankles));
	sys_write(WIRED_ANKLES_ID, NULL, ankles, sizeof(ankles));
	ankle_id = -1;
	printk("Wired tracker table cleared\n");
}

#if !defined(WIRED_HOST_TEST)
// Raw register dump of a UARTE and the GPIO pin it uses, for debugging the physical link.
static void print_uarte_regs(const char *what, uint32_t base, bool tx)
{
	NRF_UARTE_Type *u = (NRF_UARTE_Type *)base;
	uint32_t psel = tx ? u->PSEL.TXD : u->PSEL.RXD;
	printk("%s: UARTE@%08x ENABLE=%u PSEL.%s=0x%08x BAUDRATE=0x%08x\n", what, base, u->ENABLE, tx ? "TXD" : "RXD",
	       psel, u->BAUDRATE);
	if (psel & 0x80000000u) {
		printk("  pin disconnected\n");
		return;
	}
	uint32_t port = (psel >> 5) & 1, pin = psel & 31;
	NRF_GPIO_Type *g = port ? NRF_P1 : NRF_P0;
	printk("  P%u.%02u PIN_CNF=0x%08x OUT=%u IN=%u\n", port, pin, g->PIN_CNF[pin], (g->OUT >> pin) & 1,
	       (g->IN >> pin) & 1);
}
#endif

void wired_print_status(void)
{
#if !defined(WIRED_HOST_TEST)
	print_uarte_regs("TX uart", DT_REG_ADDR(DT_CHOSEN(slimenrf_wired_tx_uart)), true);
	print_uarte_regs("RX uart", DT_REG_ADDR(DT_CHOSEN(slimenrf_wired_rx_uart)), false);
#endif
	if (is_ankle) {
		printk("Wired chain mode: upstream over %s (D15), downstream on %s (D8, UART %s), radio off\n",
		       tx_uart->name, rx_uart->name, atomic_get(&rx_on) ? "on" : "off");
		printk("Sent %u data frames, %u HELLOs (own and forwarded); dropped %u (queue full), errors %u (last %d)\n",
		       tx_frames, tx_hellos, tx_dropped, tx_errors, tx_last_err);
		if (hello_time)
			printk("Last downstream tracker: %012llX fw %u.%u.%u, %lld ms ago\n", addr_u64(hello_addr), hello_fw[0],
			       hello_fw[1], hello_fw[2], k_uptime_get() - hello_time);
		else
			printk("Downstream: nothing connected\n");
		printk("Forwarded %u, bad %u, queue drops %u, line errors %u, bytes received %u\n", st_relayed, st_bad,
		       st_qdrop, st_line_err, st_rx_bytes);
		printk("Set connection_over_uart to 0 and reboot for radio relay mode\n");
		return;
	}
	printk("Relay mode: %s, UART %s\n", wired_relaying() ? "relaying" : "idle", atomic_get(&rx_on) ? "on" : "off");
	if (hello_time)
		printk("Last wired tracker: %012llX fw %u.%u.%u, %lld ms ago, id %d\n", addr_u64(hello_addr), hello_fw[0],
		       hello_fw[1], hello_fw[2], k_uptime_get() - hello_time, ankle_id);
	else
		printk("Last wired tracker: none\n");
	printk("Relayed %u, bad %u, unpaired %u, queue drops %u, line errors %u\n", st_relayed, st_bad, st_unpaired,
	       st_qdrop, st_line_err);
	printk("Wake edges %u, probes %u (found %u), UART starts %u (failed %u), bytes received %u\n",
	       st_wakes, st_probes, st_probe_found, st_rx_starts, st_rx_enable_err, st_rx_bytes);
	if (!atomic_get(&rx_on)) {
		// Sample the RX pin directly: a sending ankle pulls it low for part of the time.
		uint32_t low = 0;
		for (int i = 0; i < 50000; i++)
			low += gpio_pin_get_raw(wake_pin.port, wake_pin.pin) == 0;
		printk("RX pin sampled 50000 times: %u low (0 = no signal on the pin)\n", low);
	}
	printk("Paired wired trackers:\n");
	for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE; i++)
		if (ankles[i].used)
			printk("  %012llX -> id %u\n", addr_u64(ankles[i].addr), ankles[i].id);
}


/* ------------------------------------------------------------------------------------- */
/* Role selection at boot                                                                 */
/* ------------------------------------------------------------------------------------- */

#if !defined(WIRED_HOST_TEST)
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), wired_gnd_gpios)
#include <hal/nrf_gpio.h>
// Drive the link's ground pin low as early as possible, in both modes (high drive for low ohms).
static int wired_gnd_init(void)
{
	uint32_t pin = NRF_DT_GPIOS_TO_PSEL(DT_PATH(zephyr_user), wired_gnd_gpios);
	nrf_gpio_pin_clear(pin);
	nrf_gpio_cfg(pin, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
		     NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	return 0;
}

SYS_INIT(wired_gnd_init, PRE_KERNEL_1, 0);
#endif
#endif

// Priority 99: after the settings were loaded from retained RAM / NVS (system.c),
// and before the ESB and connection threads start.
static int wired_init(void)
{
	is_ankle = CONFIG_0_SETTINGS_READ(CONFIG_0_CONNECTION_OVER_UART) != 0;
	if (is_ankle)
		wired_ankle_start(); // upstream TX on D15
	wired_relay_start();     // downstream RX on D8, in both modes
	return 0;
}

SYS_INIT(wired_init, APPLICATION, 99);

#endif
