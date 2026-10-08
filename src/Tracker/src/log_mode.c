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
// DEBUG and TRACE console modes.
//
// The firmware's own modules are compiled with LOG_LEVEL_DBG; runtime filtering keeps them at
// INFO until DEBUG is switched on. TRACE uses its own log source ("trace"), so it can be on
// without the DEBUG flood and the other way round. Frames are decoded from the bytes that go
// out, so the values include the packet quantisation, e.g.
//     radio id 0 #17 type 2: X 1.4 Y -3.9 Z 85.8 deg, q 0.7350 0.0120 -0.0340 0.6770, accel 0.02 -0.01 0.10 m/s2, batt 44% 3850 mV, 27.5 C
#include "globals.h"
#include "util.h"
#include "log_mode.h"

#if CONFIG_LOG_MODES

#include <math.h>
#include <zephyr/init.h>
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_REGISTER(trace, LOG_LEVEL_DBG);

#define RAD_TO_DEG 57.29577951f

static bool debug_on = IS_ENABLED(CONFIG_LOG_MODES_DEBUG_DEFAULT);
static uint32_t trace_every;
static atomic_t trace_count;

void log_mode_apply(void)
{
	int16_t trace_id = LOG_CURRENT_MODULE_ID();
	uint32_t n = log_src_cnt_get(Z_LOG_LOCAL_DOMAIN_ID);
	for (uint32_t s = 0; s < n; s++)
	{
		bool dbg = s == trace_id ? trace_every != 0 : debug_on;
		// Clamped to each module's compiled level, so Zephyr's own modules stay where Kconfig put them.
		log_filter_set(NULL, Z_LOG_LOCAL_DOMAIN_ID, s, dbg ? LOG_LEVEL_DBG : LOG_LEVEL_INF);
	}
}

void log_mode_set_debug(bool on)
{
	debug_on = on;
	log_mode_apply();
}

bool log_mode_debug(void)
{
	return debug_on;
}

void log_mode_set_trace(uint32_t every)
{
	trace_every = every;
	atomic_set(&trace_count, 0);
	log_mode_apply();
}

uint32_t log_mode_trace_every(void)
{
	return trace_every;
}

static inline int16_t rd_i16(const uint8_t *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

static inline uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// q as w, x, y, z. Types 1 and 4: Q15 in wire order x, y, z, w. Types 2 and 7: exponential map.
static void frame_quat(const uint8_t *p, float q[4])
{
	if (p[0] == 1 || p[0] == 4)
	{
		q[0] = rd_i16(p + 8) / 32768.0f;
		for (int i = 0; i < 3; i++)
			q[i + 1] = rd_i16(p + 2 + 2 * i) / 32768.0f;
		return;
	}
	uint32_t b = rd_u32(p + 5);
	float v[3] = {
		(float)FIXED_10_TO_DOUBLE(b & 1023) * 2 - 1,
		(float)FIXED_11_TO_DOUBLE((b >> 10) & 2047) * 2 - 1,
		(float)FIXED_11_TO_DOUBLE((b >> 21) & 2047) * 2 - 1,
	};
	q_iem(v, q);
}

// Roll (X), pitch (Y), yaw (Z) in degrees, ZYX order.
static void quat_euler(const float q[4], float e[3])
{
	float w = q[0], x = q[1], y = q[2], z = q[3];
	float sp = 2 * (w * y - z * x);
	sp = sp > 1 ? 1 : (sp < -1 ? -1 : sp);
	e[0] = atan2f(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * RAD_TO_DEG;
	e[1] = asinf(sp) * RAD_TO_DEG;
	e[2] = atan2f(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * RAD_TO_DEG;
}

static float frame_temp(uint8_t t) // 1-255 -> -38.5 - +88.5 C, 0 = no data
{
	return t ? (t - 128.5f) / 2 + 25 : NAN;
}

static int frame_batt(uint8_t b) // percent, 100 when charged, -1 = no battery
{
	return b == 255 ? 100 : (b & 0x80) ? (b & 0x7F) : -1;
}

static unsigned frame_batt_mv(uint8_t v)
{
	return v ? (v + 245) * 10 : 0;
}

void log_mode_trace(const char *route, const uint8_t *p, int seq)
{
	if (!trace_every)
		return;
	if (trace_every > 1 && (uint32_t)atomic_inc(&trace_count) % trace_every != 0)
		return;

	float q[4], e[3], v[3];
	switch (p[0])
	{
	case 0:
		LOG_DBG("%s id %u #%d type 0: info, batt %d%% %u mV, %.1f C, board %u mcu %u imu %u mag %u, fw %u.%u.%u",
			route, p[1], seq, frame_batt(p[2]), frame_batt_mv(p[3]), (double)frame_temp(p[4]),
			p[5], p[6], p[8], p[9], p[12], p[13], p[14]);
		return;
	case 1:
	case 4:
	case 2:
	case 7:
		frame_quat(p, q);
		quat_euler(q, e);
		for (int i = 0; i < 3; i++) // accel (types 1, 2, 7) m/s2, Q7; magnetometer (type 4) gauss, Q10
			v[i] = p[0] == 4 ? rd_i16(p + 10 + 2 * i) / 1024.0f
				: rd_i16(p + (p[0] == 1 ? 10 : 9) + 2 * i) / 128.0f;
		break;
	case 3:
		LOG_DBG("%s id %u #%d type 3: status, server %u, tracker %u", route, p[1], seq, p[2], p[3]);
		return;
	case 5:
	{
		int64_t runtime_us = (int64_t)((uint64_t)rd_u32(p + 2) | ((uint64_t)rd_u32(p + 6) << 32));
		LOG_DBG("%s id %u #%d type 5: runtime estimate %lld min", route, p[1], seq,
			runtime_us < 0 ? -1LL : runtime_us / 60000000);
		return;
	}
	case 6:
		LOG_DBG("%s id %u #%d type 6: button %u, sleep in %u ms", route, p[1], seq, p[2], rd_u16(p + 3));
		return;
	default:
		LOG_DBG("%s id %u #%d type %u: unknown", route, p[1], seq, p[0]);
		return;
	}

	switch (p[0])
	{
	case 1:
		LOG_DBG("%s id %u #%d type 1: X %.1f Y %.1f Z %.1f deg, q %.4f %.4f %.4f %.4f, accel %.2f %.2f %.2f m/s2",
			route, p[1], seq, (double)e[0], (double)e[1], (double)e[2],
			(double)q[0], (double)q[1], (double)q[2], (double)q[3], (double)v[0], (double)v[1], (double)v[2]);
		break;
	case 4:
		LOG_DBG("%s id %u #%d type 4: X %.1f Y %.1f Z %.1f deg, q %.4f %.4f %.4f %.4f, mag %.3f %.3f %.3f G",
			route, p[1], seq, (double)e[0], (double)e[1], (double)e[2],
			(double)q[0], (double)q[1], (double)q[2], (double)q[3], (double)v[0], (double)v[1], (double)v[2]);
		break;
	case 2:
		LOG_DBG("%s id %u #%d type 2: X %.1f Y %.1f Z %.1f deg, q %.4f %.4f %.4f %.4f, accel %.2f %.2f %.2f m/s2, batt %d%% %u mV, %.1f C",
			route, p[1], seq, (double)e[0], (double)e[1], (double)e[2],
			(double)q[0], (double)q[1], (double)q[2], (double)q[3], (double)v[0], (double)v[1], (double)v[2],
			frame_batt(p[2]), frame_batt_mv(p[3]), (double)frame_temp(p[4]));
		break;
	case 7:
		LOG_DBG("%s id %u #%d type 7: X %.1f Y %.1f Z %.1f deg, q %.4f %.4f %.4f %.4f, accel %.2f %.2f %.2f m/s2, button %u, sleep in %u ms",
			route, p[1], seq, (double)e[0], (double)e[1], (double)e[2],
			(double)q[0], (double)q[1], (double)q[2], (double)q[3], (double)v[0], (double)v[1], (double)v[2],
			p[2], rd_u16(p + 3));
		break;
	}
}

// Logging and the RTT backend are up by now; the USB console backend calls log_mode_apply()
// itself each time it is enabled.
static int log_mode_init(void)
{
	log_mode_apply();
	return 0;
}

SYS_INIT(log_mode_init, APPLICATION, 99);

#endif
