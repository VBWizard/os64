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
#define OS64_TEXT_RANGES_MAX 4096u

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
 * externally retained font/run exists. Successful open keeps a copy of the
 * input bytes — one per distinct file in the context, shared by every face
 * opened from the same bytes — so the caller may then free its buffer. Each new open gets a unique identity
 * within the context, independent of pathname and pointer reuse. */
os64_font_status_t os64_text_create(const os64_text_options_t *, os64_text_context_t **out);
os64_font_status_t os64_text_destroy(os64_text_context_t *);
os64_font_status_t os64_text_font_open(os64_text_context_t *, const uint8_t *,
    size_t length, const os64_font_face_options_t *, os64_text_font_t **out);
/* An inclusive span of Unicode scalars, first <= last <= 0x10FFFF. */
typedef struct {
    uint32_t first, last;
} os64_text_range_t;
/* Open as above, the font answering only for the scalars in `ranges` (a
 * CSS `unicode-range`): a layout passes any other on to the next font, as
 * if this one had no glyph for it. The ranges are copied, in any order,
 * overlapping or not; count 0 (ranges may then be NULL) covers every
 * scalar. More than OS64_TEXT_RANGES_MAX is LIMIT, whatever they hold. */
os64_font_status_t os64_text_font_open_ranged(os64_text_context_t *, const uint8_t *,
    size_t length, const os64_font_face_options_t *, const os64_text_range_t *ranges,
    size_t range_count, os64_text_font_t **out);
/* Another face of the file `like` was opened from — at another size, with
 * its own ranges — sharing the context's one copy of the bytes, so the
 * caller need not keep them. `like` must be an outline font of this
 * context; ranges are as for open_ranged. */
os64_font_status_t os64_text_font_open_like(const os64_text_font_t *like,
    const os64_font_face_options_t *, const os64_text_range_t *ranges, size_t range_count,
    os64_text_font_t **out);
os64_font_status_t os64_text_font_bitmap(os64_text_context_t *, os64_text_font_t **out);
void os64_text_font_release(os64_text_font_t *);
/* What the font's face reports: its names and metrics at its size. */
os64_font_status_t os64_text_font_info(const os64_text_font_t *, os64_font_face_info_t *out);

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
    /* Tracking (CSS Text 3 § 7.2): added after every cluster but a tab, the
     * run's last too, so runs laid side by side are spaced across their
     * join; trimming it where a line ends is the caller's. A negative one
     * narrows a cluster to no less than nothing: carets never go back.
     * Grid mode takes none (BAD_ARGUMENT). */
    os64_font_pos_t letter_spacing;
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
 * Successful layout returns one owned reference. Runs are immutable; view
 * pointers remain valid while an owned reference keeps the run alive. */
os64_font_status_t os64_text_layout(os64_text_context_t *, const uint8_t *,
    size_t length, const os64_text_layout_t *, os64_text_run_t **out);
os64_font_status_t os64_text_run_view(const os64_text_run_t *, os64_text_run_view_t *out);
/* Retain an additional owner without allocating. NULL is BAD_ARGUMENT;
 * reference-count overflow is LIMIT. Failure leaves ownership unchanged.
 * Release drops one owner (NULL is harmless); the last release frees the
 * run and its font/glyph-image holds. Like other context operations, these
 * calls require caller serialization and a live owned reference. */
os64_font_status_t os64_text_run_retain(os64_text_run_t *);
void os64_text_run_release(os64_text_run_t *);
/* Positioned run contours for vector consumers. Uses unhinted font curves;
 * builtin/missing glyphs become contours of their binary bitmap cells. The
 * caller's visitor owns copied output. No script/engine reentry is permitted.
 * The command budget spans the entire run. emit may be NULL for tight bounds.
 * A failure clears bounds; discard any commands already emitted. */
os64_font_status_t os64_text_run_outline(const os64_text_run_t *,size_t max_commands,
    os64_font_outline_emit_t emit,void *,os64_geometry_bounds_t *);


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
