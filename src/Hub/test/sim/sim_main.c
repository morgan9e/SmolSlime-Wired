// Host simulation: generates the receiver's UART byte stream for a few fake trackers,
// runs it through the real deframer, decoder and slime.c, and talks UDP to a SlimeVR Server.
//
// Scenario (seconds from start):
//   tracker 0: continuous
//   tracker 1: 300 ms radio gap every 4 s (exercises gap hold and the resume blend)
//   tracker 2: silent from 8 to 15 s (server should time it out, then recover)
//   one corrupted frame at 3 s (uart_bad should end at 1)
#include <arpa/inet.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "deframer.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "hub.h"
#include "hub_batt.h"
#include "net.h"
#include "slime.h"

hub_counters_t g_hub;

bool net_is_up(void) { return true; }
int net_rssi(void) { return -42; }
void hub_batt_init(void) {}
float hub_batt_volts(void) { return 3.91f; }

#define N_TRK 3
#define TO_FIXED_15(x) ((int16_t)fmaxf(-32768.0f, fminf(32767.0f, (x) * 32768.0f)))
#define TO_FIXED_7(x) ((int16_t)fmaxf(-32768.0f, fminf(32767.0f, (x) * 128.0f)))

static QueueHandle_t q;
static deframer_t df;
static volatile int corrupt_next;
static char last_stats[4096];

static void wr16(uint8_t *p, int16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)((uint16_t)v >> 8);
}

// Same path as uart_rx.c feed().
static void push_packet(const uint8_t pkt[16])
{
	uint8_t wire[24], out[16];
	size_t n = deframer_encode(pkt, wire);
	if (corrupt_next) {
		corrupt_next = 0;
		wire[3] ^= 0x80;
		if (!wire[3])
			wire[3] = 0x80;
	}
	int64_t now = esp_timer_get_time();
	for (size_t i = 0; i < n; i++) {
		df_result_t r = deframer_push(&df, wire[i], out);
		if (r == DF_OK) {
			g_hub.uart_ok++;
			hub_sample_t s;
			if (!decoder_parse(out, &s)) {
				g_hub.uart_unknown++;
				continue;
			}
			s.t_us = now;
			if (xQueueSend(q, &s, 0) != pdTRUE)
				g_hub.q_drop++;
		} else if (r == DF_BAD) {
			g_hub.uart_bad++;
		}
	}
}

static void quat_for(int id, double t, float wq[4])
{
	double a = (id + 1) * 30.0 * M_PI / 180.0 * t; // yaw rate (id+1)*30 deg/s
	wq[0] = (float)cos(a / 2);
	wq[1] = 0;
	wq[2] = 0;
	wq[3] = (float)sin(a / 2);
}

static bool radio_on(int id, double t)
{
	if (id == 1)
		return fmod(t, 4.0) < 3.7;
	if (id == 2)
		return t < 8.0 || t >= 15.0;
	return true;
}

static void make_addr(uint8_t *p, int id)
{
	p[0] = 255;
	p[1] = (uint8_t)id;
	const uint8_t addr[6] = {(uint8_t)(0x10 + id), 0x22, 0x33, 0x44, 0x55, 0xC6};
	memcpy(p + 2, addr, 6);
}

static void make_info(uint8_t *p, int id)
{
	p[0] = 0;
	p[1] = (uint8_t)id;
	p[2] = (uint8_t)(80 - id * 10); // battery %
	p[3] = 150;                      // 3.95 V
	p[4] = 140;                      // 31 C
	p[5] = 24;                       // GENERIC_NRF
	p[6] = 9;                        // NRF52
	p[8] = 16;                       // ICM45686
	p[9] = 0;                        // mag not supported
	wr16(p + 10, (int16_t)((6 << 9) | (10 << 5) | 8));
	p[12] = 0;
	p[13] = 6;
	p[14] = 9;
	p[15] = (uint8_t)(55 + id); // RSSI magnitude
}

static void make_rot(uint8_t *p, int id, const float *wq)
{
	p[0] = 1;
	p[1] = (uint8_t)id;
	wr16(p + 2, TO_FIXED_15(wq[1]));
	wr16(p + 4, TO_FIXED_15(wq[2]));
	wr16(p + 6, TO_FIXED_15(wq[3]));
	wr16(p + 8, TO_FIXED_15(wq[0]));
	wr16(p + 10, TO_FIXED_7(0.05f));
	wr16(p + 12, TO_FIXED_7(-0.02f));
	wr16(p + 14, TO_FIXED_7(0.1f));
}

static void *stats_listener(void *arg)
{
	(void)arg;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(CONFIG_HUB_STATS_PORT)};
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("stats bind");
		return NULL;
	}
	char buf[4096];
	for (;;) {
		ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
		if (n > 0) {
			buf[n] = 0;
			memcpy(last_stats, buf, (size_t)n + 1);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	double duration = argc > 1 ? atof(argv[1]) : 25.0;
	uint32_t server_ip = 0;
	if (argc > 2 && strcmp(argv[2], "auto") != 0)
		inet_pton(AF_INET, argv[2], &server_ip);

	pthread_t st;
	pthread_create(&st, NULL, stats_listener, NULL);

	q = xQueueCreate(HUB_SAMPLE_QUEUE_LEN, sizeof(hub_sample_t));
	deframer_reset(&df);
	slime_start(q, server_ip);

	int64_t t0 = esp_timer_get_time();
	int64_t next_rot[N_TRK] = {0}, next_100[N_TRK] = {0}, next_1s[N_TRK] = {0}, next_addr = 0;
	bool corrupted = false;
	int64_t next_status = 5000000;

	for (;;) {
		int64_t now = esp_timer_get_time() - t0;
		double t = now / 1e6;
		if (t >= duration)
			break;
		if (!corrupted && t >= 3.0) {
			corrupted = true;
			corrupt_next = 1;
		}
		uint8_t p[16];

		if (now >= next_addr) { // the receiver mirror always sends these, radio or not
			next_addr = now + 100000;
			for (int i = 0; i < N_TRK; i++) {
				memset(p, 0, 16);
				make_addr(p, i);
				push_packet(p);
			}
		}
		for (int i = 0; i < N_TRK; i++) {
			if (!radio_on(i, t))
				continue;
			if (now >= next_rot[i]) {
				next_rot[i] = now + 6000;
				float wq[4];
				quat_for(i, t, wq);
				memset(p, 0, 16);
				make_rot(p, i, wq);
				push_packet(p);
			}
			if (now >= next_100[i]) {
				next_100[i] = now + 100000;
				memset(p, 0, 16);
				make_info(p, i);
				push_packet(p);
				memset(p, 0, 16);
				p[0] = 6; p[1] = (uint8_t)i; p[3] = 0xFF; p[4] = 0xFF; p[15] = 60;
				push_packet(p);
			}
			if (now >= next_1s[i]) {
				next_1s[i] = now + 1000000;
				memset(p, 0, 16);
				p[0] = 3; p[1] = (uint8_t)i; p[2] = 1; p[15] = 60;
				push_packet(p);
			}
		}
		if (now >= next_status) {
			next_status += 5000000;
			fprintf(stderr, "---- t=%.1f s\n", t);
			slime_print_status();
		}
		usleep(1000);
	}

	usleep(1200000); // let the last stats line arrive
	printf("==== final status\n");
	slime_print_status();
	printf("==== last stats line\n%s", last_stats);
	return 0;
}
