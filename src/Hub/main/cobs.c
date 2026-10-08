#include "cobs.h"

size_t cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
	size_t code_pos = 0;
	size_t w = 1;
	uint8_t code = 1;

	for (size_t r = 0; r < len; r++) {
		if (in[r] == 0) {
			out[code_pos] = code;
			code_pos = w++;
			code = 1;
			continue;
		}
		out[w++] = in[r];
		if (++code == 0xFF) {
			out[code_pos] = code;
			code_pos = w++;
			code = 1;
		}
	}
	out[code_pos] = code;
	return w;
}

int cobs_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_max)
{
	size_t r = 0;
	size_t w = 0;

	while (r < len) {
		uint8_t code = in[r++];
		if (code == 0)
			return -1;
		for (uint8_t i = 1; i < code; i++) {
			if (r >= len || w >= out_max || in[r] == 0)
				return -1;
			out[w++] = in[r++];
		}
		// A code below 0xFF implies a zero, except at the very end of the frame.
		if (code != 0xFF && r < len) {
			if (w >= out_max)
				return -1;
			out[w++] = 0;
		}
	}
	return (int)w;
}
