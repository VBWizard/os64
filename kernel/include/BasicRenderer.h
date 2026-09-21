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

// THE FACE THE CONSOLE IS DRAWING WITH — everything the painter needs to
// know about a bitmap font, and nothing about the file it came from. A glyph
// is `height` rows of `row_bytes` bytes, MSB first, the leftmost pixel in
// the top bit of a row's first byte; PSF1 and PSF2 both lay glyphs down that
// way, so one blitter serves either. The cell size is a property of the
// face, which is why no constant names it: CONSOLE_FONTS.md is the design.
//
// The fences are what the cursor's save-under buffer and the blitter's
// arithmetic are sized for. A face outside them is refused where it is
// loaded, never clamped here.
#define CONSOLE_CELL_W_MAX 64u
#define CONSOLE_CELL_H_MAX 128u
typedef struct
{
    const uint8_t *glyphs;     // nglyphs * glyph_bytes
    uint32_t nglyphs;
    uint32_t width, height;    // the cell, in pixels
    uint32_t row_bytes;        // (width + 7) / 8
    uint32_t glyph_bytes;      // height * row_bytes
    // WHICH GLYPH DRAWS A BYTE, already answered: 2 x 256 bitmap pointers,
    // [charset * 256 + byte], each glyph_bytes long. A face that arrives at
    // runtime is resolved ONCE, when it is installed (console_font.c), so the
    // blitter indexes and never decides — a byte the face cannot draw points
    // at a blank or a synthesized block, never at nothing. NULL for the boot
    // face, which keeps Latin-1 in index order and is drawn through
    // os64/charset.h as it always was.
    const uint8_t *const *resolved;
} console_face_t;

typedef struct
{
    struct Point cursor_position;
    struct Framebuffer *framebuffer;
    struct PSF1_FONT *psf1_font;
    console_face_t face;

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

// (kConsoleSink was declared here 2026-07 → 2026-08-19: a function pointer
// that diverted every print_n byte into the ring-0 GUI console window. The
// VT8 chapter retired both — glass ownership is VT focus now, asked via
// gui_owns_glass(), and the single-store-from-any-context panic property the
// pointer provided lives on in gui_emergency_disable's seated flag.)

// The live cell, for code that turns pixels into cells without a renderer
// in hand (the text-console mouse). Functions, not constants, because the
// cell belongs to the face and is known only once one is installed.
uint32_t renderer_cell_w(void);
uint32_t renderer_cell_h(void);

// Draw with this face from now on; NULL puts the boot face back. Takes the
// renderer lock itself, and deals with the text cursor FIRST: its save-under
// pixels are in the outgoing cell's geometry, and restoring them through the
// incoming one would paint them somewhere else. After a panic has taken the
// glass it installs the boot face whatever it was asked for. The face's
// memory is the caller's — it must stay valid until another install has
// returned, and may be freed the moment one has, because every read of a
// glyph happens under the lock this takes.
//
// It changes the PAINT and nothing else. The terminals' grids are re-shaped
// by the caller (console_font.c), and until the focused one is repainted the
// glass shows old cells at new positions; every glyph write clips to the
// framebuffer, so that interval is ugly and not dangerous.
void renderer_face_install(const console_face_t *face);
// The face Limine handed the kernel: what NULL above installs, and what a
// panic draws with whatever has been installed since.
const console_face_t *renderer_boot_face(void);

void init_renderer(BasicRenderer *basicrenderer, struct Framebuffer *framebuffer, struct PSF1_FONT *psf1_font);
void moveto(BasicRenderer *basicrenderer, unsigned int x, unsigned int y);
void get_cursor_pos(BasicRenderer *basicrenderer, unsigned int *x, unsigned int *y);
void print_at(BasicRenderer *basicrenderer, unsigned int x, unsigned int y, const char *str);
void print(const char *str);
void print_n(const char* str, size_t length);
// The legacy direct-to-framebuffer interpreter — print_n's old body. Two
// customers remain: early boot (before tty_init builds the grids) and the
// panic path (kTTYDirect — a dead system paints straight onto whatever
// terminal is showing). Everything else goes print_n -> tty_write, where the
// SAME control-byte semantics now live against a per-terminal grid.
void print_n_direct(const char* str, size_t length);
int printf(const char *fmt, ...);

// ── The dumb-glass API (2026-08-08, the virtual-terminal slice) ─────────────
// The renderer's demotion papers: tty.c owns the terminal LOGIC (cursor,
// wrap, scroll, control bytes — against each tty's character grid), and the
// renderer keeps only the PAINT. tty.c brackets its glass work in
// begin/end (one atomic paint per tty_write, exactly the atomicity print_n's
// whole-string lock always provided) and uses the _locked primitives between.
uint32_t renderer_cols(void);   // glass geometry, in character cells
uint32_t renderer_rows(void);
// Acquire the renderer lock (irqsave) and hide the text cursor — the tty
// layer's ticket to paint. Returns the saved flags for _end.
uint64_t renderer_glass_begin(void);
// Park the console cursor at a character cell (keeps the underscore cursor
// and print_at-era snapshots honest), optionally relight it, release.
void renderer_glass_end(uint64_t flags, uint32_t row, uint32_t col, bool show_cursor);
// The primitives (caller holds the lock via renderer_glass_begin):
void renderer_glass_putc_locked(char ch, uint32_t row, uint32_t col, uint32_t color);
// The same, with the cell's BACKGROUND named too — for an overlay that has to
// paint one cell differently from the console's one background color.
// Inverse video (swap the two) is the text-console selection's highlight and
// its mouse pointer; see vt_select.c. Leaves kRenderer.color untouched.
// `charset` is the TERMINAL's, not the renderer's: it decides which bitmap a
// byte over 0x7F draws as (os64/charset.h). Every caller that has a tty in
// hand passes that tty's; the kernel's own printing passes Latin-1.
void renderer_glass_putc_bg_locked(char ch, uint8_t charset,
                                   uint32_t row, uint32_t col,
                                   uint32_t fg, uint32_t bg);
extern uint32_t kFrameBufferBackgroundColor;   // the console's one background
void renderer_glass_scroll_locked(void);   // one text line up (throttled blit)
void renderer_glass_clear_locked(void);    // wipe to background

// The colour the glass shows where no cell covers it — the margin beyond the
// last row and column, and what a wipe leaves behind. Called under the
// renderer lock during a full tty repaint; also paints the remainder strips.
void renderer_glass_background_locked(uint32_t color);
// The repaint gait (focus switch / scrollback view): defer marks the glass
// dirty so every putc lands in the shadow only; blit pushes the finished
// frame in ONE memcpy. A terminal switch is a single blit, not 16k pokes.
void renderer_glass_defer_locked(void);
void renderer_glass_blit_locked(void);
void put_char(BasicRenderer *basicrenderer, unsigned char chr, unsigned int xOff, unsigned int yOff);
void clear(BasicRenderer *basicrenderer, uint32_t color, bool resetCursor);
void renderer_bust_lock(void);
// Blit-throttle flush rider (processSignals): if a scroll burst left the
// glass behind the shadow and the ~30Hz window has passed, push the frame.
// Cheap no-op when the glass is current. See the doctrine in BasicRenderer.c.
void renderer_flush_if_dirty(void);

// The text cursor (2026-08-08): a solid underscore at the console cursor
// cell. console_read shows it before parking (glows while the machine
// listens) and hides it on every exit; every print path hides it first
// thing, so output never lands on a painted cursor. Save/restore of the
// covered pixels lives in the renderer — callers never care what's under it.
void renderer_cursor_show(void);
void renderer_cursor_hide(void);

#endif
