#include "slime.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "hub.h"
#include "hub_batt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net.h"

static const char *TAG = "slime";

#define MS(x) ((int64_t)(x) * 1000)

#ifdef CONFIG_HUB_LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#else
#define LED_ACTIVE_LOW 0
#endif

// SlimeVR UDP protocol (big-endian). Hub -> server packet ids.
enum {
	PKT_HEARTBEAT = 0,
	PKT_HANDSHAKE = 3,
	PKT_BATTERY_LEVEL = 12,
	PKT_SENSOR_INFO = 15,
	PKT_ROTATION_DATA = 17,
	PKT_SIGNAL_STRENGTH = 19,
	PKT_TEMPERATURE = 20,
	PKT_ROTATION_AND_ACCEL = 23,
};
// Server -> hub packet ids.
enum {
	SRV_HEARTBEAT = 1,
	SRV_PING_PONG = 10,
	SRV_SENSOR_INFO = 15,
};
#define PROTOCOL_VERSION 22 // 22+: the server takes acceleration in the sensor frame as sent
static const char HANDSHAKE_REPLY[] = "Hey OVR =D 5";

// Tracker SVR_STATUS_* (build_defines.h) -> UDP SensorInfo status (0 offline, 1 ok, 2 error).
#define TRK_SVR_STATUS_ERROR 3

// A tracker without real data for this long stops answering the server, so the server marks
// it timed out (the server's own timeout is 2000 ms).
#define ACTIVE_MS 2000

typedef struct {
	// identity, from the receiver's 255 packets and the tracker's type 0 packets
	bool known;
	uint8_t addr[6];
	bool have_info;
	uint8_t imu, mcu, fw[3];
	uint8_t svr_status;

	// latest real data
	bool have_pose;
	bool have_accel;
	float q[4]; // x y z w
	float a[3];
	int64_t last_real_us;
	uint8_t batt, batt_v, temp, rssi;
	bool have_batt, have_rssi;

	// server link
	int sock;
	bool link_up;
	int64_t pkt_num;
	int64_t last_srv_rx_us, last_hs_tx_us, last_si_tx_us, last_slow_tx_us;
	uint8_t si_status;
	bool si_acked;

	// stats, reset every second
	uint32_t st_samples, st_gaps, st_gap_max_us;
	// last completed second, for the console
	uint32_t pps, gaps, gap_max_ms;
} trk_t;

static trk_t s_trk[HUB_MAX_TRACKERS];
static QueueHandle_t s_q;
static struct sockaddr_in s_server;
static bool s_server_known, s_server_fixed;
static int64_t s_last_any_srv_rx_us;
static float s_hub_v = -1.0f;

// ---------------------------------------------------------------- packet writer

typedef struct {
	uint8_t b[96];
	size_t n;
} pkt_t;

static void w_u8(pkt_t *p, uint8_t v)
{
	if (p->n < sizeof(p->b))
		p->b[p->n++] = v;
}

static void w_u16(pkt_t *p, uint16_t v)
{
	w_u8(p, (uint8_t)(v >> 8));
	w_u8(p, (uint8_t)v);
}

static void w_u32(pkt_t *p, uint32_t v)
{
	w_u16(p, (uint16_t)(v >> 16));
	w_u16(p, (uint16_t)v);
}

static void w_u64(pkt_t *p, uint64_t v)
{
	w_u32(p, (uint32_t)(v >> 32));
	w_u32(p, (uint32_t)v);
}

static void w_f32(pkt_t *p, float f)
{
	uint32_t v;
	memcpy(&v, &f, sizeof(v));
	w_u32(p, v);
}

static int16_t to_fixed(float v, float scale)
{
	float x = roundf(v * scale);
	if (x > 32767.0f)
		return 32767;
	if (x < -32768.0f)
		return -32768;
	return (int16_t)x;
}

static void pkt_begin(pkt_t *p, trk_t *t, uint32_t type)
{
	p->n = 0;
	w_u32(p, type);
	// Strictly increasing per socket; the handshake uses 0 like the ESP firmware.
	w_u64(p, type == PKT_HANDSHAKE ? 0 : (uint64_t)++t->pkt_num);
}

static void send_raw(trk_t *t, const void *buf, size_t len, bool allow_broadcast)
{
	struct sockaddr_in dst;
	if (s_server_known) {
		dst = s_server;
	} else if (allow_broadcast) {
		memset(&dst, 0, sizeof(dst));
		dst.sin_family = AF_INET;
		dst.sin_port = htons(CONFIG_HUB_SERVER_PORT);
		dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	} else {
		return;
	}
	if (sendto(t->sock, buf, len, 0, (const struct sockaddr *)&dst, sizeof(dst)) < 0)
		g_hub.udp_tx_err++;
}

static void send_pkt(trk_t *t, const pkt_t *p)
{
	send_raw(t, p->b, p->n, false);
}

// ---------------------------------------------------------------- SlimeVR packets

static void send_handshake(trk_t *t)
{
	pkt_t p;
	char fw[16];
	pkt_begin(&p, t, PKT_HANDSHAKE);
	w_u32(&p, CONFIG_HUB_REPORT_BOARD);
	w_u32(&p, t->imu);
	w_u32(&p, t->mcu);
	w_u32(&p, 0); // legacy IMU info
	w_u32(&p, 0);
	w_u32(&p, 0);
	w_u32(&p, PROTOCOL_VERSION);
	int n = snprintf(fw, sizeof(fw), "%u.%u.%u", t->fw[0], t->fw[1], t->fw[2]);
	w_u8(&p, (uint8_t)n);
	for (int i = 0; i < n; i++)
		w_u8(&p, (uint8_t)fw[i]);
	// MAC: the radio address, most significant byte first.
	for (int i = 5; i >= 0; i--)
		w_u8(&p, t->addr[i]);
	send_raw(t, p.b, p.n, true);
}

static uint8_t sensor_info_status(const trk_t *t)
{
	return t->svr_status == TRK_SVR_STATUS_ERROR ? 2 : 1;
}

static void send_sensor_info(trk_t *t)
{
	pkt_t p;
	t->si_status = sensor_info_status(t);
	pkt_begin(&p, t, PKT_SENSOR_INFO);
	w_u8(&p, 0); // sensor id
	w_u8(&p, t->si_status);
	w_u8(&p, t->imu);
	send_pkt(t, &p);
}

static void send_heartbeat(trk_t *t)
{
	pkt_t p;
	pkt_begin(&p, t, PKT_HEARTBEAT);
	send_pkt(t, &p);
}

// Rotation with acceleration (23, Q15/Q7 like the HID path) or rotation only (17, floats).
static void send_pose(trk_t *t, const float *q, bool with_accel)
{
	pkt_t p;
	if (with_accel) {
		pkt_begin(&p, t, PKT_ROTATION_AND_ACCEL);
		w_u8(&p, 0);
		for (int i = 0; i < 4; i++)
			w_u16(&p, (uint16_t)to_fixed(q[i], 32768.0f));
		for (int i = 0; i < 3; i++)
			w_u16(&p, (uint16_t)to_fixed(t->a[i], 128.0f));
	} else {
		pkt_begin(&p, t, PKT_ROTATION_DATA);
		w_u8(&p, 0); // sensor id
		w_u8(&p, 1); // data type: normal
		for (int i = 0; i < 4; i++)
			w_f32(&p, q[i]);
		w_u8(&p, 0); // calibration info
	}
	send_pkt(t, &p);
}

static void send_slow(trk_t *t)
{
	pkt_t p;
	if (t->have_batt) {
		int pct = t->batt & 127;
		float volts = (t->batt_v + 245) / 100.0f;
		pkt_begin(&p, t, PKT_BATTERY_LEVEL);
		w_f32(&p, volts);
		w_f32(&p, t->batt == 128 ? 0.0f : pct / 100.0f);
		send_pkt(t, &p);
	}
	if (t->have_rssi) {
		pkt_begin(&p, t, PKT_SIGNAL_STRENGTH);
		w_u8(&p, 255);
		w_u8(&p, (uint8_t)(int8_t)(-(int)t->rssi));
		send_pkt(t, &p);
	}
	if (t->temp) {
		pkt_begin(&p, t, PKT_TEMPERATURE);
		w_u8(&p, 0);
		w_f32(&p, t->temp / 2.0f - 39.0f);
		send_pkt(t, &p);
	}
}

// ---------------------------------------------------------------- tracker state

static bool trk_active(const trk_t *t, int64_t now)
{
	return t->have_pose && now - t->last_real_us < MS(ACTIVE_MS);
}

static void link_down(trk_t *t, const char *why)
{
	if (t->link_up)
		ESP_LOGW(TAG, "tracker %d: link down (%s)", (int)(t - s_trk), why);
	t->link_up = false;
	t->si_acked = false;
	t->last_hs_tx_us = 0;
}

static int open_socket(int id)
{
	int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0) {
		ESP_LOGE(TAG, "socket() failed for tracker %d", id);
		return -1;
	}
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	struct sockaddr_in a = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_HUB_LOCAL_PORT_BASE + id),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		ESP_LOGE(TAG, "bind(%d) failed for tracker %d", CONFIG_HUB_LOCAL_PORT_BASE + id, id);
		close(s);
		return -1;
	}
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	return s;
}

static void on_rotation(trk_t *t, const hub_sample_t *s)
{
	if (t->have_pose) {
		int64_t gap = s->t_us - t->last_real_us;
		if (gap > MS(CONFIG_HUB_GAP_COUNT_MS))
			t->st_gaps++;
		if (gap > (int64_t)t->st_gap_max_us)
			t->st_gap_max_us = (uint32_t)gap;
	}
	memcpy(t->q, s->q, sizeof(t->q));
	if (s->flags & HS_ACCEL) {
		memcpy(t->a, s->a, sizeof(t->a));
		t->have_accel = true;
	}
	t->have_pose = true;
	t->last_real_us = s->t_us;
	t->st_samples++;

	if (t->link_up)
		send_pose(t, t->q, (s->flags & HS_ACCEL) != 0);
}

static void handle_sample(const hub_sample_t *s)
{
	if (s->id >= HUB_MAX_TRACKERS) {
		g_hub.id_overflow++;
		return;
	}
	trk_t *t = &s_trk[s->id];

	if (s->flags & HS_ADDR) {
		if (t->known && memcmp(t->addr, s->addr, 6) == 0)
			return;
		if (t->known) {
			// The receiver reassigned this id (e.g. after "clear" and re-pairing).
			ESP_LOGW(TAG, "tracker %d: address changed", s->id);
			link_down(t, "address changed");
			int sock = t->sock;
			memset(t, 0, sizeof(*t));
			t->sock = sock;
		}
		memcpy(t->addr, s->addr, 6);
		t->known = true;
		if (t->sock < 0)
			t->sock = open_socket(s->id);
		ESP_LOGI(TAG, "tracker %d: address %02X:%02X:%02X:%02X:%02X:%02X", s->id, t->addr[5], t->addr[4],
			 t->addr[3], t->addr[2], t->addr[1], t->addr[0]);
		return;
	}
	if (!t->known || t->sock < 0) {
		g_hub.no_addr_drop++;
		return;
	}

	if (s->flags & HS_RSSI) {
		t->rssi = s->rssi;
		t->have_rssi = true;
	}
	if (s->flags & HS_BATT) {
		t->batt = s->batt;
		t->batt_v = s->batt_v;
		t->have_batt = true;
	}
	if (s->flags & HS_TEMP)
		t->temp = s->temp;
	if (s->flags & HS_INFO) {
		if (t->have_info && t->imu != s->imu)
			t->si_acked = false; // re-announce the sensor type
		t->imu = s->imu;
		t->mcu = s->mcu;
		memcpy(t->fw, s->fw, sizeof(t->fw));
		t->have_info = true;
	}
	if (s->flags & HS_STATUS) {
		t->svr_status = s->svr_status;
		if (sensor_info_status(t) != t->si_status)
			t->si_acked = false; // re-announce with the new status
	}
	if (s->flags & HS_ROT)
		on_rotation(t, s);
}

// ---------------------------------------------------------------- server packets

static uint32_t rd_be32(const uint8_t *b)
{
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void handle_server_packet(trk_t *t, const uint8_t *b, int n, const struct sockaddr_in *from, int64_t now)
{
	int id = (int)(t - s_trk);

	if (n >= 13 && b[0] == PKT_HANDSHAKE && memcmp(b + 1, HANDSHAKE_REPLY, 12) == 0) {
		if (s_server_fixed && from->sin_addr.s_addr != s_server.sin_addr.s_addr)
			return;
		if (!s_server_fixed && (!s_server_known || s_server.sin_addr.s_addr != from->sin_addr.s_addr)) {
			s_server = *from;
			s_server_known = true;
			char ip[16];
			inet_ntoa_r(from->sin_addr, ip, sizeof(ip));
			ESP_LOGI(TAG, "server found at %s:%d", ip, ntohs(from->sin_port));
		}
		t->last_srv_rx_us = now;
		s_last_any_srv_rx_us = now;
		if (!t->link_up) {
			t->link_up = true;
			t->si_acked = false;
			t->last_si_tx_us = 0;
			t->last_slow_tx_us = 0;
			ESP_LOGI(TAG, "tracker %d: link up", id);
		}
		return;
	}
	if (n < 4 || !s_server_known || from->sin_addr.s_addr != s_server.sin_addr.s_addr)
		return;

	t->last_srv_rx_us = now;
	s_last_any_srv_rx_us = now;
	if (!t->link_up)
		return;
	// A silent tracker must stay silent, or the server would never mark it timed out.
	bool active = trk_active(t, now);

	switch (rd_be32(b)) {
	case SRV_HEARTBEAT:
		if (active)
			send_heartbeat(t);
		break;
	case SRV_PING_PONG:
		if (active)
			send_raw(t, b, (size_t)n, false); // echo unchanged, like the ESP firmware
		break;
	case SRV_SENSOR_INFO: // ack: [id][sensor id][status], no packet number
		if (n >= 6 && b[4] == 0 && b[5] == t->si_status)
			t->si_acked = true;
		break;
	default: // feature flags, config flags, commands: not supported
		break;
	}
}

static void poll_sockets(int64_t now)
{
	fd_set rf;
	int maxfd = -1;
	FD_ZERO(&rf);
	for (int i = 0; i < HUB_MAX_TRACKERS; i++) {
		if (s_trk[i].sock >= 0) {
			FD_SET(s_trk[i].sock, &rf);
			if (s_trk[i].sock > maxfd)
				maxfd = s_trk[i].sock;
		}
	}
	if (maxfd < 0)
		return;
	struct timeval tv = {0, 0};
	if (select(maxfd + 1, &rf, NULL, NULL, &tv) <= 0)
		return;

	uint8_t buf[256];
	for (int i = 0; i < HUB_MAX_TRACKERS; i++) {
		trk_t *t = &s_trk[i];
		if (t->sock < 0 || !FD_ISSET(t->sock, &rf))
			continue;
		for (int k = 0; k < 8; k++) {
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			int n = recvfrom(t->sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
			if (n <= 0)
				break;
			handle_server_packet(t, buf, n, &from, now);
		}
	}
}

// ---------------------------------------------------------------- periodic work

static void tick_10ms(int64_t now)
{
	bool any_up = false;

	for (int i = 0; i < HUB_MAX_TRACKERS; i++) {
		trk_t *t = &s_trk[i];
		if (!t->known || t->sock < 0)
			continue;
		bool active = trk_active(t, now);

		if (t->link_up && now - t->last_srv_rx_us > MS(CONFIG_HUB_SERVER_TIMEOUT_MS))
			link_down(t, "server silent");
		if (t->link_up && !active && t->have_pose && now - t->last_real_us > MS(CONFIG_HUB_DROP_MS))
			link_down(t, "tracker silent");
		if (t->link_up)
			any_up = true;

		if (!active)
			continue;

		if (!t->link_up) {
			if (t->have_info && net_is_up() && now - t->last_hs_tx_us >= MS(1000)) {
				t->last_hs_tx_us = now;
				send_handshake(t);
			}
			continue;
		}

		if (!t->si_acked && now - t->last_si_tx_us >= MS(1000)) {
			t->last_si_tx_us = now;
			send_sensor_info(t);
		}
		if (now - t->last_slow_tx_us >= MS(1000)) {
			t->last_slow_tx_us = now;
			send_slow(t);
		}
	}

	// Forget a discovered server once every link is gone, so a new server IP can be found.
	if (s_server_known && !s_server_fixed && !any_up &&
	    now - s_last_any_srv_rx_us > MS(CONFIG_HUB_SERVER_TIMEOUT_MS)) {
		ESP_LOGW(TAG, "server lost, back to discovery");
		s_server_known = false;
	}
}

static void led_update(int64_t now)
{
#if CONFIG_HUB_LED_GPIO >= 0
	bool any_up = false;
	for (int i = 0; i < HUB_MAX_TRACKERS; i++)
		any_up |= s_trk[i].link_up;
	bool on;
	if (any_up)
		on = true; // streaming
	else if (net_is_up())
		on = (now / MS(500)) % 2; // Wi-Fi up, no server: slow blink
	else
		on = (now / MS(100)) % 2; // no Wi-Fi: fast blink
	gpio_set_level(CONFIG_HUB_LED_GPIO, LED_ACTIVE_LOW ? !on : on);
#endif
}

static void roll_stats(int64_t now)
{
	for (int i = 0; i < HUB_MAX_TRACKERS; i++) {
		trk_t *t = &s_trk[i];
		// A gap still open at the end of the second counts towards the worst gap.
		if (t->have_pose && t->st_samples == 0 && now - t->last_real_us > (int64_t)t->st_gap_max_us)
			t->st_gap_max_us = (uint32_t)(now - t->last_real_us);
		t->pps = t->st_samples;
		t->gaps = t->st_gaps;
		t->gap_max_ms = t->st_gap_max_us / 1000;
		t->st_samples = t->st_gaps = t->st_gap_max_us = 0;
	}
}

static void slime_task(void *arg)
{
	hub_sample_t s;
	int64_t last_tick = 0, last_sec = esp_timer_get_time(), last_log = last_sec;

	for (;;) {
		if (xQueueReceive(s_q, &s, pdMS_TO_TICKS(1)) == pdTRUE) {
			do {
				handle_sample(&s);
			} while (xQueueReceive(s_q, &s, 0) == pdTRUE);
		}
		int64_t now = esp_timer_get_time();
		poll_sockets(now);

		if (now - last_tick >= MS(10)) {
			last_tick = now;
			tick_10ms(now);
			led_update(now);
		}
		if (now - last_sec >= MS(1000)) {
			last_sec += MS(1000);
			if (now - last_sec > MS(1000))
				last_sec = now; // fell behind; do not burst
			s_hub_v = hub_batt_volts();
			roll_stats(now);
		}
		if (now - last_log >= MS(10000)) {
			last_log = now;
			int up = 0;
			for (int i = 0; i < HUB_MAX_TRACKERS; i++)
				up += s_trk[i].link_up;
			ESP_LOGI(TAG, "uart ok %" PRIu32 " bad %" PRIu32 " ovf %" PRIu32 " qdrop %" PRIu32 ", %d tracker(s) up, wifi %d dBm",
				 g_hub.uart_ok, g_hub.uart_bad, g_hub.uart_ovf, g_hub.q_drop, up, net_rssi());
		}
	}
}

void slime_start(QueueHandle_t sample_q, uint32_t server_ip)
{
	s_q = sample_q;
	for (int i = 0; i < HUB_MAX_TRACKERS; i++)
		s_trk[i].sock = -1;

	if (server_ip) {
		s_server.sin_family = AF_INET;
		s_server.sin_port = htons(CONFIG_HUB_SERVER_PORT);
		s_server.sin_addr.s_addr = server_ip;
		s_server_known = s_server_fixed = true;
	}

#if CONFIG_HUB_LED_GPIO >= 0
	gpio_reset_pin(CONFIG_HUB_LED_GPIO);
	gpio_set_direction(CONFIG_HUB_LED_GPIO, GPIO_MODE_OUTPUT);
#endif
	hub_batt_init();

	xTaskCreatePinnedToCore(slime_task, "slime", 6144, NULL, 12, NULL, 0);
}

void slime_print_status(void)
{
	int64_t now = esp_timer_get_time();
	char ip[16] = "broadcast";
	if (s_server_known)
		inet_ntoa_r(s_server.sin_addr, ip, sizeof(ip));
	printf("wifi %s (%d dBm), server %s%s\n", net_is_up() ? "up" : "down", net_rssi(), ip,
	       s_server_fixed ? " (fixed)" : "");
	printf("uart ok %" PRIu32 " bad %" PRIu32 " unknown %" PRIu32 " ovf %" PRIu32 " line %" PRIu32
	       " | qdrop %" PRIu32 " udp_err %" PRIu32 " no_addr %" PRIu32 " id_ovf %" PRIu32 "\n",
	       g_hub.uart_ok, g_hub.uart_bad, g_hub.uart_unknown, g_hub.uart_ovf, g_hub.uart_line_err, g_hub.q_drop,
	       g_hub.udp_tx_err, g_hub.no_addr_drop, g_hub.id_overflow);
	if (s_hub_v > 0)
		printf("hub battery %.2f V\n", s_hub_v);
	printf("id  address            link  imu  fw       pps  gapmax  gaps  rssi  batt  age_ms\n");
	for (int i = 0; i < HUB_MAX_TRACKERS; i++) {
		trk_t *t = &s_trk[i];
		if (!t->known)
			continue;
		long age = t->have_pose ? (long)((now - t->last_real_us) / 1000) : -1;
		int batt = !t->have_batt || t->batt == 128 ? -1 : (t->batt & 127);
		printf("%2d  %02X:%02X:%02X:%02X:%02X:%02X  %-4s  %3u  %u.%u.%-3u  %4" PRIu32 "  %6" PRIu32
		       "  %4" PRIu32 "  %4d  %4d  %ld\n",
		       i, t->addr[5], t->addr[4], t->addr[3], t->addr[2], t->addr[1], t->addr[0],
		       t->link_up ? "up" : "down", t->imu, t->fw[0], t->fw[1], t->fw[2], t->pps, t->gap_max_ms, t->gaps,
		       t->have_rssi ? -(int)t->rssi : 0, batt, age);
	}
}
