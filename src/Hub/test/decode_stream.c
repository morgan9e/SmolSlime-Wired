// Runs a captured byte stream (e.g. from tools/fake_receiver.py --out) through the
// firmware's deframer and decoder and prints a summary.
#include <stdio.h>

#include "deframer.h"

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s stream.bin\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	deframer_t d;
	deframer_reset(&d);
	unsigned ok = 0, bad = 0, unknown = 0, by_type[256] = {0};
	uint8_t pkt[HUB_PKT_LEN];
	int c, printed = 0;
	while ((c = fgetc(f)) != EOF) {
		df_result_t r = deframer_push(&d, (uint8_t)c, pkt);
		if (r == DF_BAD)
			bad++;
		if (r != DF_OK)
			continue;
		ok++;
		hub_sample_t s;
		if (!decoder_parse(pkt, &s)) {
			unknown++;
			continue;
		}
		by_type[s.type]++;
		if ((s.flags & HS_ROT) && s.id == 2 && printed < 3) {
			printed++;
			printf("id 2 q = (%.4f, %.4f, %.4f, %.4f) a = (%.3f, %.3f, %.3f)\n", s.q[0], s.q[1], s.q[2], s.q[3],
			       s.a[0], s.a[1], s.a[2]);
		}
	}
	printf("ok %u bad %u unknown %u | type0 %u type1 %u type255 %u\n", ok, bad, unknown, by_type[0], by_type[1],
	       by_type[255]);
	return bad || unknown ? 1 : 0;
}
