// test_zrle_host.c — drives vncd's ZRLE tile encoder over images and pixel
// formats and writes each case for tools/test_zrle_host.sh's independent
// Python decoder: u32 w, h; the format's ten fields; w*h XRGB; u32 n; n bytes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../userland/apps/vncd/zrle.h"

static uint32_t rng = 0x2545F491u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void emit(FILE *f, uint32_t w, uint32_t h, const vnc_format_t *pf, const uint32_t *px)
{
	size_t cap = zrle_bound(w, h, pf);
	uint8_t *out = malloc(cap + 1);
	size_t n = zrle_tiles(px, w, w, h, pf, out);
	if (n > cap) { fprintf(stderr, "bound exceeded\n"); exit(1); }
	uint32_t head[2] = { w, h };
	fwrite(head, 4, 2, f);
	uint8_t fmt[10] = { pf->bpp, pf->depth, pf->big_endian, (uint8_t)pf->rmax, (uint8_t)pf->gmax,
	                    (uint8_t)pf->bmax, pf->rshift, pf->gshift, pf->bshift, 0 };
	fwrite(fmt, 1, 10, f);
	fwrite(px, 4, (size_t)w * h, f);
	uint32_t len = (uint32_t)n;
	fwrite(&len, 4, 1, f);
	fwrite(out, 1, n, f);
	free(out);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	FILE *f = fopen(argv[1], "wb");
	const vnc_format_t formats[] = {
		{ 32, 24, 0, 255, 255, 255, 16, 8, 0 },   // vncd's native: CPIXEL is the low 3 bytes
		{ 32, 24, 1, 255, 255, 255, 16, 8, 0 },   // big-endian
		{ 32, 24, 0, 255, 255, 255, 24, 16, 8 },  // colour in the high 3 bytes
		{ 32, 32, 0, 255, 255, 255, 16, 8, 0 },   // depth 32: no compaction
		{ 16, 16, 0, 31, 63, 31, 11, 5, 0 },      // RGB565
		{ 16, 16, 1, 31, 63, 31, 11, 5, 0 },      // RGB565 big-endian
		{ 8, 8, 0, 7, 7, 3, 0, 3, 6 },            // BGR233
	};
	const uint32_t sizes[][2] = { {1,1}, {64,64}, {65,63}, {130,70}, {200,129}, {7,300} };
	for (unsigned fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++)
		for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
			for (int kind = 0; kind < 5; kind++)
			{
				uint32_t w = sizes[si][0], h = sizes[si][1];
				uint32_t *px = malloc((size_t)w * h * 4);
				uint32_t pal[20];
				for (int i = 0; i < 20; i++) pal[i] = next() & 0xFFFFFF;
				for (uint32_t i = 0; i < w * h; i++)
				{
					uint32_t x = i % w, y = i / w;
					switch (kind)
					{
						case 0: px[i] = pal[0]; break;                                   // solid
						case 1: px[i] = pal[(x / 5 + y / 3) % 2]; break;                  // two colours
						case 2: px[i] = pal[(x * 7 + y * 3) % 13]; break;                 // a palette
						case 3: px[i] = (x * 255 / w) << 16 | (y * 255 / h) << 8 | 0x40; break;  // gradient
						default: px[i] = (next() % 3 == 0) ? next() & 0xFFFFFF : pal[1]; break;  // noise on a field
					}
				}
				emit(f, w, h, &formats[fi], px);
				free(px);
			}
	fclose(f);
	return 0;
}
