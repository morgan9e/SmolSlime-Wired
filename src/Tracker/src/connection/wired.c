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
#include "hub.h"
#include "build_defines.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

/* ------------------------------------------------------------------------------------- */
/* Mode words (also used by the console without the wired link)                           */
/* ------------------------------------------------------------------------------------- */

static bool word_is(const char *word, const char *lower)
{
	for (; *word && *lower; word++, lower++)
		if (tolower((unsigned char)*word) != *lower)
			return false;
	return !*word && !*lower;
}

int wired_mode_parse(int argc, char **argv)
{
	if (argc == 1 && word_is(argv[0], "wireless"))
		return 0;
	if (argc == 1 && word_is(argv[0], "hub"))
		return WIRED_MODE_HUB;
	if (argc < 3 || !word_is(argv[0], "wired"))
		return -1;
	int mode = 0;
	bool have_in = false, have_out = false;
	for (int i = 1; i < argc; i += 2) {
		if (i + 1 >= argc)
			return -1;
		bool left = word_is(argv[i + 1], "left"), right = word_is(argv[i + 1], "right");
		bool both = word_is(argv[i + 1], "both");
		if (word_is(argv[i], "in") && !have_in && (left || right || both)) {
			have_in = true;
			mode |= (left || both ? WIRED_IN_LEFT : 0) | (right || both ? WIRED_IN_RIGHT : 0);
		} else if (word_is(argv[i], "out") && !have_out && (left || right)) {
			have_out = true;
			mode |= left ? WIRED_OUT_LEFT : WIRED_OUT_RIGHT;
		} else {
			return -1;
		}
	}
	return wired_mode_valid(mode) ? mode : -1;
}

void wired_mode_format(int mode, char *buf, size_t len)
{
	static const char *const sides[] = {"", "left", "right", "both"};
	if (mode == 0) {
		snprintf(buf, len, "wireless");
		return;
	}
	if (mode & WIRED_HUB) {
		snprintf(buf, len, "hub");
		return;
	}
	int in = mode & 3, out = (mode >> 2) & 3;
	snprintf(buf, len, "wired%s%s%s%s", in ? " in " : "", sides[in], out ? " out " : "", sides[out]);
}

#if CONFIG_WIRED_LINK

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(wired, LOG_LEVEL_DBG);

#define TYPE_DATA 0x01  // legacy: ESB frame without address (firmware before the chains)
#define TYPE_HELLO 0x02
#define TYPE_ADATA 0x03 // addressed data, see wired.h
#define DATA_LEN (1 + WIRED_FRAME_LEN) // 22
#define HELLO_LEN (1 + 6 + 3)          // 10
#define ADATA_LEN (1 + 6 + 16 + 4 + 1) // 28
#define ADATA_CRC_LEN (6 + 16)         // address and packet
#define MAX_ENCODED 32                 // an ADATA frame encodes to 29 bytes

#define RX_BUF_SIZE 64
#define PROBE_PERIOD_MS 1000 // fallback: listen briefly this often even without a wake edge
#define PROBE_WINDOW_MS 100 // long enough for one frame at low baud rates
#define RX_TIMEOUT_US 100 // deliver bytes this long after the line goes idle
#define HELLO_VALID_MS 3000
#define UNPAIRED_LOG_MS 10000
#define SEEN_MAX 8

enum { PORT_LEFT, PORT_RIGHT, PORT_COUNT };
static const char *const port_names[PORT_COUNT] = {"left", "right"};
static const char *const port_pins[PORT_COUNT] = {"D9", "D10"}; // data pins

// One header: its UART (an input receives, an output transmits) and its data pin, which is
// watched as a GPIO while an input's UART is off.
struct port {
	const struct device *uart;
	struct gpio_dt_spec data_pin;
	bool in, out;
	bool always_on; // hub mode: receive all the time (the left UART also carries the hub output)
	// receive
	uint8_t rx_bufs[2][RX_BUF_SIZE];
	uint8_t rx_next_buf;
	atomic_t rx_on;
	atomic_t active; // valid frames arriving
	uint8_t enc[MAX_ENCODED]; // deframer, touched only from the UART callback
	size_t enc_len;
	bool enc_over, enc_synced;
	struct gpio_callback wake_cb;
	struct k_work start_rx_work, rearm_work;
	struct k_work_delayable idle_work, probe_work;
	bool session_from_probe, session_found;
	// The last HELLO on this input: names legacy DATA frames, which carry no address.
	uint8_t hello_addr[6];
	int64_t hello_time;
	uint32_t st_frames, st_bad, st_line_err, st_wakes, st_rx_starts, st_rx_bytes, st_rx_enable_err;
	uint32_t st_probes, st_probe_found;
};

static struct port ports[PORT_COUNT] = {
	[PORT_LEFT] = {
		.uart = DEVICE_DT_GET(DT_CHOSEN(slimenrf_wired_left_uart)),
		.data_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wired_left_gpios),
	},
	[PORT_RIGHT] = {
		.uart = DEVICE_DT_GET(DT_CHOSEN(slimenrf_wired_right_uart)),
		.data_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), wired_right_gpios),
	},
};

static int mode;          // resolved at boot
static int out_port = -1; // the header with the output, -1: none (radio relay or wireless)

int wired_mode(void)
{
	return mode;
}

bool wired_is_ankle(void)
{
	return out_port >= 0;
}

bool wired_is_hub(void)
{
	return (mode & WIRED_HUB) != 0;
}

int wired_hub_tx(const uint8_t *buf, size_t len)
{
	return uart_tx(ports[PORT_LEFT].uart, buf, len, SYS_FOREVER_US);
}

static uint64_t addr_u64(const uint8_t *addr)
{
	uint64_t v = 0;
	memcpy(&v, addr, 6);
	return v;
}

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
/* Output: frames upstream                                                                */
/* ------------------------------------------------------------------------------------- */

// TX queue: the connection thread often sends several packets back to back (rotation, then
// battery/status/button), and a chain tracker also forwards everything from its inputs.
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
		int err = uart_tx(ports[out_port].uart, txq_buf[txq_head], txq_len[txq_head], SYS_FOREVER_US);
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

static void out_uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
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
static bool out_send(const uint8_t *raw, size_t len)
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
	out_send(raw, sizeof(raw)); // if the queue is full, the next sample replaces this one
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
	k_work_reschedule(&hello_work, out_send(raw, sizeof(raw)) ? K_MSEC(1000) : K_MSEC(3));
}

/* ------------------------------------------------------------------------------------- */
/* Inputs: receive, then forward (output) or stamp and queue for ESB (radio relay)        */
/* ------------------------------------------------------------------------------------- */

K_MSGQ_DEFINE(relay_q, WIRED_FRAME_LEN, 32, 1); // a hip relays four trackers at up to ~1000/s

// Receiver ids for wired trackers, by address; stored in NVS, filled by pair_wired.
struct ankle_entry {
	uint8_t addr[6];
	uint8_t id;
	uint8_t used;
};
static struct ankle_entry ankles[CONFIG_WIRED_ANKLE_TABLE_SIZE];

// Wired trackers heard recently, for pairing and the status.
static struct seen_entry {
	uint8_t addr[6];
	uint8_t fw[3];
	uint8_t port;
	bool used, have_fw;
	int64_t last;
	uint32_t frames;
} seen[SEEN_MAX];

static uint32_t st_relayed, st_unpaired, st_qdrop;
static int64_t last_unpaired_log = -UNPAIRED_LOG_MS;

bool wired_relaying(void)
{
	if (wired_is_hub())
		return true; // the hub stays awake, at full rate, with the HF clock on
	for (int i = 0; i < PORT_COUNT; i++)
		if (atomic_get(&ports[i].active))
			return true;
	return false;
}

bool wired_relay_get(uint8_t frame[WIRED_FRAME_LEN])
{
	return k_msgq_get(&relay_q, frame, K_NO_WAIT) == 0;
}

static int lookup_id(const uint8_t *addr)
{
	for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE; i++)
		if (ankles[i].used && memcmp(ankles[i].addr, addr, 6) == 0)
			return ankles[i].id;
	return -1;
}

// Records a wired tracker; a new one replaces the least recently heard. Runs in UART callbacks.
static struct seen_entry *note_seen(int port, const uint8_t *addr)
{
	struct seen_entry *e = NULL, *oldest = &seen[0];
	for (int i = 0; i < SEEN_MAX && !e; i++)
		if (seen[i].used && memcmp(seen[i].addr, addr, 6) == 0)
			e = &seen[i];
	bool is_new = e == NULL;
	for (int i = 0; i < SEEN_MAX && !e; i++) {
		if (!seen[i].used)
			e = &seen[i];
		else if (seen[i].last < oldest->last)
			oldest = &seen[i];
	}
	if (!e)
		e = oldest;
	if (is_new) {
		memset(e, 0, sizeof(*e));
		memcpy(e->addr, addr, 6);
		e->used = true;
		int id = lookup_id(addr);
		if (out_port >= 0 || wired_is_hub()) // no pairing: the relay or hub.c gives the id
			LOG_INF("Wired tracker %012llX on the %s header", addr_u64(addr), port_names[port]);
		else if (id >= 0)
			LOG_INF("Wired tracker %012llX on the %s header, id %d", addr_u64(addr), port_names[port], id);
		else
			LOG_WRN("Wired tracker %012llX on the %s header, not paired: run pair_wired", addr_u64(addr),
				port_names[port]);
	}
	e->port = (uint8_t)port;
	e->last = k_uptime_get();
	return e;
}

// Output: passes a checked frame from an input on, unchanged. Runs in the input's UART callback;
// out_send() is safe there. Dropped (and counted) if the TX queue is full.
static void forward(const uint8_t *raw, size_t len)
{
	if (out_send(raw, len))
		st_relayed++;
	else
		st_qdrop++;
}

static void relay_frame(uint8_t *frame, int id)
{
	// Stamp the real id and recompute the CRC; the tracker's sequence byte is kept.
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
	connection_wake();
}

static void drop_unpaired(const uint8_t *addr)
{
	st_unpaired++;
	if (k_uptime_get() - last_unpaired_log >= UNPAIRED_LOG_MS) {
		last_unpaired_log = k_uptime_get();
		LOG_WRN("Wired tracker %012llX not paired, dropping its data: run pair_wired", addr_u64(addr));
	}
}

static void handle_hello(struct port *p, const uint8_t *d)
{
	memcpy(p->hello_addr, &d[1], 6);
	p->hello_time = k_uptime_get();
	struct seen_entry *e = note_seen(p - ports, &d[1]);
	memcpy(e->fw, &d[7], 3);
	e->have_fw = true;
	if (out_port >= 0)
		forward(d, HELLO_LEN);
}

// Addressed data from an input, CRC already checked.
static void handle_adata(struct port *p, const uint8_t *d)
{
	note_seen(p - ports, &d[1])->frames++;
	if (out_port >= 0) {
		forward(d, ADATA_LEN);
		return;
	}
	if (wired_is_hub()) {
		hub_put_wired(&d[1], &d[7]);
		st_relayed++;
		return;
	}
	int id = lookup_id(&d[1]);
	if (id < 0) {
		drop_unpaired(&d[1]);
		return;
	}
	uint8_t frame[WIRED_FRAME_LEN];
	memcpy(frame, &d[7], 16);
	frame[20] = d[27];
	relay_frame(frame, id);
}

// Legacy DATA (older ankle firmware): no address, so use the one from this input's HELLO.
static void handle_data(struct port *p, uint8_t *d)
{
	uint8_t *frame = &d[1]; // packet[16], CRC32, sequence
	if (p->hello_time == 0) {
		st_unpaired++;
		return;
	}
	note_seen(p - ports, p->hello_addr)->frames++;
	if (out_port >= 0) {
		uint8_t raw[ADATA_LEN];
		build_adata(raw, p->hello_addr, frame, frame[20]);
		forward(raw, sizeof(raw));
		return;
	}
	if (wired_is_hub()) {
		hub_put_wired(p->hello_addr, frame);
		st_relayed++;
		return;
	}
	int id = lookup_id(p->hello_addr);
	if (id < 0) {
		drop_unpaired(p->hello_addr);
		return;
	}
	relay_frame(frame, id);
}

// A valid frame arrived: keep listening, and count as relaying (keeps this tracker awake).
static void frame_valid(struct port *p)
{
	if (p->session_from_probe && !p->session_found)
		p->st_probe_found++;
	p->session_found = true;
	p->st_frames++;
	atomic_set(&p->active, 1);
	k_work_reschedule(&p->idle_work, K_MSEC(CONFIG_WIRED_RELAY_IDLE_MS));
}

static void frame_done(struct port *p)
{
	uint8_t d[MAX_ENCODED];
	size_t r = 0, w = 0;
	bool ok = !p->enc_over;
	// COBS decode
	while (ok && r < p->enc_len) {
		uint8_t code = p->enc[r++];
		for (uint8_t i = 1; ok && i < code; i++) {
			if (r >= p->enc_len || w >= sizeof(d))
				ok = false;
			else
				d[w++] = p->enc[r++];
		}
		if (ok && code != 0xFF && r < p->enc_len) {
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
			frame_valid(p);
			handle_data(p, d);
			return;
		}
	} else if (ok && w == ADATA_LEN && d[0] == TYPE_ADATA) {
		uint32_t crc;
		memcpy(&crc, &d[23], sizeof(crc));
		if (crc == crc32_k_4_2_update(0x93a409eb, &d[1], ADATA_CRC_LEN)) {
			frame_valid(p);
			handle_adata(p, d);
			return;
		}
	} else if (ok && w == HELLO_LEN && d[0] == TYPE_HELLO) {
		frame_valid(p);
		handle_hello(p, d);
		return;
	}
	p->st_bad++;
}

static void feed(struct port *p, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t c = buf[i];
		if (c == 0) {
			if (p->enc_synced && p->enc_len > 0)
				frame_done(p);
			p->enc_synced = true; // the first frame after waking is usually partial
			p->enc_len = 0;
			p->enc_over = false;
		} else if (p->enc_len < sizeof(p->enc)) {
			p->enc[p->enc_len++] = c;
		} else {
			p->enc_over = true;
		}
	}
}

static void in_uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	struct port *p = user_data;
	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED: // hub mode: the left UART also sends the hub output
		hub_tx_done();
		break;
	case UART_RX_RDY:
		p->st_rx_bytes += evt->data.rx.len;
		feed(p, evt->data.rx.buf + evt->data.rx.offset, evt->data.rx.len);
		break;
	case UART_RX_BUF_REQUEST:
		uart_rx_buf_rsp(dev, p->rx_bufs[p->rx_next_buf], RX_BUF_SIZE);
		p->rx_next_buf ^= 1;
		break;
	case UART_RX_STOPPED:
		p->st_line_err++; // break or framing error, e.g. cable unplugged; RX_DISABLED follows
		break;
	case UART_RX_DISABLED:
		atomic_clear(&p->rx_on);
		atomic_clear(&p->active);
		k_work_cancel_delayable(&p->idle_work);
		// An always-on input listens again at once (no suspend: its UART may be sending).
		k_work_submit(p->always_on ? &p->start_rx_work : &p->rearm_work);
		break;
	default:
		break;
	}
}

static void wake_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	struct port *p = CONTAINER_OF(cb, struct port, wake_cb);
	gpio_pin_interrupt_configure_dt(&p->data_pin, GPIO_INT_DISABLE);
	p->st_wakes++;
	k_work_submit(&p->start_rx_work);
}

static void rearm_work_fn(struct k_work *work)
{
	struct port *p = CONTAINER_OF(work, struct port, rearm_work);
	// Release the pin: a suspended UARTE leaves it to the GPIO, so the edge detector sees it.
	pm_device_action_run(p->uart, PM_DEVICE_ACTION_SUSPEND);
	gpio_pin_configure_dt(&p->data_pin, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&p->data_pin, GPIO_INT_EDGE_TO_ACTIVE);
}

static void start_rx(struct port *p, bool from_probe)
{
	if (!atomic_cas(&p->rx_on, 0, 1))
		return;
	gpio_pin_interrupt_configure_dt(&p->data_pin, GPIO_INT_DISABLE);
	p->session_from_probe = from_probe;
	p->session_found = false;
	pm_device_action_run(p->uart, PM_DEVICE_ACTION_RESUME);
	p->enc_len = 0;
	p->enc_over = false;
	p->enc_synced = false;
	p->rx_next_buf = 1;
	p->st_rx_starts++;
	int err = uart_rx_enable(p->uart, p->rx_bufs[0], RX_BUF_SIZE, RX_TIMEOUT_US);
	if (err != 0) {
		p->st_rx_enable_err++;
		LOG_ERR("Wired UART RX enable failed on the %s header: %d", port_names[p - ports], err);
		atomic_clear(&p->rx_on);
		k_work_submit(&p->rearm_work);
		return;
	}
	// Give up again if nothing valid arrives (noise on an open connector, or an empty probe).
	k_work_reschedule(&p->idle_work, K_MSEC(from_probe ? PROBE_WINDOW_MS : CONFIG_WIRED_RELAY_IDLE_MS));
}

static void start_rx_work_fn(struct k_work *work)
{
	start_rx(CONTAINER_OF(work, struct port, start_rx_work), false);
}

static void probe_work_fn(struct k_work *work)
{
	struct port *p = CONTAINER_OF(k_work_delayable_from_work(work), struct port, probe_work);
	if (!atomic_get(&p->rx_on)) {
		p->st_probes++;
		start_rx(p, true);
	}
	k_work_reschedule(&p->probe_work, K_MSEC(PROBE_PERIOD_MS));
}

static void idle_work_fn(struct k_work *work)
{
	struct port *p = CONTAINER_OF(k_work_delayable_from_work(work), struct port, idle_work);
	if (atomic_get(&p->active))
		LOG_INF("Wired link on the %s header idle", port_names[p - ports]);
	if (p->always_on) {
		atomic_clear(&p->active); // keep listening
		return;
	}
	if (atomic_get(&p->rx_on) && uart_rx_disable(p->uart) != 0) {
		// Already stopping or stopped; make sure the edge detector is armed.
		atomic_clear(&p->rx_on);
		atomic_clear(&p->active);
		k_work_submit(&p->rearm_work);
	}
}

static void in_start(struct port *p)
{
	k_work_init(&p->start_rx_work, start_rx_work_fn);
	k_work_init(&p->rearm_work, rearm_work_fn);
	k_work_init_delayable(&p->idle_work, idle_work_fn);
	k_work_init_delayable(&p->probe_work, probe_work_fn);
	if (device_init(p->uart) != 0 || !device_is_ready(p->uart) ||
	    uart_callback_set(p->uart, in_uart_cb, p) != 0) {
		LOG_ERR("Wired UART for the %s header not ready", port_names[p - ports]);
		return;
	}
	if (p->always_on) {
		start_rx(p, false); // no wake edge or probes: the UART stays on
		return;
	}
	if (!gpio_is_ready_dt(&p->data_pin)) {
		LOG_ERR("Wired data pin on the %s header not ready", port_names[p - ports]);
		return;
	}
	gpio_init_callback(&p->wake_cb, wake_isr, BIT(p->data_pin.pin));
	gpio_add_callback(p->data_pin.port, &p->wake_cb);
	rearm_work_fn(&p->rearm_work);
	k_work_reschedule(&p->probe_work, K_MSEC(PROBE_PERIOD_MS));
}

/* ---- output start: transmit pins for the header's UART ---- */

#if !defined(WIRED_HOST_TEST)
#include <zephyr/drivers/pinctrl.h>

PINCTRL_DT_DEV_CONFIG_DECLARE(DT_CHOSEN(slimenrf_wired_left_uart));
PINCTRL_DT_DEV_CONFIG_DECLARE(DT_CHOSEN(slimenrf_wired_right_uart));

PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), wired_left_tx_default);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), wired_right_tx_default);
#ifdef CONFIG_PM_DEVICE
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), wired_left_tx_sleep);
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), wired_right_tx_sleep);
#endif

static const struct pinctrl_state left_tx_states[] = {
	PINCTRL_DT_STATE_INIT(wired_left_tx_default, PINCTRL_STATE_DEFAULT),
#ifdef CONFIG_PM_DEVICE
	PINCTRL_DT_STATE_INIT(wired_left_tx_sleep, PINCTRL_STATE_SLEEP),
#endif
};

static const struct pinctrl_state right_tx_states[] = {
	PINCTRL_DT_STATE_INIT(wired_right_tx_default, PINCTRL_STATE_DEFAULT),
#ifdef CONFIG_PM_DEVICE
	PINCTRL_DT_STATE_INIT(wired_right_tx_sleep, PINCTRL_STATE_SLEEP),
#endif
};

// The UARTs receive on their header's data pin by default; an output transmits there instead.
static int use_tx_pins(int port)
{
	if (port == PORT_LEFT)
		return pinctrl_update_states(PINCTRL_DT_DEV_CONFIG_GET(DT_CHOSEN(slimenrf_wired_left_uart)),
					     left_tx_states, ARRAY_SIZE(left_tx_states));
	return pinctrl_update_states(PINCTRL_DT_DEV_CONFIG_GET(DT_CHOSEN(slimenrf_wired_right_uart)),
				     right_tx_states, ARRAY_SIZE(right_tx_states));
}

// Hub mode: the left UART receives the left header (D9) and sends the hub output (D15).
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), wired_hub_default);
#ifdef CONFIG_PM_DEVICE
PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), wired_hub_sleep);
#endif

static const struct pinctrl_state hub_states[] = {
	PINCTRL_DT_STATE_INIT(wired_hub_default, PINCTRL_STATE_DEFAULT),
#ifdef CONFIG_PM_DEVICE
	PINCTRL_DT_STATE_INIT(wired_hub_sleep, PINCTRL_STATE_SLEEP),
#endif
};

static int use_hub_pins(void)
{
	return pinctrl_update_states(PINCTRL_DT_DEV_CONFIG_GET(DT_CHOSEN(slimenrf_wired_left_uart)), hub_states,
				     ARRAY_SIZE(hub_states));
}
#else
static int use_tx_pins(int port)
{
	(void)port;
	return 0;
}

static int use_hub_pins(void)
{
	return 0;
}
#endif

static void out_start(struct port *p)
{
	int err = use_tx_pins(p - ports);
	if (err != 0 || device_init(p->uart) != 0 || !device_is_ready(p->uart) ||
	    uart_callback_set(p->uart, out_uart_cb, NULL) != 0) {
		LOG_ERR("Wired output on the %s header not ready (%d)", port_names[p - ports], err);
		return;
	}
	uart_ok = true;
	k_work_reschedule(&hello_work, K_MSEC(100));
}

/* ---- id table and pairing ---- */

static void store_id(const uint8_t *addr, uint8_t id)
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

// Addresses heard in the last HELLO_VALID_MS that have no receiver id yet; returns how many.
static int pair_candidates(uint8_t addrs[SEEN_MAX][6], int *heard)
{
	int n = 0;
	*heard = 0;
	int64_t now = k_uptime_get();
	unsigned int key = irq_lock();
	for (int i = 0; i < SEEN_MAX; i++) {
		if (!seen[i].used || now - seen[i].last > HELLO_VALID_MS)
			continue;
		(*heard)++;
		if (lookup_id(seen[i].addr) < 0)
			memcpy(addrs[n++], seen[i].addr, 6);
	}
	irq_unlock(key);
	return n;
}

static K_SEM_DEFINE(pair_sem, 0, 1);

void wired_request_pair_ankle(void)
{
	if (wired_is_hub()) {
		LOG_INF("Hub mode gives wired trackers their ids by itself; pairing is for wireless ones");
		hub_pair_open();
		return;
	}
	if (out_port >= 0) {
		LOG_WRN("A tracker with a wired output needs no pairing: the radio relay pairs it");
		return;
	}
	k_sem_give(&pair_sem);
}

static void pair_thread(void)
{
	while (1) {
		k_sem_take(&pair_sem, K_FOREVER);
		uint8_t addrs[SEEN_MAX][6];
		int heard;
		int n = pair_candidates(addrs, &heard);
		if (heard == 0) {
			LOG_WRN("No wired tracker connected (nothing heard in the last %d s)", HELLO_VALID_MS / 1000);
			continue;
		}
		if (n == 0) {
			LOG_INF("All %d wired trackers are paired (forget_wired to pair them again)", heard);
			continue;
		}
		LOG_INF("Pairing %d wired tracker(s) through this relay, receiver must be in pairing mode", n);
		for (int i = 0; i < n; i++) {
			uint8_t id;
			int err = esb_pair_other(addrs[i], &id);
			if (err) {
				LOG_WRN("Wired tracker %012llX: pairing failed: %d", addr_u64(addrs[i]), err);
				continue;
			}
			store_id(addrs[i], id);
			LOG_INF("Wired tracker %012llX paired as id %u", addr_u64(addrs[i]), id);
		}
	}
}

K_THREAD_DEFINE(wired_pair_thread_id, 1024, pair_thread, NULL, NULL, NULL, 7, 0, 0);

void wired_forget_ankles(void)
{
	memset(ankles, 0, sizeof(ankles));
	sys_write(WIRED_ANKLES_ID, NULL, ankles, sizeof(ankles));
	printk("Wired tracker table cleared\n");
}

/* ---- status ---- */

#if !defined(WIRED_HOST_TEST)
// Raw register dump of a header's UARTE and its data pin, for debugging the physical link.
static void print_uarte_regs(uint32_t base, bool tx)
{
	NRF_UARTE_Type *u = (NRF_UARTE_Type *)base;
	uint32_t psel = tx ? u->PSEL.TXD : u->PSEL.RXD;
	printk("  UARTE@%08x ENABLE=%u PSEL.%s=0x%08x BAUDRATE=0x%08x", base, u->ENABLE, tx ? "TXD" : "RXD", psel,
	       u->BAUDRATE);
	if (psel & 0x80000000u) {
		printk(", pin disconnected\n");
		return;
	}
	uint32_t port = (psel >> 5) & 1, pin = psel & 31;
	NRF_GPIO_Type *g = port ? NRF_P1 : NRF_P0;
	printk(", P%u.%02u PIN_CNF=0x%08x OUT=%u IN=%u\n", port, pin, g->PIN_CNF[pin], (g->OUT >> pin) & 1,
	       (g->IN >> pin) & 1);
}
#endif

static void print_port(int i)
{
	struct port *p = &ports[i];
	printk("%s header (data %s): ", i == PORT_LEFT ? "Left" : "Right", port_pins[i]);
	if (p->out) {
		printk("output, sent %u data frames, %u HELLOs; dropped %u (queue full), errors %u (last %d)\n",
		       tx_frames, tx_hellos, tx_dropped, tx_errors, tx_last_err);
	} else if (p->in) {
		printk("input, %s, UART %s\n", atomic_get(&p->active) ? "receiving" : "idle",
		       atomic_get(&p->rx_on) ? "on" : "off");
		printk("  frames %u, bad %u, line errors %u, bytes %u; wake edges %u, probes %u (found %u), "
		       "UART starts %u (failed %u)\n", p->st_frames, p->st_bad, p->st_line_err, p->st_rx_bytes,
		       p->st_wakes, p->st_probes, p->st_probe_found, p->st_rx_starts, p->st_rx_enable_err);
		if (!atomic_get(&p->rx_on)) {
			// Sample the data pin directly: a sending tracker pulls it low for part of the time.
			uint32_t low = 0;
			for (int k = 0; k < 50000; k++)
				low += gpio_pin_get_raw(p->data_pin.port, p->data_pin.pin) == 0;
			printk("  data pin sampled 50000 times: %u low (0 = no signal)\n", low);
		}
	} else {
		printk("unused\n");
		return;
	}
#if !defined(WIRED_HOST_TEST)
	print_uarte_regs(i == PORT_LEFT ? DT_REG_ADDR(DT_CHOSEN(slimenrf_wired_left_uart))
					: DT_REG_ADDR(DT_CHOSEN(slimenrf_wired_right_uart)), p->out);
#endif
}

void wired_print_status(void)
{
	char name[40];
	wired_mode_format(mode, name, sizeof(name));
	printk("Mode: %s (radio %s)%s\n", name,
	       out_port >= 0 ? "off" : wired_is_hub() ? "receiving for the hub" : mode ? "on, relaying the inputs" : "on",
	       CONFIG_2_SETTINGS_READ(CONFIG_2_WIRED_MODE) == WIRED_MODE_LEGACY ? ", from connection_over_uart" : "");
	for (int i = 0; i < PORT_COUNT; i++)
		print_port(i);
	bool inputs = mode & (WIRED_IN_LEFT | WIRED_IN_RIGHT);
	if (inputs)
		printk("%s %u, unpaired %u, queue drops %u\n",
		       out_port >= 0 ? "Forwarded" : wired_is_hub() ? "To the hub" : "Relayed", st_relayed, st_unpaired,
		       st_qdrop);
	int64_t now = k_uptime_get();
	bool any = false;
	for (int i = 0; i < SEEN_MAX; i++) {
		struct seen_entry e = seen[i];
		if (!e.used)
			continue;
		if (!any)
			printk("Wired trackers heard:\n");
		any = true;
		printk("  %012llX on the %s header", addr_u64(e.addr), port_names[e.port]);
		if (e.have_fw)
			printk(", fw %u.%u.%u", e.fw[0], e.fw[1], e.fw[2]);
		printk(", %u frames, last %lld ms ago", e.frames, now - e.last);
		if (out_port < 0 && !wired_is_hub()) { // the hub lists its ids below
			int id = lookup_id(e.addr);
			if (id >= 0)
				printk(", id %d", id);
			else
				printk(", not paired");
		}
		printk("\n");
	}
	if (!any && inputs)
		printk("Wired trackers heard: none\n");
	if (wired_is_hub()) {
		hub_print_status();
		return;
	}
	if (out_port < 0 && inputs) {
		printk("Paired wired trackers:\n");
		for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE; i++)
			if (ankles[i].used)
				printk("  %012llX -> id %u\n", addr_u64(ankles[i].addr), ankles[i].id);
	}
}

/* ------------------------------------------------------------------------------------- */
/* Roles at boot                                                                          */
/* ------------------------------------------------------------------------------------- */

#if !defined(WIRED_HOST_TEST)
#include <hal/nrf_gpio.h>
// Drive both headers' ground pins low as early as possible (high drive for low ohms).
static int wired_gnd_init(void)
{
	uint32_t pins[] = {
		NRF_DT_GPIOS_TO_PSEL(DT_PATH(zephyr_user), wired_left_gnd_gpios),
		NRF_DT_GPIOS_TO_PSEL(DT_PATH(zephyr_user), wired_right_gnd_gpios),
	};
	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		nrf_gpio_pin_clear(pins[i]);
		nrf_gpio_cfg(pins[i], NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT, NRF_GPIO_PIN_NOPULL,
			     NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	}
	return 0;
}

SYS_INIT(wired_gnd_init, PRE_KERNEL_1, 0);
#endif

// Priority 99: after the settings were loaded from retained RAM / NVS (system.c),
// and before the ESB and connection threads start.
static int wired_init(void)
{
	mode = CONFIG_2_SETTINGS_READ(CONFIG_2_WIRED_MODE);
	if (mode == WIRED_MODE_LEGACY)
		mode = CONFIG_0_SETTINGS_READ(CONFIG_0_CONNECTION_OVER_UART) ? WIRED_IN_LEFT | WIRED_OUT_RIGHT
									       : WIRED_IN_LEFT;
	if (!wired_mode_valid(mode)) {
		LOG_ERR("wired_mode %d is not valid, running wireless", mode);
		mode = 0;
	}
	out_port = (mode & WIRED_OUT_LEFT) ? PORT_LEFT : (mode & WIRED_OUT_RIGHT) ? PORT_RIGHT : -1;
	ports[PORT_LEFT].in = mode & WIRED_IN_LEFT;
	ports[PORT_RIGHT].in = mode & WIRED_IN_RIGHT;
	ports[PORT_LEFT].out = out_port == PORT_LEFT;
	ports[PORT_RIGHT].out = out_port == PORT_RIGHT;
	sys_read(WIRED_ANKLES_ID, ankles, sizeof(ankles));

	char name[40];
	wired_mode_format(mode, name, sizeof(name));
	LOG_INF("Mode: %s, radio %s", name, out_port >= 0 ? "off" : wired_is_hub() ? "receiving" : "on");
	if (wired_is_hub()) {
		ports[PORT_LEFT].always_on = ports[PORT_RIGHT].always_on = true;
		int err = use_hub_pins();
		if (err)
			LOG_ERR("Hub output pins not set: %d", err);
	}
	if (out_port >= 0)
		out_start(&ports[out_port]);
	for (int i = 0; i < PORT_COUNT; i++)
		if (ports[i].in)
			in_start(&ports[i]);
	if (wired_is_hub())
		hub_start();
	return 0;
}

SYS_INIT(wired_init, APPLICATION, 99);

#endif
