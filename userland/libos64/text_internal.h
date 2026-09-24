#ifndef OS64_TEXT_INTERNAL_H
#define OS64_TEXT_INTERNAL_H
#include "os64/text.h"
#include <stdbool.h>
#include <limits.h>

#define TEXT_BUCKETS 257u
#define TEXT_MARKER UINT32_MAX
#define TEXT_TAB (UINT32_MAX-1u)
#define TEXT_CP437_BASE 0x200000u

typedef struct text_image text_image;
struct os64_text_context {
    os64_font_memory_t memory;
    const os64_font_backend_t *backend;
    os64_font_engine_t *engine;
    size_t cap, live, cache_cap, cache_bytes, runs;
    os64_font_status_t refusal;
    uint64_t next_identity;
    os64_text_font_t *fonts;
    text_image *buckets[TEXT_BUCKETS], *oldest, *newest;
};
struct os64_text_font {
    os64_text_context_t *context;
    os64_text_font_t *next;
    os64_font_face_t *face;
    os64_font_face_info_t info;
    os64_font_face_options_t options;
    uint8_t *bytes;
    uint64_t identity;
    size_t refs;
};
struct text_image {
    text_image *hash_next, *older, *newer;
    os64_text_context_t *context;
    os64_font_glyph_t *glyph;
    os64_font_glyph_view_t view;
    uint64_t identity;
    uint32_t index, width, height;
    int32_t top;
    size_t pins, cost;
    uint8_t *owned;
};
struct os64_text_run {
    os64_text_context_t *context;
    os64_text_run_view_t view;
    os64_text_font_t *fonts[OS64_TEXT_FALLBACK_MAX];
    size_t font_count;
    text_image **images;
    os64_font_pos_t cell_advance, cell_ascent, cell_height;
    os64_text_encoding_t encoding;
};
typedef struct {
    size_t end;
    uint32_t scalar;
    bool extra_marker;
} text_cluster;

#pragma GCC visibility push(hidden)
void *text_alloc(os64_text_context_t *, size_t);
void text_free(os64_text_context_t *, void *);
size_t text_evict(os64_text_context_t *);
void text_trim(os64_text_context_t *);
void text_drop_font_cache(os64_text_context_t *, uint64_t);
os64_font_status_t text_status(os64_text_context_t *, os64_font_status_t);
bool text_retry(os64_text_context_t *, os64_font_status_t);
os64_font_status_t text_image_get(os64_text_font_t *, uint32_t, uint32_t,
    uint32_t, int32_t, text_image **);
void text_image_unpin(text_image *);
text_cluster text_decode(const uint8_t *, size_t, size_t, os64_text_encoding_t, bool);
bool text_w1_supported(uint32_t);
uint8_t text_box_pixel(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
const uint8_t *text_bitmap_rows(uint32_t);
#pragma GCC visibility pop
static inline int64_t text_floor(int64_t n, int64_t d)
{
    int64_t q=n/d;
    return q-(n%d<0);
}
static inline int64_t text_pixel(int64_t position)
{
    return text_floor(position+32,64);
}
#endif
