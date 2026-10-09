// Consistent Overhead Byte Stuffing (COBS) for the receiver -> hub UART link.
// Frames on the wire are COBS(payload) followed by a single 0x00 delimiter.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Worst-case encoded size of n payload bytes, excluding the 0x00 delimiter.
#define COBS_MAX_ENCODED(n) ((n) + ((n) / 254) + 1)

// Encodes len bytes from in to out. out must hold COBS_MAX_ENCODED(len) bytes.
// Returns the encoded length. The 0x00 delimiter is not written.
size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out);

// Decodes len bytes (delimiter already stripped) from in to out.
// Returns the decoded length, or -1 if the input is malformed or out_max is too small.
int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_max);
