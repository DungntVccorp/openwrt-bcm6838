/* Pack a binary as a Broadcom CFE "vmlinux.lz4":
 * 20-byte big-endian header (load, entry, clen, "BRCM", ulen) + one raw LZ4 block.
 * usage: brcmlz4 <in> <out> <load addr> <entry addr>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "lz4.h"
#include "lz4hc.h"

static void be32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

int main(int argc, char **argv)
{
	FILE *f;
	long n;
	char *src, *dst;
	int bound, clen;
	unsigned char hdr[20];

	if (argc != 5) {
		fprintf(stderr, "usage: %s in out load entry\n", argv[0]);
		return 1;
	}
	f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	src = malloc(n);
	if (fread(src, 1, n, f) != (size_t)n) { perror("read"); return 1; }
	fclose(f);

	bound = LZ4_compressBound(n);
	dst = malloc(bound);
	clen = LZ4_compress_HC(src, dst, n, bound, LZ4HC_CLEVEL_MAX);
	if (clen <= 0) { fprintf(stderr, "compression failed\n"); return 1; }

	be32(hdr, strtoul(argv[3], NULL, 0));
	be32(hdr + 4, strtoul(argv[4], NULL, 0));
	be32(hdr + 8, clen);
	hdr[12] = 'B'; hdr[13] = 'R'; hdr[14] = 'C'; hdr[15] = 'M';
	be32(hdr + 16, n);

	f = fopen(argv[2], "wb");
	if (!f) { perror(argv[2]); return 1; }
	fwrite(hdr, 1, sizeof(hdr), f);
	fwrite(dst, 1, clen, f);
	fclose(f);
	printf("%s: %ld -> %d bytes\n", argv[2], n, clen);
	return 0;
}
