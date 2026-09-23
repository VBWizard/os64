// zrle.c — ZRLE tiles (zrle.h is the contract; RFC 6143 section 7.7.6).

#include "zrle.h"

uint32_t vnc_pixel(uint32_t xrgb, const vnc_format_t *pf)
{
	uint32_t r = (xrgb >> 16) & 0xFF, g = (xrgb >> 8) & 0xFF, b = xrgb & 0xFF;
	return ((r * pf->rmax + 127) / 255) << pf->rshift |
	       ((g * pf->gmax + 127) / 255) << pf->gshift |
	       ((b * pf->bmax + 127) / 255) << pf->bshift;
}

// Where a 32-bit pixel's colour bits live: the low three bytes, the high
// three, or neither (then a CPIXEL is the whole PIXEL).
enum { CPIXEL_FULL, CPIXEL_LOW3, CPIXEL_HIGH3 };

static int cpixel_kind(const vnc_format_t *pf)
{
	if (pf->bpp != 32 || pf->depth > 24)
		return CPIXEL_FULL;
	uint32_t bits = (uint32_t)pf->rmax << pf->rshift | (uint32_t)pf->gmax << pf->gshift |
	                (uint32_t)pf->bmax << pf->bshift;
	if (bits < (1u << 24))
		return CPIXEL_LOW3;
	if ((bits & 0xFF) == 0)
		return CPIXEL_HIGH3;
	return CPIXEL_FULL;
}

uint32_t zrle_cpixel_bytes(const vnc_format_t *pf)
{
	return cpixel_kind(pf) == CPIXEL_FULL ? pf->bpp / 8u : 3u;
}

static uint8_t *put_cpixel(uint8_t *out, uint32_t v, const vnc_format_t *pf)
{
	int kind = cpixel_kind(pf);
	uint32_t n = pf->bpp / 8u;
	if (kind != CPIXEL_FULL)
	{
		if (kind == CPIXEL_HIGH3)
			v >>= 8;
		n = 3;
	}
	for (uint32_t i = 0; i < n; i++)
		out[pf->big_endian ? n - 1 - i : i] = (uint8_t)(v >> (8 * i));
	return out + n;
}

// A run length as ZRLE spells it: (length - 1) as 255s and a final remainder.
static uint8_t *put_run(uint8_t *out, uint32_t length)
{
	length--;
	while (length >= 255)
	{
		*out++ = 255;
		length -= 255;
	}
	*out++ = (uint8_t)length;
	return out;
}

static uint32_t run_bytes(uint32_t length)
{
	return (length - 1) / 255 + 1;
}

size_t zrle_bound(uint32_t w, uint32_t h, const vnc_format_t *pf)
{
	// Raw is the ceiling: the chooser never picks a shape that costs more.
	size_t tiles = (size_t)((w + ZRLE_TILE - 1) / ZRLE_TILE) * ((h + ZRLE_TILE - 1) / ZRLE_TILE);
	return tiles + (size_t)w * h * zrle_cpixel_bytes(pf);
}

// One tile of tw x th pixel VALUES (already in the viewer's format).
static uint8_t *tile(const uint32_t *v, uint32_t tw, uint32_t th, const vnc_format_t *pf, uint8_t *out)
{
	uint32_t count = tw * th, cb = zrle_cpixel_bytes(pf);

	// The palette, as far as 16 colours go; `palette_full` past that.
	uint32_t palette[16];
	uint32_t colours = 0;
	bool palette_full = false;
	for (uint32_t i = 0; i < count && !palette_full; i++)
	{
		uint32_t c = 0;
		while (c < colours && palette[c] != v[i])
			c++;
		if (c == colours)
		{
			if (colours == 16)
				palette_full = true;
			else
				palette[colours++] = v[i];
		}
	}

	if (!palette_full && colours == 1)
	{
		*out++ = 1;                          // solid
		return put_cpixel(out, v[0], pf);
	}

	// The costs of every shape that applies; the cheapest wins.
	size_t raw = (size_t)count * cb;
	size_t rle = 0, prle = 0;
	for (uint32_t i = 0; i < count; )
	{
		uint32_t j = i + 1;
		while (j < count && v[j] == v[i])
			j++;
		uint32_t len = j - i;
		rle += cb + run_bytes(len);
		prle += 1 + (len > 1 ? run_bytes(len) : 0);
		i = j;
	}
	size_t packed = (size_t)-1;
	uint32_t bits = colours <= 2 ? 1 : colours <= 4 ? 2 : 4;
	if (!palette_full)
	{
		packed = (size_t)colours * cb + (size_t)th * ((tw * bits + 7) / 8);
		prle += (size_t)colours * cb;
	}
	else
		prle = (size_t)-1;

	size_t best = raw;
	int shape = 0;
	if (packed < best) { best = packed; shape = 2; }
	if (rle < best) { best = rle; shape = 128; }
	if (prle < best) { best = prle; shape = 130; }

	if (shape == 0)
	{
		*out++ = 0;
		for (uint32_t i = 0; i < count; i++)
			out = put_cpixel(out, v[i], pf);
		return out;
	}
	if (shape == 128)
	{
		*out++ = 128;
		for (uint32_t i = 0; i < count; )
		{
			uint32_t j = i + 1;
			while (j < count && v[j] == v[i])
				j++;
			out = put_cpixel(out, v[i], pf);
			out = put_run(out, j - i);
			i = j;
		}
		return out;
	}

	// The palette shapes: the palette, then indices.
	*out++ = (uint8_t)(shape == 2 ? colours : 128 + colours);
	for (uint32_t c = 0; c < colours; c++)
		out = put_cpixel(out, palette[c], pf);
	#define INDEX_OF(value, idx) do { idx = 0; while (palette[idx] != (value)) idx++; } while (0)
	if (shape == 2)
	{
		for (uint32_t y = 0; y < th; y++)
		{
			uint32_t acc = 0, nbits = 0;
			for (uint32_t x = 0; x < tw; x++)
			{
				uint32_t idx;
				INDEX_OF(v[y * tw + x], idx);
				acc = (acc << bits) | idx;
				nbits += bits;
				if (nbits == 8)
				{
					*out++ = (uint8_t)acc;
					acc = 0;
					nbits = 0;
				}
			}
			if (nbits)
				*out++ = (uint8_t)(acc << (8 - nbits));   // rows start on a byte
		}
		return out;
	}
	for (uint32_t i = 0; i < count; )
	{
		uint32_t j = i + 1;
		while (j < count && v[j] == v[i])
			j++;
		uint32_t idx;
		INDEX_OF(v[i], idx);
		if (j - i == 1)
			*out++ = (uint8_t)idx;
		else
		{
			*out++ = (uint8_t)(idx | 128);
			out = put_run(out, j - i);
		}
		i = j;
	}
	#undef INDEX_OF
	return out;
}

size_t zrle_tiles(const uint32_t *px, size_t stride, uint32_t w, uint32_t h,
                  const vnc_format_t *pf, uint8_t *out)
{
	uint8_t *start = out;
	uint32_t values[ZRLE_TILE * ZRLE_TILE];
	for (uint32_t ty = 0; ty < h; ty += ZRLE_TILE)
		for (uint32_t tx = 0; tx < w; tx += ZRLE_TILE)
		{
			uint32_t tw = w - tx < ZRLE_TILE ? w - tx : ZRLE_TILE;
			uint32_t th = h - ty < ZRLE_TILE ? h - ty : ZRLE_TILE;
			for (uint32_t y = 0; y < th; y++)
				for (uint32_t x = 0; x < tw; x++)
					values[y * tw + x] = vnc_pixel(px[(size_t)(ty + y) * stride + tx + x], pf);
			out = tile(values, tw, th, pf, out);
		}
	return (size_t)(out - start);
}
