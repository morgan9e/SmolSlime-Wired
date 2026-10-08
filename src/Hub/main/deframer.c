#include "deframer.h"

#include <string.h>

#include "cobs.h"
#include "crc16.h"

void deframer_reset(deframer_t *d)
{
	memset(d, 0, sizeof(*d));
}

df_result_t deframer_push(deframer_t *d, uint8_t c, uint8_t pkt[HUB_PKT_LEN])
{
	if (c != 0) {
		if (d->len < sizeof(d->buf))
			d->buf[d->len++] = c;
		else
			d->overlong = true;
		return DF_NONE;
	}

	bool had_frame = d->synced && d->len > 0;
	bool overlong = d->overlong;
	size_t len = d->len;
	d->synced = true;
	d->len = 0;
	d->overlong = false;
	if (!had_frame)
		return DF_NONE;
	if (overlong)
		return DF_BAD;

	uint8_t dec[DEFRAMER_MAX_ENCODED];
	if (cobs_decode(d->buf, len, dec, sizeof(dec)) != HUB_FRAME_LEN)
		return DF_BAD;
	uint16_t crc = (uint16_t)(dec[HUB_PKT_LEN] | (dec[HUB_PKT_LEN + 1] << 8));
	if (hub_crc16(dec, HUB_PKT_LEN) != crc)
		return DF_BAD;
	memcpy(pkt, dec, HUB_PKT_LEN);
	return DF_OK;
}

size_t deframer_encode(const uint8_t pkt[HUB_PKT_LEN], uint8_t *out)
{
	uint8_t raw[HUB_FRAME_LEN];
	memcpy(raw, pkt, HUB_PKT_LEN);
	uint16_t crc = hub_crc16(pkt, HUB_PKT_LEN);
	raw[HUB_PKT_LEN] = (uint8_t)crc;
	raw[HUB_PKT_LEN + 1] = (uint8_t)(crc >> 8);
	size_t n = cobs_encode(raw, sizeof(raw), out);
	out[n++] = 0;
	return n;
}
