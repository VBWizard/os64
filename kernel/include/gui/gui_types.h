#ifndef GUI_TYPES_H
#define GUI_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// Shared GUI value types. Deliberately dependency-free (stdint only) so any
// layer — and eventually userland — can include this without dragging in
// kernel headers.

// A screen-space or surface-space rectangle. Signed x/y/w/h on purpose:
// window frames can hang partially off-screen (negative origin) during a
// drag, and clipping math is much cleaner when intermediate results are
// allowed to go negative instead of wrapping.
typedef struct rect
{
    int32_t x, y;
    int32_t w, h;
} rect_t;

// A pixel canvas. Everything the GUI renders — the backbuffer, each window's
// content, icon/cursor art — is one of these. Format is fixed 32-bpp XRGB
// (matches what Limine hands us; the X byte is reserved for future alpha).
typedef struct surface
{
    uint32_t *pixels;   // XRGB8888; kmalloc_aligned-backed for RAM surfaces
    uint32_t width;     // in pixels
    uint32_t height;    // in pixels
    uint32_t pitch_px;  // pixels per row: == width for RAM surfaces, but the
                        // hardware framebuffer may pad rows (pitch != width)
} surface_t;

static inline bool rect_is_empty(rect_t r)
{
    return r.w <= 0 || r.h <= 0;
}

// Smallest rectangle containing both a and b. An empty operand is treated as
// "no area" and the other operand wins — that makes union usable as a damage
// accumulator starting from an empty rect.
static inline rect_t rect_union(rect_t a, rect_t b)
{
    if (rect_is_empty(a))
        return b;
    if (rect_is_empty(b))
        return a;

    int32_t x1 = a.x < b.x ? a.x : b.x;
    int32_t y1 = a.y < b.y ? a.y : b.y;
    int32_t x2 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return (rect_t){x1, y1, x2 - x1, y2 - y1};
}

// Intersection of a and b into *out. Returns false (and leaves *out empty)
// when they don't overlap. This is THE clipping primitive: every draw call
// clips its target rect against the destination surface with this.
static inline bool rect_intersect(rect_t a, rect_t b, rect_t *out)
{
    int32_t x1 = a.x > b.x ? a.x : b.x;
    int32_t y1 = a.y > b.y ? a.y : b.y;
    int32_t x2 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    int32_t y2 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    out->x = x1;
    out->y = y1;
    out->w = x2 - x1;
    out->h = y2 - y1;
    return !rect_is_empty(*out);
}

static inline bool rect_contains_point(rect_t r, int32_t px, int32_t py)
{
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

// GUI color constants, 32-bpp XRGB. (The old BasicRenderer color macros have
// trailing commas baked in and can't be used in expressions — these replace
// them for GUI code.)
#define GUI_COLOR_BLACK      0xff000000
#define GUI_COLOR_WHITE      0xffffffff
#define GUI_COLOR_GRAY       0xff808080
#define GUI_COLOR_LIGHT_GRAY 0xffc0c0c0
#define GUI_COLOR_DARK_GRAY  0xff404040
#define GUI_COLOR_RED        0xffcc3333
#define GUI_COLOR_GREEN      0xff33aa55
#define GUI_COLOR_BLUE       0xff3355cc
#define GUI_COLOR_YELLOW     0xffe0c040
#define GUI_COLOR_DESKTOP    0xff2a5566  // muted teal desktop background

#endif // GUI_TYPES_H
