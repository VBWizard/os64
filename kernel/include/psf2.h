#ifndef OS64_KERNEL_PSF2_H
#define OS64_KERNEL_PSF2_H

// psf2.h — read a PSF2 console font, and work out which glyph draws a byte.
//
// WHAT IT IS. Two pure functions over bytes somebody else supplied. No
// kernel headers, no allocation, no locks: `psf2_parse` answers "is this a
// font the console can draw with, and where are its parts?", and
// `psf2_build_charmap` answers "which glyph does this terminal byte mean?".
// Pure on purpose, for ansi.c's reason — the image is written by a ring-3
// program, so every field in it is a claim, and tools/test_psf2_host.sh
// feeds this file truncated, lying and random images under ASan on the host
// where a wrong read is a report and not a ring-0 fault. CONSOLE_FONTS.md is
// the design.
//
// THE FORMAT (Linux kbd, 1999), all fields little-endian:
//
//   magic          72 B5 4A 86
//   version        0
//   headersize     offset of the first glyph; 32 or more
//   flags          bit 0: a Unicode table follows the glyphs
//   numglyph
//   bytesperglyph  height * ((width + 7) / 8)
//   height, width  the cell, in pixels
//
// then numglyph bitmaps — rows top to bottom, each row padded to whole
// bytes, leftmost pixel in the first byte's top bit — and then, if flagged,
// the table: for each glyph in order, the UTF-8 of every code point it
// draws, then 0xFF. A 0xFE inside an entry starts SEQUENCES (a base plus
// combining marks); a console draws one code point per cell, so everything
// from a 0xFE to that entry's 0xFF is skipped.
//
// WHY A CHARMAP AT ALL. A terminal cell holds a BYTE and the character set
// it was written under (os64/charset.h); a font holds glyphs in whatever
// order its author liked, and says which code points each one draws. The
// boot face happens to keep Latin-1 in index order and nothing says the
// next one will, so BOTH sets go through the table: Latin-1 byte b is code
// point b,
// CP437 byte b is os64_cp437_codepoint(b), and the map holds the glyph that
// claims that code point — or PSF2_MAP_NONE, which draws blank or one of
// the synthesized blocks below, never a guess.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The fences. The cell bounds are BasicRenderer.h's CONSOLE_CELL_*_MAX
// restated (its cursor save-under buffer is sized by them; the renderer
// static-asserts the two agree). The byte bound is what one font may cost
// in kernel memory: a 32x64 face of 512 glyphs is 131 KB.
#define PSF2_CELL_W_MIN     4u
#define PSF2_CELL_W_MAX    64u
#define PSF2_CELL_H_MIN     6u
#define PSF2_CELL_H_MAX   128u
#define PSF2_GLYPHS_MIN   128u       // fewer cannot hold printable ASCII
#define PSF2_GLYPHS_MAX 65535u       // 0xFFFF is PSF2_MAP_NONE
#define PSF2_IMAGE_MAX  (1024u * 1024u)

typedef enum
{
    PSF2_OK = 0,
    PSF2_TOO_SHORT,        // not even a header
    PSF2_TOO_BIG,          // past PSF2_IMAGE_MAX
    PSF2_BAD_MAGIC,
    PSF2_BAD_VERSION,
    PSF2_BAD_HEADER_SIZE,  // under 32, or past the image
    PSF2_BAD_CELL,         // width or height outside the fences
    PSF2_BAD_GLYPH_BYTES,  // bytesperglyph is not height * row bytes
    PSF2_BAD_GLYPH_COUNT,
    PSF2_TRUNCATED,        // the glyphs the header promises are not all here
    PSF2_BAD_TABLE,        // the table ends inside an entry, or names too few
    PSF2_NO_ASCII,         // a printable ASCII character has no glyph
} psf2_status_t;

const char *psf2_status_name(psf2_status_t status);

// Pointers INTO the caller's image — nothing is copied, so the image must
// outlive the face.
typedef struct
{
    const uint8_t *glyphs;
    uint32_t nglyphs;
    uint32_t width, height;
    uint32_t row_bytes;        // (width + 7) / 8
    uint32_t glyph_bytes;      // height * row_bytes
    const uint8_t *table;      // NULL when the font carries none
    size_t table_bytes;
} psf2_face_t;

// Validate an image and describe it. `*out` is written only on PSF2_OK.
// `*offender`, when not NULL, receives the number that broke the rule (the
// width, the count, the size…) so the refusal can say more than its name.
psf2_status_t psf2_parse(const uint8_t *image, size_t bytes,
                         psf2_face_t *out, uint32_t *offender);

#define PSF2_MAP_NONE 0xFFFFu

typedef struct
{
    // [OS64_CHARSET_LATIN1] and [OS64_CHARSET_CP437]: glyph index per byte.
    uint16_t glyph[2][256];
    bool from_table;           // false: the font had none, both rows are identity
    uint32_t unmapped[2];      // printable bytes (0x20 up, not 0x7F) left NONE
} psf2_charmap_t;

// Build both rows from the face's Unicode table. The FIRST glyph to claim a
// code point keeps it, which is the order a font's author put them in. A
// face with no table gets the identity (byte b is glyph b): such a font is
// drawn in its own code page and nothing here can know which.
//
// Refuses PSF2_NO_ASCII when any of 0x20..0x7E has no glyph under Latin-1 —
// a console nobody can read a prompt on is not a font choice, it is an
// outage, and the refusal is cheaper than the reboot.
psf2_status_t psf2_build_charmap(const psf2_face_t *face, psf2_charmap_t *out,
                                 uint32_t *offender);

// The blocks a face may lack and ANSI art cannot be drawn without: the
// three shades, the full block and the four halves (U+2591..2593, U+2588,
// U+2580, U+2584, U+258C, U+2590). Pure geometry, so they are made to
// measure for any cell instead of being stored at one size. Writes
// height * row_bytes bytes to `out` and returns true, or returns false for
// a code point that is not one of these.
bool psf2_synth_block(uint32_t codepoint, uint32_t width, uint32_t height,
                      uint8_t *out);

#endif // OS64_KERNEL_PSF2_H
