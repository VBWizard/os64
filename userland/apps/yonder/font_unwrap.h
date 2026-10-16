#ifndef YONDER_FONT_UNWRAP_H
#define YONDER_FONT_UNWRAP_H

// A web font's container, told by its signature and taken off on the worker
// that fetched it, so the window's thread opens a plain SFNT (PILE3.md § Web
// fonts, Where the bytes become a face). Pure: bytes in, bytes out.

#include <stddef.h>
#include <stdint.h>

typedef enum {
    YONDER_FONT_SFNT,           // TrueType (0x00010000, `true`) or OpenType (`OTTO`)
    YONDER_FONT_WOFF,           // `wOFF`
    YONDER_FONT_WOFF2,          // `wOF2`
    YONDER_FONT_UNKNOWN,        // anything else, a collection (`ttcf`) among it
} yonder_font_kind_t;

yonder_font_kind_t yonder_font_sniff(const uint8_t *bytes, size_t len);

typedef enum {
    YONDER_FONT_OK,
    YONDER_FONT_MALFORMED,
    YONDER_FONT_LIMIT,          // past YONDER_FONT_SFNT_MAX, or 64 to 1
    YONDER_FONT_NO_MEMORY,
} yonder_font_status_t;

// The most an unwrapped SFNT may be — the most a text context opens
// (OS64_FONT_FILE_MAX; webfont.c asserts it), so no unwrap makes a font
// that cannot be opened — and the most it may be per byte of its
// container (FreeType's sfwoff.c, after its 656cb77).
#define YONDER_FONT_SFNT_MAX ((size_t)32 << 20)
#define YONDER_FONT_EXPANSION_MAX 64

// A WOFF 1.0 container's SFNT: the header read — a plain font's flavor,
// never a collection's or another container's, at most 0xFFF tables,
// neither file its directory alone, no block length without an offset,
// no metadata unpacking to nothing — and the directory: tags ascending
// from above zero, none packed longer than it unpacks,
// and the file nothing but its parts, each table where the last one's
// padding ended, the metadata and private blocks after them, the file
// ending where the last does (sfwoff.c's rules) — each table taken as
// stored or inflated
// through libgzip's zlib door to exactly its length, and laid out again
// under an SFNT directory — the data in the order it lay in the container,
// the directory in its tags' — each on a 4-byte boundary, zero-padded. The
// header's `totalSfntSize` must be what that layout makes. `*sfnt` is
// os64_malloc'd, the caller's to free; NULL on any refusal.
yonder_font_status_t yonder_font_unwrap_woff(const uint8_t *woff, size_t len, uint8_t **sfnt,
                                             size_t *sfnt_len);

const char *yonder_font_status_name(yonder_font_status_t status);

#endif // YONDER_FONT_UNWRAP_H
