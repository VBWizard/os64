#ifndef OS64_TEXT_H
#define OS64_TEXT_H

/* Text-run declarations for F2; implementation status is recorded in FONTS_WORK_PLAN.md.
 * FONT_CONTRACTS.md defines positioning, encoding and failure semantics. */
#include <stddef.h>
#include <stdint.h>
#include "os64/font_backend.h"

#define OS64_TEXT_BYTES_MAX (1024u * 1024u)
#define OS64_TEXT_GLYPHS_MAX (1024u * 1024u)
#define OS64_TEXT_FALLBACK_MAX 8u
#define OS64_TEXT_CACHE_DEFAULT (8u * 1024u * 1024u)
#define OS64_TEXT_MEMORY_DEFAULT (128u * 1024u * 1024u)

typedef struct os64_text_context os64_text_context_t;
typedef struct os64_text_font os64_text_font_t;
typedef struct os64_text_run os64_text_run_t;

typedef struct {
    os64_font_memory_t memory;
    const os64_font_backend_t *backend;
    size_t memory_cap; /* total context budget, including owned font bytes */
    size_t cache_cap; /* eviction target within memory_cap; zero = default */
} os64_text_options_t;

/* Context operations are serialized by the caller. Destroy is BUSY while any
 * externally retained font/run exists. Successful open copies input bytes;
 * the caller may then free its buffer. Each new open gets a unique identity
 * within the context, independent of pathname and pointer reuse. */
os64_font_status_t os64_text_create(const os64_text_options_t *, os64_text_context_t **out);
os64_font_status_t os64_text_destroy(os64_text_context_t *);
os64_font_status_t os64_text_font_open(os64_text_context_t *, const uint8_t *,
    size_t length, const os64_font_face_options_t *, os64_text_font_t **out);
os64_font_status_t os64_text_font_bitmap(os64_text_context_t *, os64_text_font_t **out);
void os64_text_font_release(os64_text_font_t *);

typedef enum {
    OS64_TEXT_UTF8_WESTERN_V1 = 0,
    OS64_TEXT_LATIN1 = 1,
    OS64_TEXT_CP437 = 2
} os64_text_encoding_t;

typedef struct {
    os64_text_encoding_t encoding;
    os64_text_font_t *const *fonts; /* primary then fallback, 1..8 */
    size_t font_count;
    os64_font_pos_t tab_origin, tab_interval; /* interval > 0 */
    /* Positive cell advance selects grid mode: one decoded input unit per
     * cell, no pair kerning. Grid mode accepts LATIN1 or CP437, not UTF-8. */
    os64_font_pos_t cell_advance;
} os64_text_layout_t;

typedef struct {
    size_t byte_begin, byte_end; /* original input, half-open cluster span */
    uint64_t font_identity; /* zero = synthetic missing/control marker */
    uint32_t glyph_index;
    os64_font_pos_t x, y; /* baseline origin in run coordinates */
    os64_font_pos_t advance_x;
    os64_font_rect_t ink; /* translated to run coordinates */
} os64_text_placement_t;

typedef struct {
    size_t byte_offset;
    os64_font_pos_t x;
} os64_text_caret_t;

typedef struct {
    const uint8_t *bytes;
    size_t byte_count;
    const os64_text_placement_t *glyphs;
    size_t glyph_count;
    const os64_text_caret_t *carets;
    size_t caret_count;
    os64_font_pos_t advance_x, ascent, descent, line_height;
    os64_font_rect_t ink;
} os64_text_run_view_t;

/* One logical line; literal LF is BAD_ARGUMENT. NUL is input data, not an end
 * marker. Successful layout owns a byte copy and retains fonts/glyph images.
 * Failed layout returns no partial run and preserves existing runs/fonts.
 * View pointers remain valid until run_release. Runs are immutable. */
os64_font_status_t os64_text_layout(os64_text_context_t *, const uint8_t *,
    size_t length, const os64_text_layout_t *, os64_text_run_t **out);
os64_font_status_t os64_text_run_view(const os64_text_run_t *, os64_text_run_view_t *out);
void os64_text_run_release(os64_text_run_t *);

typedef enum {
    OS64_TEXT_BEFORE = 0,
    OS64_TEXT_AFTER = 1
} os64_text_bias_t;

/* Interior byte offsets snap by bias; offsets past the end are BAD_ARGUMENT.
 * Hit chooses the nearest caret, ties toward the later byte offset. */
os64_font_status_t os64_text_caret(const os64_text_run_t *, size_t byte_offset,
    os64_text_bias_t, os64_text_caret_t *out);
os64_font_status_t os64_text_hit(const os64_text_run_t *, os64_font_pos_t x,
    os64_text_caret_t *out);
/* Largest source boundary whose logical caret fits; ink overhang is separate.
 * Width < 0 is BAD_ARGUMENT; zero is a valid fit of the empty prefix. */
os64_font_status_t os64_text_fit(const os64_text_run_t *, os64_font_pos_t width,
    size_t *byte_end);
/* Selection endpoints must be legal carets; an empty selection has empty ink.
 * The result spans logical X and [-ascent, line_height-ascent) vertically. */
os64_font_status_t os64_text_selection(const os64_text_run_t *, size_t begin,
    size_t end, os64_font_rect_t *out);

/* Paint is declared separately so this layout header has no GUI dependency. */
#endif
