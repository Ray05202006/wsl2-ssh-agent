/*
 * gen SEED IN EXPECTED REPLY OLD: write a random client byte stream to IN,
 * the exact output the helper must produce against the echo agent to
 * EXPECTED (OLD=1: extension requests are answered with FAILURE locally),
 * and a random, often malformed, agent reply stream to REPLY.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define MAXMSG (256 * 1024)
static uint64_t s;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
static void be32(FILE *f, uint32_t v) { fputc(v >> 24, f); fputc(v >> 16, f); fputc(v >> 8, f); fputc(v, f); }
static uint32_t pick_len(void) {
	static const uint32_t edge[] = {0, 1, 2, 4, 5, 7, 8, MAXMSG - 1, MAXMSG, MAXMSG + 1, MAXMSG + 4, MAXMSG + 16,
	                                MAXMSG + 64, 0x80000000u, 0xffffffffu, 0xfffffffbu};
	switch (rnd() % 6) {
	case 0: return edge[rnd() % (sizeof edge / sizeof *edge)];
	case 1: return 1 + rnd() % 65536;
	default: return 1 + rnd() % 64;
	}
}
int main(int argc, char **argv) {
	s = strtoull(argv[1], 0, 10) * 2654435761u + 1;
	int old = argv[5][0] == '1';
	FILE *in = fopen(argv[2], "wb"), *ex = fopen(argv[3], "wb"), *rp = fopen(argv[4], "wb");
	int frames = 1 + rnd() % 6, stop = 0;
	for (int i = 0; i < frames && !stop; i++) {
		uint32_t n = pick_len();
		int type = rnd() % 4 == 0 ? 27 : (int)(rnd() % 256);
		int trunc = rnd() % 10 == 0;
		be32(in, n);
		if (n == 0 || n > MAXMSG) {
			/* must be rejected; sometimes send a full body for lengths just
			 * over the limit so an off-by-N check would actually overflow */
			uint32_t body = n > MAXMSG && n <= MAXMSG + 64 && rnd() % 2 ? n : rnd() % 100;
			for (uint32_t k = 0; k < body; k++) fputc(rnd(), in);
			break;
		}
		uint32_t body = trunc ? rnd() % n : n;
		unsigned char *b = malloc(n);
		for (uint32_t k = 0; k < n; k++) b[k] = (unsigned char)rnd();
		b[0] = (unsigned char)type;
		fwrite(b, 1, body, in);
		if (trunc) { stop = 1; free(b); break; }
		if (type == 27 && old) { be32(ex, 1); fputc(5, ex); }
		else { be32(ex, n); fwrite(b, 1, n, ex); }
		free(b);
	}
	/* hostile agent reply stream: random frames with random (often bad) lengths */
	for (int i = 1 + rnd() % 4; i; i--) {
		uint32_t n = pick_len(), body = rnd() % 3 ? (n < 70000 ? n : rnd() % 70000) : rnd() % 50;
		be32(rp, n);
		for (uint32_t k = 0; k < body; k++) fputc(rnd(), rp);
	}
	fclose(in); fclose(ex); fclose(rp);
	return 0;
}
