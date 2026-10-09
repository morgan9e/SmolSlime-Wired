// Decoder for the 16-byte SlimeVR nRF tracker packets, as mirrored by the receiver.
// Byte layouts follow connection_write_packet_n() in SlimeVR-Tracker-nRF
// (src/connection/connection.c) and HIDCommon.kt in SlimeVR-Server.
// This file has no ESP-IDF dependencies so it can be unit tested on a PC.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HUB_PKT_LEN 16
#define HUB_FRAME_LEN (HUB_PKT_LEN + 2) // packet + CRC16, before COBS

enum {
	HS_ADDR = 1 << 0,    // type 255: radio address of an id
	HS_INFO = 1 << 1,    // type 0: board, MCU, IMU, magnetometer, firmware
	HS_ROT = 1 << 2,     // q is valid
	HS_ACCEL = 1 << 3,   // a is valid
	HS_BATT = 1 << 4,    // batt, batt_v are valid
	HS_TEMP = 1 << 5,    // temp is valid (0 still means "no reading")
	HS_RSSI = 1 << 6,    // rssi is valid
	HS_STATUS = 1 << 7,  // svr_status, status are valid
	HS_BUTTON = 1 << 8,  // button, sleep_ms are valid
	HS_RUNTIME = 1 << 9, // runtime_us is valid
};

typedef struct {
	int64_t t_us;        // hub receive time, filled by the UART task
	uint16_t flags;      // HS_* bits
	uint8_t type;        // raw packet type
	uint8_t id;          // receiver-assigned tracker id
	float q[4];          // x, y, z, w in the tracker's sensor frame, unit length
	float a[3];          // acceleration, m/s^2, sensor frame
	uint8_t addr[6];     // radio address, as stored by the receiver (little-endian)
	uint8_t batt;        // bit 7 charging, bits 0-6 percent; 128 = unknown
	uint8_t batt_v;      // volts = (batt_v + 245) / 100
	uint8_t temp;        // celsius = temp / 2 - 39; 0 = no reading
	uint8_t rssi;        // positive magnitude, dBm = -rssi
	uint8_t board, mcu, imu, mag;
	uint8_t fw[3];       // major, minor, patch
	uint16_t fw_date;    // (year - 2020) << 9 | month << 5 | day
	uint8_t svr_status;  // SVR_STATUS_* from the tracker
	uint8_t status;      // raw tracker status bits
	uint8_t button;
	uint16_t sleep_ms;
	int64_t runtime_us;
} hub_sample_t;

// Decodes one 16-byte packet. Returns false for unknown types or a degenerate quaternion
// on a rotation-only packet. Never reads beyond 16 bytes.
bool decoder_parse(const uint8_t pkt[HUB_PKT_LEN], hub_sample_t *out);

// Packet types 224-254 come from the receiver itself (link diagnostics: 253 receiver,
// 254 per tracker; layout in the receiver's HUB.md). The hub forwards them unchanged to
// UDP 6971 on the server's PC and never decodes them.
static inline bool decoder_is_receiver_diag(uint8_t type)
{
	return type >= 224 && type <= 254;
}

// Inverse of the tracker's q_fem() exponential-map quantisation (types 2 and 7).
// q_out is x, y, z, w.
void decoder_expmap_to_quat(uint32_t q_buf, float q_out[4]);
