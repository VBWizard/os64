#ifndef OS64_FONT_BACKEND_H
#define OS64_FONT_BACKEND_H

/* Backend table v1; freeze status and complete semantics are recorded in
 * FONT_CONTRACTS.md. This standalone header has no libos64 link dependency. */
#include <stddef.h>
#include <stdint.h>

#define OS64_FONT_BACKEND_REVISION 1u
#define OS64_FONT_UNIT 64
#define OS64_FONT_FILE_MAX (32u * 1024u * 1024u)
#define OS64_FONT_MEMORY_DEFAULT (64u * 1024u * 1024u)
#define OS64_FONT_MEMORY_MAX (128u * 1024u * 1024u)
#define OS64_FONT_PIXEL_MAX 256u
#define OS64_FONT_MASK_DIM_MAX 4096u
#define OS64_FONT_FACE_MAX 64u
#define OS64_FONT_NAME_CAP 128u

/* Signed 26.6 pixels. X increases right; Y increases down. Rectangles are
 * half-open; an empty ink rectangle has all fields zero. */
typedef int32_t os64_font_pos_t;
typedef struct {
    os64_font_pos_t x0, y0, x1, y1;
} os64_font_rect_t;

typedef enum {
    OS64_FONT_OK = 0,
    OS64_FONT_BAD_ARGUMENT,
    OS64_FONT_UNSUPPORTED,
    OS64_FONT_MALFORMED,
    OS64_FONT_LIMIT,
    OS64_FONT_NO_MEMORY,
    OS64_FONT_MISSING,
    OS64_FONT_BUSY,
    OS64_FONT_ENGINE_ERROR
} os64_font_status_t;

/* alloc receives a positive size and returns max_align_t-aligned storage or
 * NULL. free receives a live allocation and its original requested size.
 * Callbacks/context outlive the engine, and may not reenter that engine. */
typedef struct {
    void *context;
    void *(*alloc)(void *context, size_t bytes);
    void (*free)(void *context, void *allocation, size_t bytes);
} os64_font_memory_t;

typedef struct {
    os64_font_memory_t memory;
    /* Zero selects the default; includes engine and owned glyphs. Cap refusal
     * is LIMIT at any operation; callback NULL is NO_MEMORY. */
    size_t memory_cap;
} os64_font_engine_options_t;

typedef enum {
    /* Backend's normal grayscale hinting. The selected FreeType build uses
     * the autofitter for TrueType and native Adobe hinting for CFF outlines. */
    OS64_FONT_HINT_NORMAL = 0,
    /* Disable both native hinting and autohinting. */
    OS64_FONT_HINT_NONE = 1
} os64_font_hint_t;

typedef struct {
    uint32_t pixel_height; /* 1..OS64_FONT_PIXEL_MAX, nominal em size */
    os64_font_hint_t hint;
} os64_font_face_options_t;

typedef struct os64_font_engine os64_font_engine_t;
typedef struct os64_font_face os64_font_face_t;
typedef struct os64_font_glyph os64_font_glyph_t;

#define OS64_FONT_FACE_FIXED_WIDTH 1u
#define OS64_FONT_FACE_NAME_TRUNCATED 2u
typedef struct {
    uint32_t glyph_count;
    uint32_t flags;
    /* UTF-8, NUL-terminated display labels, not font identities or paths.
     * Prefer typographic names, then the deterministic platform/language order
     * in FONT_CONTRACTS.md. Malformed Unicode uses U+FFFD; legacy-only entries
     * may use the engine's lossy ASCII fallback. Truncate at scalar boundaries
     * and set the flag above when capacity is exhausted. */
    char family[OS64_FONT_NAME_CAP];
    char style[OS64_FONT_NAME_CAP];
    /* Nonnegative distances; line_height >= ascent + descent. */
    os64_font_pos_t ascent, descent, line_height;
} os64_font_face_info_t;

typedef struct {
    os64_font_pos_t advance_x;
    os64_font_rect_t ink; /* raster rectangle relative to baseline origin */
    uint32_t width, height, stride; /* stride == width; top row first */
    int32_t left, top; /* integer pixel offsets, Y down, baseline-relative */
    const uint8_t *coverage; /* 0..255; NULL for a zero-area glyph */
} os64_font_glyph_view_t;

typedef struct {
    size_t live_bytes, peak_bytes;
    uint32_t live_faces, live_glyphs;
} os64_font_engine_stats_t;

/* Operations on one engine and its children require caller serialization,
 * including view/release/stats. Distinct engines have no shared mutable state.
 * Success transfers one owned handle from create/open/render. On failure,
 * non-NULL output pointers/structs/scalars are cleared. Output storage must
 * not alias inputs. NULL required arguments are BAD_ARGUMENT.
 *
 * close/release accept NULL as a no-op; double use of a released non-NULL
 * handle is invalid. destroy(NULL) succeeds. Destroy with live children returns
 * BUSY without changing the engine. The caller retains and keeps font bytes
 * immutable until face_close. Glyphs remain valid after face_close and retain
 * their engine until glyph_release. No operation reads a path or a surface. */
typedef struct {
    uint32_t revision;
    size_t struct_size;
    os64_font_status_t (*engine_create)(const os64_font_engine_options_t *,
                                        os64_font_engine_t **out);
    os64_font_status_t (*engine_destroy)(os64_font_engine_t *);
    os64_font_status_t (*engine_stats)(os64_font_engine_t *,
                                       os64_font_engine_stats_t *out);
    os64_font_status_t (*face_open)(os64_font_engine_t *, const uint8_t *bytes,
        size_t length, const os64_font_face_options_t *, os64_font_face_t **out);
    void (*face_close)(os64_font_face_t *);
    os64_font_status_t (*face_info)(os64_font_face_t *, os64_font_face_info_t *out);
    /* Scalar lookup: glyph zero is MISSING, not a successful .notdef lookup. */
    os64_font_status_t (*lookup)(os64_font_face_t *, uint32_t scalar,
                                 uint32_t *glyph_index);
    /* Zero/out-of-range operands are BAD_ARGUMENT. Return scaled 26.6 kerning
     * without whole-pixel grid fitting, in both hint modes. Absent pairs and
     * unsupported pair-table layouts return OK with zero; that zero does not
     * prove the font has no positioning information. Coverage and precedence
     * follow FONT_CONTRACTS.md; this operation does not perform shaping. */
    os64_font_status_t (*pair_adjust)(os64_font_face_t *, uint32_t left,
                                      uint32_t right, os64_font_pos_t *delta_x);
    /* Zero/out-of-range indices are BAD_ARGUMENT. F2 supplies its own missing
     * marker after fallback is exhausted; it does not request .notdef here. */
    os64_font_status_t (*render)(os64_font_face_t *, uint32_t glyph_index,
                                 os64_font_glyph_t **out);
    os64_font_status_t (*glyph_view)(os64_font_glyph_t *, os64_font_glyph_view_t *out);
    void (*glyph_release)(os64_font_glyph_t *);
} os64_font_backend_t;

/* The FreeType library exports this getter. Consumers check revision and
 * struct_size before use. Tests inject a different backend table directly. */
__attribute__((visibility("default")))
const os64_font_backend_t *os64_freetype_backend_v1(void);

#endif
