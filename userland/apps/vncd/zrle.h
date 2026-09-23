#ifndef VNCD_ZRLE_H
#define VNCD_ZRLE_H

// zrle.h — RFB's ZRLE encoding, the tile half (RFC 6143 section 7.7.6).
// Pure: pixels in, bytes out, no I/O, so tools/test_zrle_host.sh proves it on
// the host against an independent decoder.
//
// A rectangle is cut into 64x64 tiles, left to right, top to bottom, and
// each tile picks the cheapest of five shapes: raw pixels, one solid colour,
// a packed palette of up to 16 colours, plain run-length, or run-length over
// a palette. A desktop is mostly flat colour and text, so most tiles are
// solid or a few colours. The tile bytes are then compressed as ONE zlib
// stream for the whole connection, flushed after each rectangle; that half
// lives in vncd.c, over libgzip.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A viewer's pixel format (RFB's PIXEL_FORMAT, true colour only).
typedef struct
{
	uint8_t  bpp;            // 8, 16 or 32
	uint8_t  depth;
	uint8_t  big_endian;
	uint16_t rmax, gmax, bmax;
	uint8_t  rshift, gshift, bshift;
} vnc_format_t;

#define ZRLE_TILE 64

// The pixel value an XRGB colour has in `pf`, from three 256-entry tables:
// each channel scaled to its max with rounding and shifted into place. Built
// once per rectangle, so a pixel costs a lookup per channel and two ORs
// instead of three multiplies and three divisions — the translation table
// every VNC server since the first has kept.
typedef struct
{
	uint32_t r[256], g[256], b[256];
} vnc_lut_t;

void vnc_lut_build(vnc_lut_t *lut, const vnc_format_t *pf);

static inline uint32_t vnc_lut_pixel(const vnc_lut_t *lut, uint32_t xrgb)
{
	return lut->r[(xrgb >> 16) & 0xFF] | lut->g[(xrgb >> 8) & 0xFF] | lut->b[xrgb & 0xFF];
}

// The bytes of a CPIXEL in `pf`: 3 where the RFC compacts a 32-bit pixel whose
// colour fits three bytes, else bpp / 8.
uint32_t zrle_cpixel_bytes(const vnc_format_t *pf);

// The most bytes zrle_tiles can write for a w x h rectangle.
size_t zrle_bound(uint32_t w, uint32_t h, const vnc_format_t *pf);

// Encode w x h XRGB pixels (row stride `stride`, in pixels) as ZRLE's
// uncompressed tile stream into `out`. Returns the bytes written.
size_t zrle_tiles(const uint32_t *px, size_t stride, uint32_t w, uint32_t h,
                  const vnc_format_t *pf, uint8_t *out);

#endif // VNCD_ZRLE_H
