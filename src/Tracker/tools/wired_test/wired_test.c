// Host test of src/connection/wired.c: the mode words, a wired tracker's output (encoder),
// a chain tracker forwarding its input, and a hip relaying two inputs with several trackers.
#define CONFIG_WIRED_LINK 1
#define WIRED_HOST_TEST 1
#define CONFIG_WIRED_RELAY_IDLE_MS 1000
#define CONFIG_WIRED_ANKLE_TABLE_SIZE 8
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#include "wired.c"

int64_t sim_now_ms = 1000;
int sim_role_ankle;
int sim_wired_mode = WIRED_MODE_LEGACY;
int sim_device_inits;
struct device sim_uarts[2] = {{"uart1"}, {"uart0"}};
uart_callback_t sim_cb;
uint8_t sim_tx[4096];
size_t sim_tx_len;
struct sim_ficr sim_ficr = {{0x44332211u, 0x0000C655u}}; // address bytes 11 22 33 44 55 C6
static uint8_t nvs[128];
void sys_write(uint16_t id, void *ptr, const void *data, size_t len) { (void)id; (void)ptr; memcpy(nvs, data, len); }
void sys_read(uint16_t id, void *data, size_t len) { (void)id; memcpy(data, nvs, len); }
static int wakes;
void connection_wake(void) { wakes++; }
static uint8_t next_pair_id = 10;
int esb_pair_other(const uint8_t addr[6], uint8_t *id_out) { (void)addr; *id_out = next_pair_id++; return 0; }
// hub.c stand-ins: record what wired.c hands over
static int hub_started, hub_tx_dones, hub_wired_count, hub_pair_opens;
static uint8_t hub_last_addr[6], hub_last_pkt[16];
void hub_start(void) { hub_started++; }
void hub_tx_done(void) { hub_tx_dones++; }
void hub_pair_open(void) { hub_pair_opens++; }
void hub_print_status(void) {}
void hub_put_wired(const uint8_t addr[6], const uint8_t *packet)
{
	hub_wired_count++;
	memcpy(hub_last_addr, addr, 6);
	memcpy(hub_last_pkt, packet, 16);
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Back to power-on state, then boot with the given setting.
static int boot(int wired_mode_setting, int legacy_role)
{
	for (int i = 0; i < PORT_COUNT; i++) {
		struct port keep = ports[i];
		memset(&ports[i], 0, sizeof(ports[i]));
		ports[i].uart = keep.uart;
		ports[i].data_pin = keep.data_pin;
	}
	out_port = -1;
	mode = 0;
	txq_head = txq_count = 0;
	tx_active = uart_ok = false;
	tx_dropped = tx_frames = tx_hellos = tx_errors = 0;
	memset(seen, 0, sizeof(seen));
	st_relayed = st_unpaired = st_qdrop = 0;
	uint8_t junk[WIRED_FRAME_LEN];
	while (k_msgq_get(&relay_q, junk, K_NO_WAIT) == 0) {}
	sim_device_inits = 0;
	sim_wired_mode = wired_mode_setting;
	sim_role_ankle = legacy_role;
	return wired_init_ptr();
}

static void tx_done_all(void)
{
	struct uart_event done = {.type = UART_TX_DONE};
	while (txq_count)
		out_uart_cb(ports[out_port].uart, &done, NULL);
}

// Build an ESB frame the way connection_thread() does on a wired tracker.
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

static void set_addr(uint8_t last)
{
	sim_ficr.DEVICEADDR[0] = 0x44332211u;
	sim_ficr.DEVICEADDR[1] = 0x00005500u | last;
}

// Wire bytes a wired tracker with address ..55<last> sends: a HELLO, then frames seq0, seq0+1, ...
// Uses this file's own output path (out_port must be set), so encoder and decoder are both tested.
static size_t tracker_wire(uint8_t last, uint8_t seq0, int frames, uint8_t *buf)
{
	set_addr(last);
	sim_tx_len = 0;
	hello_work_fn(NULL);
	tx_done_all();
	for (int s = 0; s < frames; s++) {
		uint8_t f[WIRED_FRAME_LEN];
		make_frame(f, 1, (uint8_t)(seq0 + s), (uint8_t)(last + s));
		wired_write_data(f);
		tx_done_all();
	}
	memcpy(buf, sim_tx, sim_tx_len);
	return sim_tx_len;
}

// Decodes the COBS frames in b[0..n) into raw frames; returns how many were found.
static int decode_frames(const uint8_t *b, size_t n, uint8_t raw[][40], size_t *raw_len, int max)
{
	int count = 0;
	size_t start = 0;
	for (size_t i = 0; i < n && count < max; i++) {
		if (b[i] != 0)
			continue;
		size_t r = start, w = 0;
		while (r < i) {
			uint8_t code = b[r++];
			for (uint8_t k = 1; k < code && r < i; k++)
				raw[count][w++] = b[r++];
			if (code != 0xFF && r < i)
				raw[count][w++] = 0;
		}
		if (w)
			raw_len[count++] = w;
		start = i + 1;
	}
	return count;
}

static void input_feed(int port, const uint8_t *b, size_t n)
{
	uint8_t tmp[4096];
	memcpy(tmp, b, n);
	struct uart_event e = {.type = UART_RX_RDY};
	e.data.rx.buf = tmp;
	e.data.rx.offset = 0;
	e.data.rx.len = n;
	in_uart_cb(ports[port].uart, &e, &ports[port]);
}

static void input_open(int port)
{
	atomic_clear(&ports[port].rx_on);
	start_rx_work_fn(&ports[port].start_rx_work);
	uint8_t sync = 0;
	input_feed(port, &sync, 1);
}

static void test_mode_words(void)
{
	struct { const char *words; int mode; } cases[] = {
		{"wireless", 0},
		{"wired out right", WIRED_OUT_RIGHT},
		{"wired in left out right", WIRED_IN_LEFT | WIRED_OUT_RIGHT},
		{"wired out right in left", WIRED_IN_LEFT | WIRED_OUT_RIGHT},
		{"WIRED In Left", WIRED_IN_LEFT},
		{"wired in both", WIRED_IN_LEFT | WIRED_IN_RIGHT},
		{"wired in right out left", WIRED_IN_RIGHT | WIRED_OUT_LEFT},
		{"wired in left out left", -1},
		{"wired in both out right", -1},
		{"wired out both", -1},
		{"wired", -1},
		{"wired in", -1},
		{"wired in middle", -1},
		{"wired in left in right", -1},
		{"wireless now", -1},
		{"hub", WIRED_MODE_HUB},
		{"HUB", WIRED_MODE_HUB},
		{"hub left", -1},
	};
	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		char buf[64], *argv[8];
		int argc = 0;
		strcpy(buf, cases[c].words);
		for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " "))
			argv[argc++] = t;
		int m = wired_mode_parse(argc, argv);
		CHECK(m == cases[c].mode, "\"%s\" parsed as %d, expected %d", cases[c].words, m, cases[c].mode);
		if (m >= 0) {
			char name[40], again[64], *av[8];
			int ac = 0;
			wired_mode_format(m, name, sizeof(name));
			strcpy(again, name);
			for (char *t = strtok(again, " "); t && ac < 8; t = strtok(NULL, " "))
				av[ac++] = t;
			CHECK(wired_mode_parse(ac, av) == m, "\"%s\" does not parse back", name);
		}
	}
	for (int m = 0; m < 16; m++) {
		int in = m & 3, out = m >> 2;
		CHECK(wired_mode_valid(m) == (out != 3 && (in & out) == 0), "valid(%d)", m);
	}
	for (int m = 16; m < 32; m++)
		CHECK(wired_mode_valid(m) == (m == WIRED_MODE_HUB), "valid(%d)", m);
	CHECK(!wired_mode_valid(32) && !wired_mode_valid(-1), "out of range");
}

int main(void)
{
	uint8_t out[WIRED_FRAME_LEN];
	struct uart_event done = {.type = UART_TX_DONE};

	test_mode_words();

	// Older setting: connection_over_uart on = in left out right, off = in left.
	CHECK(boot(WIRED_MODE_LEGACY, 1) == 0 && mode == (WIRED_IN_LEFT | WIRED_OUT_RIGHT) && out_port == PORT_RIGHT,
	      "legacy chain mode %d", mode);
	CHECK(wired_is_ankle() && uart_ok && sim_device_inits == 2, "legacy chain: output and input started");
	CHECK(boot(WIRED_MODE_LEGACY, 0) == 0 && mode == WIRED_IN_LEFT && out_port < 0 && !wired_is_ankle(),
	      "legacy relay mode %d", mode);
	CHECK(boot(13, 0) == 0 && mode == 0, "invalid setting runs wireless");
	CHECK(boot(0, 1) == 0 && mode == 0 && sim_device_inits == 0 && !wired_is_ankle(), "wireless starts no UART");

	// Ankle: wired out right. HELLO and ADATA sizes, layout, CRC; TX queue.
	CHECK(boot(WIRED_OUT_RIGHT, 0) == 0 && out_port == PORT_RIGHT && !ports[PORT_LEFT].in && sim_device_inits == 1,
	      "ankle role");
	wired_request_pair_ankle();
	CHECK(pair_sem.count == 0, "a tracker with an output must not start pairing");
	set_addr(0xC6);
	sim_tx_len = 0;
	hello_work_fn(NULL);
	tx_done_all();
	size_t hello_len = sim_tx_len;
	uint8_t f[WIRED_FRAME_LEN];
	make_frame(f, 1, 7, 0);
	wired_write_data(f);
	tx_done_all();
	size_t data_len = sim_tx_len - hello_len;
	printf("HELLO %zu bytes, ADATA %zu bytes on the wire\n", hello_len, data_len);
	CHECK(data_len <= 31, "ADATA frame too long: %zu", data_len);
	{
		uint8_t raw[2][40];
		size_t rl[2];
		CHECK(decode_frames(sim_tx, sim_tx_len, raw, rl, 2) == 2 && rl[1] == ADATA_LEN && raw[1][0] == TYPE_ADATA,
		      "ADATA layout");
		uint32_t crc;
		memcpy(&crc, &raw[1][23], 4);
		CHECK(raw[1][1] == 0x11 && raw[1][5] == 0xC6 && raw[1][6] == 0x55 && raw[1][27] == 7, "ADATA address/sequence");
		CHECK(crc == crc32_k_4_2_update(0x93a409eb, &raw[1][1], 22), "ADATA CRC");
	}
	size_t before = sim_tx_len;
	wired_write_data(f);
	wired_write_data(f); // queued behind the first
	CHECK(sim_tx_len - before == data_len, "only one frame on the wire while busy");
	out_uart_cb(ports[out_port].uart, &done, NULL);
	CHECK(sim_tx_len - before == 2 * data_len && tx_dropped == 0, "queued frame not sent");
	out_uart_cb(ports[out_port].uart, &done, NULL);
	for (int i = 0; i < TXQ_LEN + 1; i++)
		wired_write_data(f);
	CHECK(tx_dropped == 1, "queue full drop: %u", tx_dropped);
	tx_done_all();

	// Wire streams of four trackers, made with the output path: left leg ankle A1 and thigh A2,
	// right leg ankle B1 and thigh B2.
	uint8_t a1[1024], a2[1024], b1[1024], b2[1024];
	size_t a1n = tracker_wire(0xA1, 10, 3, a1), a2n = tracker_wire(0xA2, 20, 3, a2);
	size_t b1n = tracker_wire(0xB1, 30, 3, b1), b2n = tracker_wire(0xB2, 40, 3, b2);

	// Thigh: wired in left out right. Forwards its input unchanged, wraps legacy DATA.
	CHECK(boot(WIRED_IN_LEFT | WIRED_OUT_RIGHT, 0) == 0 && ports[PORT_LEFT].in && out_port == PORT_RIGHT &&
	      sim_device_inits == 2, "thigh role");
	{
		uint8_t legacy_raw[DATA_LEN], legacy_enc[MAX_ENCODED];
		legacy_raw[0] = TYPE_DATA;
		make_frame(&legacy_raw[1], 1, 50, 9);
		size_t ln = cobs_encode(legacy_raw, sizeof(legacy_raw), legacy_enc);
		legacy_enc[ln++] = 0;
		uint8_t down[2048];
		memcpy(down, a1, a1n);
		memcpy(down + a1n, legacy_enc, ln);
		size_t down_len = a1n + ln;

		sim_tx_len = 0;
		input_open(PORT_LEFT);
		for (size_t i = 0; i < down_len; i++) {
			input_feed(PORT_LEFT, &down[i], 1);
			if (down[i] == 0)
				tx_done_all();
		}
		CHECK(st_relayed == 5 && ports[PORT_LEFT].st_bad == 0, "forwarded %u bad %u", st_relayed,
		      ports[PORT_LEFT].st_bad);
		CHECK(sim_tx_len >= a1n && memcmp(sim_tx, a1, a1n) == 0, "HELLO and ADATA must be forwarded byte for byte");
		uint8_t raw[8][40];
		size_t rl[8];
		int n = decode_frames(sim_tx, sim_tx_len, raw, rl, 8);
		CHECK(n == 5 && raw[4][0] == TYPE_ADATA && raw[4][1] == 0x11 && raw[4][5] == 0xA1 && raw[4][27] == 50,
		      "legacy DATA wrapped with the HELLO address (%d frames)", n);
		CHECK(wired_relaying(), "a forwarding tracker counts as relaying");
		CHECK(!wired_relay_get(out), "a chain tracker queues nothing for the radio");
	}

	// Hip: wired in both. Left carries A1 and A2, right B1 and B2.
	memset(nvs, 0, sizeof(nvs));
	memset(ankles, 0, sizeof(ankles));
	CHECK(boot(WIRED_IN_LEFT | WIRED_IN_RIGHT, 0) == 0 && ports[PORT_LEFT].in && ports[PORT_RIGHT].in && out_port < 0 &&
	      sim_device_inits == 2 && !wired_is_ankle(), "hip role");
	input_open(PORT_LEFT);
	input_open(PORT_RIGHT);
	input_feed(PORT_LEFT, a1, a1n);
	input_feed(PORT_LEFT, a2, a2n);
	input_feed(PORT_RIGHT, b1, b1n);
	input_feed(PORT_RIGHT, b2, b2n);
	CHECK(st_unpaired == 12 && !wired_relay_get(out), "unpaired frames dropped (%u)", st_unpaired);
	{
		uint8_t addrs[SEEN_MAX][6];
		int heard, n = pair_candidates(addrs, &heard);
		CHECK(heard == 4 && n == 4, "pairing candidates %d of %d", n, heard);
		for (int i = 0; i < n; i++) { // one pass of pair_thread
			uint8_t id;
			esb_pair_other(addrs[i], &id);
			store_id(addrs[i], id);
		}
		CHECK(pair_candidates(addrs, &heard) == 0 && heard == 4, "all paired");
	}
	int id_of[256];
	for (int i = 0; i < 256; i++)
		id_of[i] = -1;
	for (int i = 0; i < CONFIG_WIRED_ANKLE_TABLE_SIZE; i++)
		if (ankles[i].used)
			id_of[ankles[i].addr[5] == 0x55 ? ankles[i].addr[4] : 0] = ankles[i].id;
	CHECK(id_of[0xA1] >= 10 && id_of[0xA2] >= 10 && id_of[0xB1] >= 10 && id_of[0xB2] >= 10,
	      "ids %d %d %d %d", id_of[0xA1], id_of[0xA2], id_of[0xB1], id_of[0xB2]);

	// Table survives a reboot (NVS); frames from both inputs come out with their own ids.
	CHECK(boot(WIRED_IN_LEFT | WIRED_IN_RIGHT, 0) == 0, "hip reboot");
	input_open(PORT_LEFT);
	input_open(PORT_RIGHT);
	int w0 = wakes;
	input_feed(PORT_LEFT, a2, a2n);
	input_feed(PORT_RIGHT, b1, b1n);
	CHECK(wired_relaying() && wakes - w0 == 6, "relaying, wakes %d", wakes - w0);
	struct { uint8_t addr, seq0; } expect[] = {{0xA2, 20}, {0xB1, 30}};
	for (int e = 0; e < 2; e++)
		for (int s = 0; s < 3; s++) {
			uint8_t ref[WIRED_FRAME_LEN];
			make_frame(ref, 1, (uint8_t)(expect[e].seq0 + s), (uint8_t)(expect[e].addr + s));
			CHECK(wired_relay_get(out), "frame %d/%d missing", e, s);
			uint32_t crc;
			memcpy(&crc, &out[16], 4);
			CHECK(out[1] == id_of[expect[e].addr], "id byte %u, expected %d", out[1], id_of[expect[e].addr]);
			CHECK(crc == crc32_k_4_2_update(0x93a409eb, out, 16), "relayed CRC invalid");
			CHECK(out[20] == expect[e].seq0 + s, "sequence %u", out[20]);
			CHECK(out[0] == ref[0] && memcmp(&out[2], &ref[2], 14) == 0, "payload changed");
		}
	CHECK(!wired_relay_get(out), "queue should be empty");

	// Legacy DATA on the right input takes the address of the right input's last HELLO.
	{
		uint8_t legacy_raw[DATA_LEN], legacy_enc[MAX_ENCODED];
		legacy_raw[0] = TYPE_DATA;
		make_frame(&legacy_raw[1], 1, 60, 3);
		size_t ln = cobs_encode(legacy_raw, sizeof(legacy_raw), legacy_enc);
		legacy_enc[ln++] = 0;
		input_feed(PORT_RIGHT, legacy_enc, ln);
		CHECK(wired_relay_get(out) && out[1] == id_of[0xB1] && out[20] == 60, "legacy DATA id %u", out[1]);
	}

	// Queue overflow: 40 frames without draining keep the newest 32.
	uint8_t many[4096];
	set_addr(0xA2);
	out_port = PORT_RIGHT; // borrow the output path to build the stream
	uart_ok = true;
	sim_tx_len = 0;
	for (int s = 0; s < 40; s++) {
		make_frame(f, 1, (uint8_t)(100 + s), 0);
		wired_write_data(f);
		tx_done_all();
	}
	size_t many_len = sim_tx_len;
	memcpy(many, sim_tx, many_len);
	out_port = -1;
	uint32_t qd = st_qdrop;
	input_feed(PORT_LEFT, many, many_len);
	CHECK(st_qdrop - qd == 8, "queue drops %u", st_qdrop - qd);
	CHECK(wired_relay_get(out) && out[20] == 108, "oldest kept should be seq 108, got %u", out[20]);
	while (wired_relay_get(out)) {}

	// Corruption: a flipped bit fails CRC32 and is counted as bad on that input only.
	uint8_t bad_frame[64];
	size_t bl = 0;
	for (size_t i = 0; i < many_len && bl < sizeof(bad_frame); i++) {
		bad_frame[bl++] = many[i];
		if (many[i] == 0)
			break;
	}
	bad_frame[5] ^= 0x40;
	if (bad_frame[5] == 0)
		bad_frame[5] = 0x40;
	uint32_t bad_l = ports[PORT_LEFT].st_bad, bad_r = ports[PORT_RIGHT].st_bad;
	input_feed(PORT_LEFT, bad_frame, bl);
	CHECK(ports[PORT_LEFT].st_bad == bad_l + 1 && ports[PORT_RIGHT].st_bad == bad_r && !wired_relay_get(out),
	      "corrupted frame accepted");

	// RX disabled on one input: that input stops and re-arms, the other keeps relaying.
	struct uart_event dis = {.type = UART_RX_DISABLED};
	int rearm_before = ports[PORT_LEFT].rearm_work.submitted;
	in_uart_cb(ports[PORT_LEFT].uart, &dis, &ports[PORT_LEFT]);
	CHECK(!atomic_get(&ports[PORT_LEFT].active) && ports[PORT_LEFT].rearm_work.submitted == rearm_before + 1,
	      "disable handling");
	CHECK(wired_relaying(), "the right input is still active");

	// Hub: both inputs listen at once and stay on, every wired frame goes to hub.c by address,
	// the left UART's TX events (the hub output) go to hub.c, and nothing is queued for ESB.
	hub_started = hub_wired_count = hub_tx_dones = hub_pair_opens = 0;
	CHECK(boot(WIRED_MODE_HUB, 0) == 0 && wired_is_hub() && !wired_is_ankle() && out_port < 0 && hub_started == 1,
	      "hub role");
	CHECK(ports[PORT_LEFT].always_on && ports[PORT_RIGHT].always_on && atomic_get(&ports[PORT_LEFT].rx_on) &&
	      atomic_get(&ports[PORT_RIGHT].rx_on), "hub inputs listen at once");
	CHECK(wired_relaying(), "the hub counts as relaying (stays awake)");
	{
		uint8_t sync0 = 0;
		input_feed(PORT_LEFT, &sync0, 1);
		input_feed(PORT_RIGHT, &sync0, 1);
		input_feed(PORT_LEFT, a1, a1n);
		CHECK(hub_wired_count == 3 && hub_last_addr[4] == 0xA1 && hub_last_pkt[1] == WIRED_ID_PLACEHOLDER,
		      "left input to the hub (%d)", hub_wired_count);
		input_feed(PORT_RIGHT, b2, b2n);
		CHECK(hub_wired_count == 6 && hub_last_addr[4] == 0xB2, "right input to the hub (%d)", hub_wired_count);
		uint8_t legacy_raw[DATA_LEN], legacy_enc[MAX_ENCODED];
		legacy_raw[0] = TYPE_DATA;
		make_frame(&legacy_raw[1], 1, 70, 5);
		size_t ln = cobs_encode(legacy_raw, sizeof(legacy_raw), legacy_enc);
		legacy_enc[ln++] = 0;
		input_feed(PORT_RIGHT, legacy_enc, ln);
		CHECK(hub_wired_count == 7 && hub_last_addr[4] == 0xB2, "legacy DATA takes the right input's HELLO address");
		CHECK(!wired_relay_get(out) && st_unpaired == 0, "the hub needs no pairing and queues nothing for ESB");
	}
	struct uart_event txd = {.type = UART_TX_DONE};
	in_uart_cb(ports[PORT_LEFT].uart, &txd, &ports[PORT_LEFT]);
	CHECK(hub_tx_dones == 1, "hub output TX done reaches hub.c");
	int starts = ports[PORT_LEFT].start_rx_work.submitted, rearms = ports[PORT_LEFT].rearm_work.submitted;
	in_uart_cb(ports[PORT_LEFT].uart, &dis, &ports[PORT_LEFT]);
	CHECK(ports[PORT_LEFT].start_rx_work.submitted == starts + 1 && ports[PORT_LEFT].rearm_work.submitted == rearms,
	      "an always-on input restarts instead of suspending");
	wired_request_pair_ankle();
	CHECK(hub_pair_opens == 1 && pair_sem.count == 0, "pair_wired opens radio pairing on the hub");

	printf(failures ? "%d FAILURE(S)\n" : "all wired link tests passed\n", failures);
	return failures != 0;
}
