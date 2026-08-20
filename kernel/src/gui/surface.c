// surface.c — GUI layers 0/1: RAM pixel surfaces + software rendering.
//
// Design rules (see gui/surface.h and GRAPHICS.md):
//  * All drawing happens in cacheable RAM. Only surface_flush_rect() touches
//    the uncached hardware framebuffer, and it is strictly write-only.
//  * Every primitive clips against the destination — callers never have to
//    pre-clip, and rects hanging off any edge are legal.
//  * Fixed 32-bpp XRGB throughout (what Limine gives us; X reserved).

#include "gui/surface.h"

#include "CONFIG.h"
#include "kmalloc.h"
#include "memset.h"
#include "serial_logging.h"
#include "video.h"
#include "BasicRenderer.h"

extern struct Framebuffer kFrameBuffer;
extern BasicRenderer kRenderer;

// The whole-surface rect, for clipping any caller rect to a surface.
static inline rect_t surface_bounds(const surface_t *s)
{
	return (rect_t){0, 0, (int32_t)s->width, (int32_t)s->height};
}

static inline uint32_t *surface_row(const surface_t *s, int32_t y)
{
	return s->pixels + (size_t)y * s->pitch_px;
}

int surface_init(surface_t *s, uint32_t w, uint32_t h)
{
	// A plain surface is its own capacity — dense rows, pitch == width. Only
	// the hardware FB (which pads) and resizable canvases (which reserve)
	// differ, and both say so explicitly.
	return surface_init_capacity(s, w, h, w, h);
}

int surface_init_capacity(surface_t *s, uint32_t w, uint32_t h,
                          uint32_t cap_w, uint32_t cap_h)
{
	// A capacity smaller than the size asked for would hand back a surface
	// whose own bounds walk off its buffer — refuse rather than round, since
	// every caller computes both numbers and one of them is simply wrong.
	if (cap_w < w || cap_h < h) {
		printd(DEBUG_GUI, "surface_init_capacity: %ux%u does not fit capacity %ux%u\n",
			w, h, cap_w, cap_h);
		s->pixels = NULL;
		s->width = s->height = s->pitch_px = 0;
		return -1;
	}

	s->pixels = kmalloc_aligned((uint64_t)cap_w * cap_h * sizeof(uint32_t));
	if (!s->pixels) {
		printd(DEBUG_GUI, "surface_init: allocation failed for %ux%u surface (capacity %ux%u)\n",
			w, h, cap_w, cap_h);
		s->width = s->height = s->pitch_px = 0;
		return -1;
	}
	memset(s->pixels, 0, (uint64_t)cap_w * cap_h * sizeof(uint32_t));
	s->width = w;
	s->height = h;
	s->pitch_px = cap_w;   // the RESERVATION's width — see the header
	return 0;
}

bool surface_set_size(surface_t *s, uint32_t w, uint32_t h,
                      uint32_t cap_w, uint32_t cap_h)
{
	// The capacity is not recorded in surface_t (it would be a fourth number
	// that could disagree with the other three), so the owner passes it back
	// in and we check it here. pitch_px IS the capacity width, so that half
	// is self-checking; the height half is the caller's word, and the caller
	// is the window that reserved it.
	if (w > cap_w || h > cap_h || s->pitch_px != cap_w)
		return false;
	s->width = w;
	s->height = h;
	return true;
}

void surface_free(surface_t *s)
{
	if (s->pixels)
		kfree(s->pixels);
	s->pixels = NULL;
	s->width = s->height = s->pitch_px = 0;
}

void surface_fill_rect(surface_t *dst, rect_t r, uint32_t color)
{
	rect_t c;
	if (!rect_intersect(r, surface_bounds(dst), &c))
		return;

	for (int32_t y = c.y; y < c.y + c.h; y++) {
		uint32_t *row = surface_row(dst, y) + c.x;
		for (int32_t x = 0; x < c.w; x++)
			row[x] = color;
	}
}

void surface_blit(surface_t *dst, int32_t dx, int32_t dy,
                  const surface_t *src, rect_t src_rect)
{
	// Clip the source rect to the source surface first...
	rect_t sc;
	if (!rect_intersect(src_rect, surface_bounds((surface_t *)src), &sc))
		return;
	// (dx,dy) tracks any clipping the source rect just took, so the copied
	// pixels stay registered with where the caller aimed them.
	dx += sc.x - src_rect.x;
	dy += sc.y - src_rect.y;

	// ...then clip the destination placement to the destination surface.
	rect_t dc;
	if (!rect_intersect((rect_t){dx, dy, sc.w, sc.h}, surface_bounds(dst), &dc))
		return;
	// And mirror THAT clip back into the source origin.
	sc.x += dc.x - dx;
	sc.y += dc.y - dy;

	for (int32_t y = 0; y < dc.h; y++) {
		uint32_t *d = surface_row(dst, dc.y + y) + dc.x;
		const uint32_t *s = surface_row((surface_t *)src, sc.y + y) + sc.x;
		for (int32_t x = 0; x < dc.w; x++)
			d[x] = s[x];
	}
}

void surface_blit_masked(surface_t *dst, int32_t dx, int32_t dy,
                         const uint32_t *pixels, const uint8_t *mask,
                         uint32_t w, uint32_t h)
{
	rect_t dc;
	if (!rect_intersect((rect_t){dx, dy, (int32_t)w, (int32_t)h},
	                    surface_bounds(dst), &dc))
		return;
	// Where in the source art the clipped region starts.
	int32_t sx = dc.x - dx;
	int32_t sy = dc.y - dy;

	for (int32_t y = 0; y < dc.h; y++) {
		uint32_t *d = surface_row(dst, dc.y + y) + dc.x;
		const uint32_t *s = pixels + (size_t)(sy + y) * w + sx;
		const uint8_t *m = mask + (size_t)(sy + y) * w + sx;
		for (int32_t x = 0; x < dc.w; x++)
			if (m[x])
				d[x] = s[x];
	}
}

void surface_draw_hline(surface_t *dst, int32_t x, int32_t y, int32_t len, uint32_t color)
{
	surface_fill_rect(dst, (rect_t){x, y, len, 1}, color);
}

void surface_draw_vline(surface_t *dst, int32_t x, int32_t y, int32_t len, uint32_t color)
{
	surface_fill_rect(dst, (rect_t){x, y, 1, len}, color);
}

void surface_draw_rect(surface_t *dst, rect_t r, uint32_t color)
{
	surface_draw_hline(dst, r.x, r.y, r.w, color);
	surface_draw_hline(dst, r.x, r.y + r.h - 1, r.w, color);
	surface_draw_vline(dst, r.x, r.y, r.h, color);
	surface_draw_vline(dst, r.x + r.w - 1, r.y, r.h, color);
}

void surface_draw_text(surface_t *dst, int32_t x, int32_t y,
                       const char *str, size_t len,
                       uint32_t fg, uint32_t bg)
{
	// Reuse the PSF1 console font Limine loaded for us at boot (8-wide
	// glyphs, charsize tall — 16 for the shipped font). No layout logic
	// here beyond advancing the pen: no wrapping, no control characters.
	// Higher layers (console window, titlebars) own text flow.
	struct PSF1_FONT *font = kRenderer.psf1_font;
	if (!font)
		return;
	int32_t glyph_h = font->psf1_header->charsize;

	for (size_t i = 0; i < len; i++, x += 8) {
		// Paint the opaque cell (bg) and set fg bits — clipped per pixel via
		// fill/blit-free direct writes guarded by an intersect on the cell.
		rect_t cell;
		if (!rect_intersect((rect_t){x, y, 8, glyph_h}, surface_bounds(dst), &cell))
			continue;

		const uint8_t *glyph = (const uint8_t *)font->glyph_buffer
		                       + (uint8_t)str[i] * glyph_h;
		for (int32_t cy = cell.y; cy < cell.y + cell.h; cy++) {
			uint8_t bits = glyph[cy - y];
			uint32_t *row = surface_row(dst, cy);
			for (int32_t cx = cell.x; cx < cell.x + cell.w; cx++)
				row[cx] = (bits & (0x80 >> (cx - x))) ? fg : bg;
		}
	}
}

void surface_flush_rect(const surface_t *back, rect_t r)
{
	// Clip to both the backbuffer and the hardware framebuffer dimensions
	// (they should be identical, but never trust that with pointer math).
	rect_t fb_bounds = {0, 0, (int32_t)kFrameBuffer.width, (int32_t)kFrameBuffer.height};
	rect_t c;
	if (!rect_intersect(r, surface_bounds((surface_t *)back), &c))
		return;
	if (!rect_intersect(c, fb_bounds, &c))
		return;

	// Row-by-row copy, widened to 64-bit stores. The framebuffer is mapped
	// uncached (PCD), so each store goes straight to the bus — halving the
	// store count roughly halves the flush cost. Alignment is chosen off the
	// DESTINATION: a leading/trailing 32-bit store when the dst row start/end
	// isn't 8-byte aligned, 64-bit stores for the body.
	for (int32_t y = c.y; y < c.y + c.h; y++) {
		const uint32_t *src = back->pixels + (size_t)y * back->pitch_px + c.x;
		uint32_t *dst = (uint32_t *)kFrameBuffer.base_address
		                + (size_t)y * kFrameBuffer.pixels_per_scan_line + c.x;
		int32_t n = c.w;

		if (((uintptr_t)dst & 7) && n) {  // unaligned head
			*dst++ = *src++;
			n--;
		}
		while (n >= 2) {                  // 64-bit body
			// src may be 4-aligned only; read it as two 32-bit halves to
			// keep the write (the expensive UC side) the wide one.
			uint64_t v = (uint64_t)src[0] | ((uint64_t)src[1] << 32);
			*(uint64_t *)dst = v;
			src += 2;
			dst += 2;
			n -= 2;
		}
		if (n)                            // unaligned tail
			*dst = *src;
	}
}
