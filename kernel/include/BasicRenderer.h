#ifndef BASICRENDERER_H
#define BASICRENDERER_H
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "limine.h"
#include "video.h"

struct Point
{
    unsigned int x;
    unsigned int y;
};

#define WHITE 0xffffffff,
#define SILVER 0xffc0c0c0,
#define GRAY 0xff808080,
#define BGRAY 0xffC0C0C0,
#define DGRAY 0xff404040,
#define BLACK 0xff000000,
#define PINK 0xffFF1493,
#define GREEN 0xff008000,
#define RED 0xff800000,
#define PURPLE 0xff800080,
#define ORANGE 0xffFF4500,
#define CYAN 0xff008080,
#define YELLOW 0xffFFD700,
#define BROWN 0xffA52A2A,
#define BLUE 0xff000080,
#define DBLUE 0xff000030,
#define BRED 0xffFF0000,
#define BBLUE 0xff0000FF,
#define BGREEN 0xff00FF00,
#define TBLACK 0x00000000;

typedef struct
{
    struct Point cursor_position;
    struct Framebuffer *framebuffer;
    struct PSF1_FONT *psf1_font;

    unsigned int color;
    bool overwrite;
    // RAM mirror of the framebuffer (same pixels_per_scan_line stride).
    // NULL until renderer_attach_shadow() — the rule it exists to enforce:
    // NEVER READ VRAM. Scrolling used to memmove the framebuffer itself,
    // which is fine when the "framebuffer" is QEMU's RAM and ~2 lines per
    // second when it is real write-combined VRAM across PCIe (the Bosgame
    // P5's first scroll, 2026-07-19, was a sight). All reads hit this
    // mirror; VRAM only ever receives writes.
    unsigned int *shadow;
} BasicRenderer;

// Allocate the shadow and copy the live framebuffer into it — the one VRAM
// read the console will ever perform. Call once kmalloc is up; every print
// before that scrolls the slow honest way (early boot never fills a screen).
void renderer_attach_shadow(void);

extern BasicRenderer kRenderer;

// When non-NULL, print_n() diverts all console bytes to the GUI console
// window (kernel/src/gui/console_window.c) instead of drawing directly on
// the framebuffer. ONE seam covers everything: printf/print/panic and the
// WRITE syscall all funnel through print_n. A plain volatile pointer, not a
// lock — panic() must be able to kill the diversion with a single store
// from ANY context (gui_emergency_disable), after which text falls back to
// the direct-to-framebuffer path and scribbles over the desktop (which is
// exactly what you want from a dead system).
typedef void (*console_sink_fn)(const char *bytes, size_t length);
extern volatile console_sink_fn kConsoleSink;

// Glyph cell size for the built-in PSF1 console font. These were bare 8s and
// 16s scattered through the renderer; naming them means a font change breaks
// in one place instead of five. (FONT_HEIGHT still shadows the font's own
// charsize field — where a BasicRenderer is in hand, prefer
// psf1_font->psf1_header->charsize, which is the authority.)
#define FONT_WIDTH  8
#define FONT_HEIGHT 16

void init_renderer(BasicRenderer *basicrenderer, struct Framebuffer *framebuffer, struct PSF1_FONT *psf1_font);
void moveto(BasicRenderer *basicrenderer, unsigned int x, unsigned int y);
void get_cursor_pos(BasicRenderer *basicrenderer, unsigned int *x, unsigned int *y);
void print_at(BasicRenderer *basicrenderer, unsigned int x, unsigned int y, const char *str);
void print(const char *str);
void print_n(const char* str, size_t length);
int printf(const char *fmt, ...);
void put_char(BasicRenderer *basicrenderer, char chr, unsigned int xOff, unsigned int yOff);
void clear(BasicRenderer *basicrenderer, uint32_t color, bool resetCursor);
void renderer_bust_lock(void);

#endif