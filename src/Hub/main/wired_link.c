#include "wired_link.h"

#include <string.h>

#include "cobs.h"

#define CRC32K_POLY 0x93a409ebu
#define CRC32K_SEED 0x93a409ebu

uint32_t wl_crc32k(const uint8_t *data, size_t len)
{
	uint32_t crc = CRC32K_SEED;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint32_t)data[i] << 24;
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ CRC32K_POLY : crc << 1;
	}
	return crc;
}

void wl_reset(wl_deframer_t *d)
{
	d->len = 0;
	d->overlong = false;
	d->synced = false;
}

static wl_result_t frame_done(const wl_deframer_t *d, wl_frame_t *out)
{
	uint8_t raw[WL_MAX_ENCODED];
	if (d->overlong)
		return WL_BAD;
	int n = cobs_decode(d->buf, d->len, raw, sizeof(raw));
	if (n == WL_HELLO_LEN && raw[0] == WL_TYPE_HELLO) {
		memcpy(out->addr, &raw[1], 6);
		memcpy(out->fw, &raw[7], 3);
		return WL_HELLO;
	}
	if (n == WL_ADATA_LEN && raw[0] == WL_TYPE_ADATA) {
		uint32_t crc = (uint32_t)raw[23] | ((uint32_t)raw[24] << 8) | ((uint32_t)raw[25] << 16) |
			       ((uint32_t)raw[26] << 24);
		if (crc != wl_crc32k(&raw[1], 6 + HUB_PKT_LEN))
			return WL_BAD;
		memcpy(out->addr, &raw[1], 6);
		memcpy(out->pkt, &raw[7], HUB_PKT_LEN);
		out->seq = raw[27];
		return WL_DATA;
	}
	return WL_BAD; // malformed COBS, wrong length, or a type the hub does not use (legacy DATA)
}

wl_result_t wl_push(wl_deframer_t *d, uint8_t c, wl_frame_t *out)
{
	if (c != 0) {
		if (d->len < sizeof(d->buf))
			d->buf[d->len++] = c;
		else
			d->overlong = true;
		return WL_NONE;
	}
	wl_result_t r = WL_NONE;
	if (d->synced && (d->len > 0 || d->overlong))
		r = frame_done(d, out);
	d->synced = true;
	d->len = 0;
	d->overlong = false;
	return r;
}

static size_t encode(const uint8_t *raw, size_t len, uint8_t *out)
{
	size_t n = cobs_encode(raw, len, out);
	out[n++] = 0;
	return n;
}

size_t wl_encode_adata(const uint8_t addr[6], const uint8_t pkt[HUB_PKT_LEN], uint8_t seq, uint8_t *out)
{
	uint8_t raw[WL_ADATA_LEN];
	raw[0] = WL_TYPE_ADATA;
	memcpy(&raw[1], addr, 6);
	memcpy(&raw[7], pkt, HUB_PKT_LEN);
	uint32_t crc = wl_crc32k(&raw[1], 6 + HUB_PKT_LEN);
	for (int i = 0; i < 4; i++)
		raw[23 + i] = (uint8_t)(crc >> (8 * i));
	raw[27] = seq;
	return encode(raw, sizeof(raw), out);
}

size_t wl_encode_hello(const uint8_t addr[6], const uint8_t fw[3], uint8_t *out)
{
	uint8_t raw[WL_HELLO_LEN];
	raw[0] = WL_TYPE_HELLO;
	memcpy(&raw[1], addr, 6);
	memcpy(&raw[7], fw, 3);
	return encode(raw, sizeof(raw), out);
}

int wl_table_id(wl_table_t *t, const uint8_t addr[6], bool *added)
{
	*added = false;
	for (int i = 0; i < t->count; i++)
		if (memcmp(t->addr[i], addr, 6) == 0)
			return i;
	if (t->count >= WL_MAX_IDS)
		return -1;
	memcpy(t->addr[t->count], addr, 6);
	*added = true;
	return t->count++;
}
