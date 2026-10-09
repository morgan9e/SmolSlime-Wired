#include "decoder.h"

#include <math.h>
#include <string.h>

#define HALF_PI_F 1.57079632679489661923f

static inline uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline int16_t rd_i16(const uint8_t *p)
{
	return (int16_t)rd_u16(p);
}

static inline uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t rd_u64(const uint8_t *p)
{
	return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
}

// Q15 quaternion in wire order x, y, z, w (connection_write_packet_1 writes q[1], q[2], q[3], q[0]).
static void rd_q15_quat(const uint8_t *p, float q[4])
{
	const float s = 1.0f / 32768.0f;
	for (int i = 0; i < 4; i++)
		q[i] = rd_i16(p + 2 * i) * s;
}

// Q7 vector, m/s^2.
static void rd_q7_vec(const uint8_t *p, float v[3])
{
	const float s = 1.0f / 128.0f;
	for (int i = 0; i < 3; i++)
		v[i] = rd_i16(p + 2 * i) * s;
}

void decoder_expmap_to_quat(uint32_t q_buf, float q[4])
{
	float v[3] = {
		(float)(q_buf & 1023u) / 1024.0f,
		(float)((q_buf >> 10) & 2047u) / 2048.0f,
		(float)((q_buf >> 21) & 2047u) / 2048.0f,
	};
	for (int i = 0; i < 3; i++)
		v[i] = v[i] * 2.0f - 1.0f;
	// Same maths as the tracker's q_iem() and the server's HIDCommon.kt.
	float d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	float inv_sqrt_d = 1.0f / sqrtf(d + 1e-6f);
	float a = HALF_PI_F * d * inv_sqrt_d;
	float k = sinf(a) * inv_sqrt_d;
	q[0] = k * v[0];
	q[1] = k * v[1];
	q[2] = k * v[2];
	q[3] = cosf(a);
}

static bool normalize(float q[4])
{
	float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	if (!(n > 0.25f) || !(n < 4.0f)) // also rejects NaN
		return false;
	float inv = 1.0f / sqrtf(n);
	for (int i = 0; i < 4; i++)
		q[i] *= inv;
	return true;
}

bool decoder_parse(const uint8_t p[HUB_PKT_LEN], hub_sample_t *o)
{
	memset(o, 0, sizeof(*o));
	o->type = p[0];
	o->id = p[1];

	switch (p[0]) {
	case 0: // device info
		o->flags = HS_INFO | HS_BATT | HS_TEMP | HS_RSSI;
		o->batt = p[2];
		o->batt_v = p[3];
		o->temp = p[4];
		o->board = p[5];
		o->mcu = p[6];
		o->imu = p[8];
		o->mag = p[9];
		o->fw_date = rd_u16(p + 10);
		o->fw[0] = p[12];
		o->fw[1] = p[13];
		o->fw[2] = p[14];
		o->rssi = p[15];
		break;
	case 1: // full precision quat and accel, no RSSI
		o->flags = HS_ROT | HS_ACCEL;
		rd_q15_quat(p + 2, o->q);
		rd_q7_vec(p + 10, o->a);
		break;
	case 2: // reduced precision quat and accel with battery, temperature, RSSI
		o->flags = HS_ROT | HS_ACCEL | HS_BATT | HS_TEMP | HS_RSSI;
		o->batt = p[2];
		o->batt_v = p[3];
		o->temp = p[4];
		decoder_expmap_to_quat(rd_u32(p + 5), o->q);
		rd_q7_vec(p + 9, o->a);
		o->rssi = p[15];
		break;
	case 3: // status
		o->flags = HS_STATUS | HS_RSSI;
		o->svr_status = p[2];
		o->status = p[3];
		o->rssi = p[15];
		break;
	case 4: // full precision quat and magnetometer, no RSSI; magnetometer is unused
		o->flags = HS_ROT;
		rd_q15_quat(p + 2, o->q);
		break;
	case 5: // runtime estimate
		o->flags = HS_RUNTIME | HS_RSSI;
		o->runtime_us = (int64_t)rd_u64(p + 2);
		o->rssi = p[15];
		break;
	case 6: // button and sleep time
		o->flags = HS_BUTTON | HS_RSSI;
		o->button = p[2];
		o->sleep_ms = rd_u16(p + 3); // the tracker writes this little-endian
		o->rssi = p[15];
		break;
	case 7: // button and sleep time with reduced precision quat and accel
		o->flags = HS_BUTTON | HS_ROT | HS_ACCEL | HS_RSSI;
		o->button = p[2];
		o->sleep_ms = rd_u16(p + 3);
		decoder_expmap_to_quat(rd_u32(p + 5), o->q);
		rd_q7_vec(p + 9, o->a);
		o->rssi = p[15];
		break;
	case 255: // receiver: id <-> radio address
		o->flags = HS_ADDR;
		memcpy(o->addr, p + 2, 6);
		break;
	default:
		return false;
	}

	if ((o->flags & HS_ROT) && !normalize(o->q)) {
		o->flags &= (uint16_t)~(HS_ROT | HS_ACCEL);
		if (o->flags == 0)
			return false;
	}
	return true;
}
