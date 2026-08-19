#ifndef GUI_SURFACE_H
#define GUI_SURFACE_H

#include <stddef.h>
#include "gui/gui_types.h"

// Layer 0/1 of the GUI: pixel surfaces and software rendering primitives.
//
// Every function here draws into ordinary cacheable RAM and clips against the
// destination surface — callers may pass rects that hang off any edge. Only
// surface_flush_rect() ever touches the real (uncached) framebuffer, and it
// is write-only: we NEVER read from the framebuffer (UC reads are brutally
// slow and there is never a reason to — the backbuffer is the truth).

// Allocate a w×h RAM surface (pixels zeroed). Returns 0 on success, -1 on
// allocation failure. Failure must be handled by the caller — never panic
// over a window that couldn't be created.
int surface_init(surface_t *s, uint32_t w, uint32_t h);
void surface_free(surface_t *s);

// Fill r (clipped) with a solid color.
void surface_fill_rect(surface_t *dst, rect_t r, uint32_t color);

// Copy src_rect from src onto dst at (dx,dy), clipped to both surfaces.
void surface_blit(surface_t *dst, int32_t dx, int32_t dy,
                  const surface_t *src, rect_t src_rect);

// Copy a w×h pixel array onto dst at (dx,dy), skipping pixels whose mask byte
// is 0. This is how shaped art (the mouse cursor arrow) gets drawn without
// needing per-pixel alpha support.
void surface_blit_masked(surface_t *dst, int32_t dx, int32_t dy,
                         const uint32_t *pixels, const uint8_t *mask,
                         uint32_t w, uint32_t h);

// 1-pixel-thick lines and outlines.
void surface_draw_hline(surface_t *dst, int32_t x, int32_t y, int32_t len, uint32_t color);
void surface_draw_vline(surface_t *dst, int32_t x, int32_t y, int32_t len, uint32_t color);
void surface_draw_rect(surface_t *dst, rect_t r, uint32_t color);

// Render text with the boot PSF1 console font (8×16 cells, opaque: fg on bg).
// (x,y) is the top-left of the first glyph cell.
void surface_draw_text(surface_t *dst, int32_t x, int32_t y,
                       const char *str, size_t len,
                       uint32_t fg, uint32_t bg);

// Copy r (screen coordinates, clipped) from the backbuffer surface to the
// hardware framebuffer. The single point where pixels leave RAM.
void surface_flush_rect(const surface_t *back, rect_t r);

// A VIEW onto part of a surface: same pixels, same pitch, origin moved to the
// rect's top-left and bounds shrunk to the rect. Free — no allocation, no copy,
// just arithmetic — and it converts a rule everyone must REMEMBER into one the
// code cannot break: since every primitive above clips against its
// DESTINATION's bounds, drawing into a view physically cannot write outside
// the rect. Translate your coordinates into view space and stop thinking about
// clipping. (`r` must already lie within `s` — the compositor clips damage to
// the screen before it gets here.)
//
// This is what makes multi-rect damage safe: a frame composites several rects
// before flushing any of them, so a paint that strays outside the rect it was
// asked for would corrupt a sibling rect that is already correct. See the
// contract note on composite_one in window.c for the bug that taught us.
static inline surface_t surface_view(surface_t *s, rect_t r)
{
    return (surface_t){
        .pixels   = s->pixels + (size_t)r.y * s->pitch_px + r.x,
        .width    = (uint32_t)r.w,
        .height   = (uint32_t)r.h,
        .pitch_px = s->pitch_px,
    };
}

#endif // GUI_SURFACE_H
