// Host test of src/connection/wired.c: ankle encoder -> wire bytes -> relay decoder/stamper.
#define CONFIG_WIRED_LINK 1
#define WIRED_HOST_TEST 1
#define CONFIG_WIRED_RELAY_IDLE_MS 1000
#define CONFIG_WIRED_ANKLE_TABLE_SIZE 4
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#include "wired.c"

int64_t sim_now_ms = 1000;
int sim_role_ankle;
int sim_device_inits;
struct device sim_uart = {"uart0"};
uart_callback_t sim_cb;
uint8_t sim_tx[4096];
size_t sim_tx_len;
struct sim_ficr sim_ficr = {{0x44332211u, 0x0000C655u}}; // address 55C6...: bytes 11 22 33 44 55 C6
static uint8_t nvs[64];
void sys_write(uint16_t id, void *ptr, const void *data, size_t len) { (void)id; (void)ptr; memcpy(nvs, data, len); }
void sys_read(uint16_t id, void *data, size_t len) { (void)id; memcpy(data, nvs, len); }
static int wakes;
void connection_wake(void) { wakes++; }
int esb_pair_other(const uint8_t addr[6], uint8_t *id_out) { (void)addr; *id_out = 5; return 0; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Build an ESB frame the way connection_thread() does on an ankle.
static void make_frame(uint8_t *f, uint8_t type, uint8_t seq, uint8_t fill)
{
	memset(f, 0, WIRED_FRAME_LEN);
	f[0] = type;
	f[1] = WIRED_ID_PLACEHOLDER;
	for (int i = 2; i < 16; i++)
		f[i] = (uint8_t)(fill + i * 37); // includes zeros now and then
	f[20] = seq;
	uint32_t crc = crc32_k_4_2_update(0x93a409eb, f, 16);
	memcpy(&f[16], &crc, 4);
}

// Ankle side: frames go into sim_tx. The TX-done callback is delivered at once.
static void ankle_send_data(uint8_t seq, uint8_t fill)
{
	uint8_t f[WIRED_FRAME_LEN];
	make_frame(f, 1, seq, fill);
	wired_write_data(f);
	struct uart_event e = {.type = UART_TX_DONE};
	ankle_uart_cb(&sim_uart, &e, NULL);
}

static void ankle_send_hello(void)
{
	hello_work_fn(NULL);
	struct uart_event e = {.type = UART_TX_DONE};
	ankle_uart_cb(&sim_uart, &e, NULL);
}

static void relay_feed(const uint8_t *b, size_t n)
{
	uint8_t tmp[4096];
	memcpy(tmp, b, n);
	struct uart_event e = {.type = UART_RX_RDY};
	e.data.rx.buf = tmp;
	e.data.rx.offset = 0;
	e.data.rx.len = n;
	relay_uart_cb(&sim_uart, &e, NULL);
}

int main(void)
{
	uint8_t out[WIRED_FRAME_LEN];

	// Role from the setting: ankle starts only the TX UART, relay only the RX UART.
	sim_role_ankle = 1;
	CHECK(wired_init_ptr() == 0 && wired_is_ankle() && sim_device_inits == 1 && uart_ok, "ankle role");
	int pair_before = pair_sem.count;
	wired_request_pair_ankle(); // refused on an ankle
	CHECK(pair_sem.count == pair_before, "ankle must not start pairing");

	// Ankle: one HELLO and a DATA frame; check sizes and that both sides agree.
	sim_tx_len = 0;
	ankle_send_hello();
	size_t hello_len = sim_tx_len;
	ankle_send_data(7, 0);
	size_t data_len = sim_tx_len - hello_len;
	printf("HELLO %zu bytes, DATA %zu bytes on the wire\n", hello_len, data_len);
	CHECK(data_len <= 24, "DATA frame too long: %zu", data_len);
	CHECK(sim_tx[hello_len - 1] == 0 && sim_tx[sim_tx_len - 1] == 0, "missing delimiters");

	// TX queue: frames sent while one is on the wire are queued, not dropped.
	size_t before = sim_tx_len;
	uint8_t f[WIRED_FRAME_LEN];
	make_frame(f, 1, 8, 0);
	wired_write_data(f);
	wired_write_data(f); // queued behind the first
	CHECK(sim_tx_len - before == data_len, "only one frame on the wire while busy: %zu", sim_tx_len - before);
	struct uart_event done = {.type = UART_TX_DONE};
	ankle_uart_cb(&sim_uart, &done, NULL); // first done -> second starts
	CHECK(sim_tx_len - before == 2 * data_len && tx_dropped == 0, "queued frame not sent");
	ankle_uart_cb(&sim_uart, &done, NULL);
	// Queue full: 4 queued while busy, the 5th is dropped.
	before = sim_tx_len;
	for (int i = 0; i < 5; i++)
		wired_write_data(f);
	CHECK(tx_dropped == 1, "queue full drop: %u", tx_dropped);
	for (int i = 0; i < 4; i++)
		ankle_uart_cb(&sim_uart, &done, NULL);
	CHECK(sim_tx_len - before == 4 * data_len && txq_count == 0, "queue drained: %zu bytes, %u left", sim_tx_len - before, txq_count);
	// Drop the extra frames from the captured wire so the relay test below sees a clean stream.
	sim_tx_len -= 5 * data_len;

	uint8_t wire[4096];
	size_t wire_len = sim_tx_len;
	memcpy(wire, sim_tx, wire_len);

	// Relay: unpaired ankle. A partial frame before the first delimiter is not counted as bad.
	sim_role_ankle = 0;
	is_ankle = false;
	wired_relay_start();
	CHECK(!wired_is_ankle() && sim_device_inits == 2, "relay role");
	start_rx_work_fn(NULL);
	uint8_t junk[] = {0x13, 0x37, 0x00};
	relay_feed(junk, sizeof(junk));
	relay_feed(wire, wire_len);
	CHECK(st_bad == 0, "bad frames: %u", st_bad);
	CHECK(ankle_id == -1 && st_unpaired == 2 && !wired_relay_get(out), "unpaired ankle must be dropped (id %d, unpaired %u)", ankle_id, st_unpaired);
	CHECK(hello_addr[0] == 0x11 && hello_addr[5] == 0xC6 && hello_fw[2] == 3, "HELLO contents");

	// Pair through the relay (esb_pair_other stub returns id 5).
	wired_request_pair_ankle();
	{
		// run one iteration of pair_thread's body
		uint8_t addr[6], id;
		memcpy(addr, hello_addr, 6);
		CHECK(esb_pair_other(addr, &id) == 0, "pair");
		store_ankle(addr, id);
		if (memcmp(addr, hello_addr, 6) == 0)
			ankle_id = id;
	}
	CHECK(ankle_id == 5 && ankles[0].used && ankles[0].id == 5, "table after pairing");

	// Reboot of the relay: table comes back from NVS; HELLO restores the id.
	memset(ankles, 0, sizeof(ankles));
	ankle_id = -1;
	wired_relay_start();
	start_rx_work_fn(NULL);
	atomic_clear(&rx_on);
	start_rx_work_fn(NULL);
	sim_tx_len = 0;
	ankle_send_hello();
	for (int s = 0; s < 3; s++)
		ankle_send_data((uint8_t)(10 + s), (uint8_t)s);
	uint8_t sync0 = 0;
	relay_feed(&sync0, 1);
	relay_feed(sim_tx, sim_tx_len);
	CHECK(ankle_id == 5, "id after reboot: %d", ankle_id);
	CHECK(wired_relaying() && wakes >= 3, "relaying %d wakes %d", (int)wired_relaying(), wakes);

	// Stamped frames: id 5, CRC valid for the receiver, sequence and payload kept.
	for (int s = 0; s < 3; s++) {
		uint8_t ref[WIRED_FRAME_LEN];
		make_frame(ref, 1, (uint8_t)(10 + s), (uint8_t)s);
		CHECK(wired_relay_get(out), "frame %d missing", s);
		uint32_t crc;
		memcpy(&crc, &out[16], 4);
		CHECK(out[1] == 5, "id byte %u", out[1]);
		CHECK(crc == crc32_k_4_2_update(0x93a409eb, out, 16), "relayed CRC invalid");
		CHECK(out[20] == 10 + s, "sequence %u", out[20]);
		CHECK(out[0] == ref[0] && memcmp(&out[2], &ref[2], 14) == 0, "payload changed");
	}
	CHECK(!wired_relay_get(out), "queue should be empty");

	// Queue overflow: 6 frames without draining keeps the newest 4.
	sim_tx_len = 0;
	for (int s = 0; s < 6; s++)
		ankle_send_data((uint8_t)(20 + s), 0);
	uint32_t qd = st_qdrop;
	relay_feed(sim_tx, sim_tx_len);
	CHECK(st_qdrop - qd == 2, "queue drops %u", st_qdrop - qd);
	CHECK(wired_relay_get(out) && out[20] == 22, "oldest kept should be seq 22, got %u", out[20]);

	// Corruption: a flipped bit fails CRC32 and is counted as bad.
	while (wired_relay_get(out)) {}
	sim_tx_len = 0;
	ankle_send_data(30, 0);
	sim_tx[5] ^= 0x40;
	if (sim_tx[5] == 0)
		sim_tx[5] = 0x40;
	uint32_t bad = st_bad;
	relay_feed(sim_tx, sim_tx_len);
	CHECK(st_bad == bad + 1 && !wired_relay_get(out), "corrupted frame accepted");

	// RX disabled (idle or line error): relaying stops and the edge detector is re-armed.
	struct uart_event dis = {.type = UART_RX_DISABLED};
	int rearm_before = rearm_work.submitted;
	relay_uart_cb(&sim_uart, &dis, NULL);
	CHECK(!wired_relaying() && rearm_work.submitted == rearm_before + 1, "disable handling");

	printf(failures ? "%d FAILURE(S)\n" : "all wired link tests passed\n", failures);
	return failures != 0;
}
