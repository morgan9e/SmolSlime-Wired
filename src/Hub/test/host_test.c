// Host-side tests for the portable parts of the hub firmware: COBS, CRC16, decoder.
// Packets are built with the same formulas as SlimeVR-Tracker-nRF connection.c / util.c.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cobs.h"
#include "crc16.h"
#include "decoder.h"
#include "deframer.h"

static int failures;
#define CHECK(cond, ...)                                                   \
	do {                                                                   \
		if (!(cond)) {                                                     \
			failures++;                                                    \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                    \
			printf(__VA_ARGS__);                                           \
			printf("\n");                                                  \
		}                                                                  \
	} while (0)

// ---- tracker-side encoders (copied semantics) ----
#define EPS 1e-6f
#define SATURATE_INT16(x) ((x) > 32767 ? 32767 : ((x) < -32768 ? -32768 : (x)))
#define SATURATE_UINT11(x) ((x) > 2047 ? 2047 : ((x) < 0 ? 0 : (x)))
#define SATURATE_UINT10(x) ((x) > 1023 ? 1023 : ((x) < 0 ? 0 : (x)))
#define TO_FIXED_15(x) ((int16_t)SATURATE_INT16((x) * (1 << 15)))
#define TO_FIXED_7(x) ((int16_t)SATURATE_INT16((x) * (1 << 7)))

static void q_fem(const float *q, float *out) // q is w, x, y, z
{
	float w = fabsf(q[0]);
	float a = 1 - w * w;
	float inv_sqrt_a = 1 / sqrtf(a + EPS);
	float k = a * inv_sqrt_a;
	float atan_term = (2 / (float)M_PI) * atanf(k / w);
	float sign_w = (q[0] == 0) ? 1 : copysignf(1, q[0]);
	float s = atan_term * inv_sqrt_a * sign_w;
	out[0] = s * q[1];
	out[1] = s * q[2];
	out[2] = s * q[3];
}

static void wr16(uint8_t *p, int16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)((uint16_t)v >> 8);
}

static void make_packet_1(uint8_t *d, uint8_t id, const float *wq, const float *a)
{
	memset(d, 0, 16);
	d[0] = 1;
	d[1] = id;
	wr16(d + 2, TO_FIXED_15(wq[1]));
	wr16(d + 4, TO_FIXED_15(wq[2]));
	wr16(d + 6, TO_FIXED_15(wq[3]));
	wr16(d + 8, TO_FIXED_15(wq[0]));
	for (int i = 0; i < 3; i++)
		wr16(d + 10 + 2 * i, TO_FIXED_7(a[i]));
}

static void make_packet_2(uint8_t *d, uint8_t id, const float *wq, const float *a, uint8_t rssi)
{
	memset(d, 0, 16);
	d[0] = 2;
	d[1] = id;
	d[2] = 0x80 | 77; // charging, 77 %
	d[3] = 150;       // 3.95 V
	d[4] = 140;       // 31 C
	float v[3];
	q_fem(wq, v);
	for (int i = 0; i < 3; i++)
		v[i] = (v[i] + 1) / 2;
	uint16_t vb[3] = {SATURATE_UINT10((1 << 10) * v[0]), SATURATE_UINT11((1 << 11) * v[1]),
			  SATURATE_UINT11((1 << 11) * v[2])};
	uint32_t qb = vb[0] | ((uint32_t)vb[1] << 10) | ((uint32_t)vb[2] << 21);
	memcpy(d + 5, &qb, 4); // little-endian host, as on nRF
	for (int i = 0; i < 3; i++)
		wr16(d + 9 + 2 * i, TO_FIXED_7(a[i]));
	d[15] = rssi;
}

// Angle between two rotations given as w,x,y,z and x,y,z,w, computed in double precision.
static float angle_deg(const float *wq, const float *xyzw)
{
	double a[4] = {wq[1], wq[2], wq[3], wq[0]}, na = 0, nb = 0, dot = 0;
	for (int i = 0; i < 4; i++) {
		na += a[i] * a[i];
		nb += (double)xyzw[i] * xyzw[i];
		dot += a[i] * xyzw[i];
	}
	dot = fabs(dot) / sqrt(na * nb);
	if (dot > 1)
		dot = 1;
	return (float)(2.0 * acos(dot) * 180.0 / M_PI);
}

static void random_quat(float *wq)
{
	float n = 0;
	for (int i = 0; i < 4; i++) {
		wq[i] = (float)((double)rand() / RAND_MAX * 2 - 1);
		n += wq[i] * wq[i];
	}
	n = sqrtf(n);
	for (int i = 0; i < 4; i++)
		wq[i] /= n;
}

static void test_crc(void)
{
	const char *s = "123456789";
	CHECK(hub_crc16((const uint8_t *)s, 9) == 0x29B1, "crc16 check value %04X", hub_crc16((const uint8_t *)s, 9));
}

static void test_cobs(void)
{
	uint8_t in[300], enc[COBS_MAX_ENCODED(300)], dec[300];
	for (int trial = 0; trial < 2000; trial++) {
		size_t len = (size_t)(rand() % 300);
		for (size_t i = 0; i < len; i++)
			in[i] = (rand() % 4 == 0) ? 0 : (uint8_t)rand();
		size_t n = cobs_encode(in, len, enc);
		CHECK(n <= COBS_MAX_ENCODED(len), "encoded too long");
		for (size_t i = 0; i < n; i++)
			CHECK(enc[i] != 0, "zero in encoded output");
		int m = cobs_decode(enc, n, dec, sizeof(dec));
		CHECK(m == (int)len && memcmp(in, dec, len) == 0, "roundtrip len %zu got %d", len, m);
	}
	// Our frame size: 18 bytes encodes to at most 19.
	uint8_t z[18] = {0};
	CHECK(cobs_encode(z, 18, enc) == 19, "18 zero bytes");
	// Malformed: code points past the end.
	uint8_t bad[] = {5, 1, 2};
	CHECK(cobs_decode(bad, sizeof(bad), dec, sizeof(dec)) < 0, "malformed accepted");
	// Too small output buffer.
	size_t n = cobs_encode(in, 40, enc);
	CHECK(cobs_decode(enc, n, dec, 18) < 0, "overflow accepted");
}

static void test_type1(void)
{
	float worst = 0;
	for (int t = 0; t < 10000; t++) {
		float wq[4], a[3] = {1.5f, -9.81f, 200.0f};
		random_quat(wq);
		uint8_t pkt[16];
		make_packet_1(pkt, 3, wq, a);
		hub_sample_t s;
		CHECK(decoder_parse(pkt, &s), "type 1 rejected");
		CHECK(s.flags == (HS_ROT | HS_ACCEL), "type 1 flags %x", s.flags);
		CHECK(s.id == 3, "type 1 id");
		float ang = angle_deg(wq, s.q);
		if (ang > worst)
			worst = ang;
		CHECK(fabsf(s.a[0] - 1.5f) < 0.01f && fabsf(s.a[1] + 9.81f) < 0.01f && fabsf(s.a[2] - 200.0f) < 0.01f,
		      "type 1 accel %f %f %f", s.a[0], s.a[1], s.a[2]);
	}
	CHECK(worst < 0.01f, "type 1 worst error %f deg", worst);
	printf("type 1: worst rotation error %.5f deg\n", worst);
}

static void test_type2(void)
{
	float worst = 0;
	for (int t = 0; t < 10000; t++) {
		float wq[4], a[3] = {0.25f, 3.0f, -4.0f};
		random_quat(wq);
		uint8_t pkt[16];
		make_packet_2(pkt, 7, wq, a, 61);
		hub_sample_t s;
		CHECK(decoder_parse(pkt, &s), "type 2 rejected");
		CHECK(s.flags == (HS_ROT | HS_ACCEL | HS_BATT | HS_TEMP | HS_RSSI), "type 2 flags %x", s.flags);
		CHECK(s.rssi == 61 && s.batt == (0x80 | 77) && s.batt_v == 150 && s.temp == 140, "type 2 fields");
		float ang = angle_deg(wq, s.q);
		if (ang > worst)
			worst = ang;
		CHECK(fabsf(s.a[1] - 3.0f) < 0.01f, "type 2 accel");
	}
	// 10/11-bit exponential map: expect well under one degree.
	CHECK(worst < 0.5f, "type 2 worst error %f deg", worst);
	printf("type 2: worst rotation error %.4f deg\n", worst);
}

static void test_misc_types(void)
{
	uint8_t p[16] = {0};
	hub_sample_t s;

	p[0] = 255;
	p[1] = 4;
	for (int i = 0; i < 6; i++)
		p[2 + i] = (uint8_t)(0xA0 + i);
	CHECK(decoder_parse(p, &s) && s.flags == HS_ADDR && s.id == 4 && s.addr[0] == 0xA0 && s.addr[5] == 0xA5, "type 255");

	memset(p, 0, 16);
	p[0] = 0; p[1] = 2; p[2] = 90; p[3] = 160; p[4] = 130; p[5] = 4; p[6] = 9; p[8] = 8; p[9] = 1;
	p[10] = 0x34; p[11] = 0x12; p[12] = 0; p[13] = 6; p[14] = 3; p[15] = 55;
	CHECK(decoder_parse(p, &s) && (s.flags & HS_INFO) && s.imu == 8 && s.mcu == 9 && s.board == 4 &&
		      s.fw_date == 0x1234 && s.fw[1] == 6 && s.fw[2] == 3 && s.rssi == 55,
	      "type 0");

	memset(p, 0, 16);
	p[0] = 6; p[2] = 1; p[3] = 0x10; p[4] = 0x27; p[15] = 40;
	CHECK(decoder_parse(p, &s) && s.sleep_ms == 10000 && s.button == 1 && s.rssi == 40, "type 6");

	memset(p, 0, 16);
	p[0] = 1; // all-zero quaternion
	CHECK(!decoder_parse(p, &s), "zero quaternion accepted");

	memset(p, 0, 16);
	p[0] = 9;
	CHECK(!decoder_parse(p, &s), "unknown type accepted");
}

// Full wire path: packet -> CRC16 -> COBS -> deframe -> check -> decode.
static void test_wire_frame(void)
{
	float wq[4] = {0.7071f, 0.7071f, 0, 0}, a[3] = {0};
	uint8_t pkt[16], frame[HUB_FRAME_LEN], enc[COBS_MAX_ENCODED(HUB_FRAME_LEN)], dec[32];
	make_packet_1(pkt, 0, wq, a);
	memcpy(frame, pkt, 16);
	uint16_t crc = hub_crc16(pkt, 16);
	frame[16] = (uint8_t)crc;
	frame[17] = (uint8_t)(crc >> 8);
	size_t n = cobs_encode(frame, sizeof(frame), enc);
	CHECK(n <= 19, "frame encodes to %zu bytes", n);
	int m = cobs_decode(enc, n, dec, sizeof(dec));
	CHECK(m == HUB_FRAME_LEN, "frame decode len %d", m);
	CHECK(hub_crc16(dec, 16) == (uint16_t)(dec[16] | (dec[17] << 8)), "frame crc");
	hub_sample_t s;
	CHECK(decoder_parse(dec, &s) && angle_deg(wq, s.q) < 0.01f, "frame decode");
}


// Deframer on a byte stream: partial first frame, good frames, corrupted and overlong frames.
static void test_deframer(void)
{
	deframer_t d;
	deframer_reset(&d);
	uint8_t stream[512], pkt[16], out[16];
	size_t n = 0;
	int ok = 0, bad = 0;

	// Tail of a frame from before we started listening: must not count as bad.
	stream[n++] = 0x11;
	stream[n++] = 0x22;
	stream[n++] = 0;
	for (int i = 0; i < 5; i++) {
		memset(pkt, 0, 16);
		pkt[0] = 1;
		pkt[1] = (uint8_t)i;
		pkt[9] = 0x40; // w = 0.5
		n += deframer_encode(pkt, stream + n);
	}
	// Corrupt one byte of a frame (not the delimiter).
	size_t f = n;
	n += deframer_encode(pkt, stream + n);
	stream[f + 5] ^= 0x80;
	if (stream[f + 5] == 0)
		stream[f + 5] = 0x80;
	// Overlong garbage.
	for (int i = 0; i < 40; i++)
		stream[n++] = 0x5A;
	stream[n++] = 0;
	// One more good frame, then back-to-back delimiters.
	n += deframer_encode(pkt, stream + n);
	stream[n++] = 0;
	stream[n++] = 0;

	for (size_t i = 0; i < n; i++) {
		df_result_t r = deframer_push(&d, stream[i], out);
		if (r == DF_OK) {
			ok++;
			CHECK(out[0] == 1, "deframed type");
		} else if (r == DF_BAD) {
			bad++;
		}
	}
	CHECK(ok == 6 && bad == 2, "deframer ok %d bad %d", ok, bad);
}

int main(void)
{
	srand(1234);
	test_crc();
	test_cobs();
	test_type1();
	test_type2();
	test_misc_types();
	test_wire_frame();
	test_deframer();
	if (failures) {
		printf("%d FAILURE(S)\n", failures);
		return 1;
	}
	printf("all host tests passed\n");
	return 0;
}
