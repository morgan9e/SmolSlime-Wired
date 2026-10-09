// Byte-wise deframer for the receiver -> hub UART link:
// COBS([16-byte packet][CRC16-CCITT little-endian]) followed by 0x00.
// Portable: used by the UART task and by the host simulation.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "decoder.h"

#define DEFRAMER_MAX_ENCODED 32 // a valid frame encodes to 19 bytes

typedef enum {
	DF_NONE = 0, // byte consumed, no frame finished
	DF_OK,       // a valid frame finished; pkt holds the 16-byte packet
	DF_BAD,      // a frame finished but was malformed or failed CRC16
} df_result_t;

typedef struct {
	uint8_t buf[DEFRAMER_MAX_ENCODED];
	size_t len;
	bool overlong;
	bool synced; // false until the first delimiter, so a partial first frame is not counted as bad
} deframer_t;

void deframer_reset(deframer_t *d);
df_result_t deframer_push(deframer_t *d, uint8_t c, uint8_t pkt[HUB_PKT_LEN]);

// Builds one wire frame (with trailing 0x00) from a 16-byte packet. out must hold 20 bytes.
// Returns the number of bytes written. Used by tests and the simulator.
size_t deframer_encode(const uint8_t pkt[HUB_PKT_LEN], uint8_t *out);
