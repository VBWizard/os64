#ifndef OS64_FONT_PSF2_H
#define OS64_FONT_PSF2_H

// An outline face, rendered into the one format the kernel console takes.
//
// The virtual terminals draw from a PSF2 BITMAP and nothing smarter: the
// kernel is built without SSE and keeps no rasterizer (CONSOLE_FONTS.md). So
// "DejaVu Sans Mono at 24 pixels on VT1" is ring 3's job, and this is the
// whole of it: bytes of a TrueType/OpenType face and a size in, bytes of a
// PSF2 image out, ready to be written to /sys/console/font. Touches no file
// and no terminal — the caller read the face and the caller does the write.
//
// WHAT THE IMAGE HOLDS is what a terminal can draw, which is two sets of 256
// (os64/charset.h): glyphs 0..255 are Latin-1 in index order, and the CP437
// code points Latin-1 lacks — the shades, the blocks, the box-drawing set —
// follow them. A Unicode table names every glyph, because the kernel maps
// both sets through the table and trusts no index order.
//
// A CODE POINT THE FACE LACKS — or cannot draw at this size, or that the
// kernel draws better — gets a blank glyph and NO table entry, which the
// kernel reads as "this face has no such character": it makes the block
// elements itself, to measure, and draws the rest blank. Leaving it out is
// what lets it do that — a blank glyph that CLAIMED to be the dark shade
// would be drawn, and ANSI art would have holes where it should have fills.
// "Cannot draw at this size" is a glyph the one-bit threshold ate whole (a
// hairline at 10 px), and a box-drawing glyph that does not reach an edge
// its name promises: blank reads as missing, a frame with gaps reads as
// corruption. Printable ASCII is the exception: a face without all of it,
// at this size, is refused, and `info.offender` says which character.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "os64/font_backend.h"

// The cell the kernel's loader accepts (kernel/include/psf2.h). A size that
// renders outside it is refused here, with the numbers, rather than by the
// kernel after the write.
#define OS64_FONT_PSF2_CELL_W_MIN   4u
#define OS64_FONT_PSF2_CELL_W_MAX  64u
#define OS64_FONT_PSF2_CELL_H_MIN   6u
#define OS64_FONT_PSF2_CELL_H_MAX 128u

// No image this makes is larger: 256 Latin-1 glyphs plus at most one per
// CP437 high byte, each at the largest cell, each with a table entry of at
// most three UTF-8 bytes and a terminator. A caller that allocates this
// never meets OS64_FONT_LIMIT for want of room.
#define OS64_FONT_PSF2_GLYPHS_MAX (256u + 128u)
#define OS64_FONT_PSF2_IMAGE_MAX \
    (32u + OS64_FONT_PSF2_GLYPHS_MAX * (((OS64_FONT_PSF2_CELL_W_MAX + 7u) / 8u) * OS64_FONT_PSF2_CELL_H_MAX + 4u))

typedef struct {
    // NULL selects FreeType (os64_freetype_backend_v1). A test injects its own.
    const os64_font_backend_t *backend;
    // alloc NULL selects os64_malloc/os64_free. Everything taken through it
    // is given back before the call returns, on every path.
    os64_font_memory_t memory;
    // The nominal em size, as everywhere in the font stack. The CELL comes
    // out of the face's own metrics at that size and is reported back:
    // DejaVu Sans Mono at 24 is a 14x29 cell.
    uint32_t pixel_height;
} os64_font_psf2_options_t;

typedef struct {
    // True once the engine took the bytes as a font. The engine answers
    // UNSUPPORTED for a file in no format it knows and this answers it for a
    // proportional face, and "that is not a font" and "that font cannot be a
    // terminal's" are different things to tell a person.
    bool opened;
    uint32_t cell_w, cell_h;
    uint32_t glyphs;    // in the image
    // Wanted code points left unnamed: the face has no glyph, or the one it
    // has vanishes at this size, or is a box-drawing glyph with a gap. The
    // blocks the kernel draws itself are not counted; nothing is missing.
    uint32_t missing;
    // Glyphs that lost ink to the cell's edge. A DAMAGE count — worth a
    // warning: DejaVu at 42 px loses half of Ñ's tilde. The box-drawing set
    // is not in it, because a face draws that set to overshoot its line on
    // purpose so that two of them meet, and cutting the overshoot at the
    // cell is what joins them.
    uint32_t clipped;
    // The printable ASCII character a refusal is about, once the face has
    // opened: under MISSING the one the face lacks or cannot draw at this
    // size, under UNSUPPORTED the one whose advance is not the cell's
    // (0 when the refusal names none).
    uint32_t offender;
} os64_font_psf2_info_t;

// Render `face` into `out`. On success *out_len is the image's length and
// *info (optional) says what was made. Refusals, and what each one means
// HERE:
//   BAD_ARGUMENT  a NULL that is required, a zero size, a backend table of
//                 the wrong revision
//   UNSUPPORTED   a proportional face, or one whose advance is not a whole
//                 number of pixels at this size — a terminal has one cell
//   MISSING       the face lacks some of printable ASCII, or a character of
//                 it vanishes at this size (`info.offender` names it)
//   LIMIT         the cell at this size is outside what the kernel accepts
//                 (info carries the cell), or `cap` is too small (*out_len
//                 carries a size that is enough for this face at this size)
// and whatever the backend answers for a face it cannot open. On any failure
// the contents of `out` are unspecified.
os64_font_status_t os64_font_render_psf2(const os64_font_psf2_options_t *options,
    const uint8_t *face, size_t face_len,
    uint8_t *out, size_t cap, size_t *out_len, os64_font_psf2_info_t *info);

#endif
