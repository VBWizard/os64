#include "os64/font_provider.h"
#include "text_internal.h"
#include "os64/str.h"

#define FAMILY_SOURCES (OS64_FONT_FAMILY_STYLES + OS64_FONT_CONFIG_FALLBACK_MAX)
#define FAMILY_FILES (OS64_FONT_FAMILY_COUNT * FAMILY_SOURCES)
typedef struct {
    os64_font_family_t family;
    uint32_t size, style;
    os64_text_font_t *fonts[OS64_FONT_ROLE_FONTS_MAX];
    size_t count;
} family_entry;
struct os64_font_family_cache {
    os64_text_context_t *text;
    os64_text_font_t *anchor;
    os64_font_family_spec_t specs[OS64_FONT_FAMILY_COUNT];
    uint8_t *files[FAMILY_FILES];
    size_t file_count, count;
    family_entry entries[OS64_FONT_FAMILY_CACHE_MAX]; /* oldest first */
    os64_text_font_t **borrowed;
};

static os64_font_status_t source_open(os64_text_context_t *text,
    const os64_font_source_t *source, uint32_t size, os64_text_font_t **out)
{
    if (source->kind == OS64_FONT_SOURCE_BUILTIN)
        return os64_text_font_bitmap(text, out);
    os64_font_face_options_t options = {size, OS64_FONT_HINT_NORMAL};
    return os64_text_font_open(text, source->bytes, source->length, &options, out);
}
static void entry_release(family_entry *entry)
{
    for (size_t i = 0; i < entry->count; ++i) os64_text_font_release(entry->fonts[i]);
    *entry = (family_entry){0};
}
static void evict(os64_font_family_cache_t *cache)
{
    entry_release(&cache->entries[0]);
    --cache->count;
    for (size_t i = 0; i < cache->count; ++i) cache->entries[i] = cache->entries[i + 1];
    cache->entries[cache->count] = (family_entry){0};
}
void os64_font_family_cache_destroy(os64_font_family_cache_t *cache)
{
    if (!cache) return;
    os64_text_context_t *text = cache->text;
    text_free(text, cache->borrowed);
    while (cache->count) evict(cache);
    for (size_t i = 0; i < cache->file_count; ++i) text_free(text, cache->files[i]);
    os64_text_font_release(cache->anchor);
    text_free(text, cache);
}
os64_font_status_t os64_font_family_cache_create(os64_text_context_t *text,
    const os64_font_family_spec_t specs[OS64_FONT_FAMILY_COUNT],
    os64_font_family_cache_t **out, os64_font_family_t *family, size_t *source)
{
    if (out) *out = NULL;
    if (family) *family = OS64_FONT_FAMILY_COUNT;
    if (source) *source = SIZE_MAX;
    if (!text || !out) return OS64_FONT_BAD_ARGUMENT;
    const os64_font_family_spec_t defaults[OS64_FONT_FAMILY_COUNT] = {0};
    if (!specs) specs = defaults;
    os64_font_family_cache_t *cache = text_alloc(text, sizeof(*cache));
    if (!cache) return text->refusal;
    *cache = (os64_font_family_cache_t){.text = text};
    os64_font_status_t status = os64_text_font_bitmap(text, &cache->anchor);
    if (status) goto fail;
    const uint8_t *original[FAMILY_FILES] = {0};
    size_t lengths[FAMILY_FILES] = {0};
    for (size_t f = 0; f < OS64_FONT_FAMILY_COUNT; ++f) {
        if (family) *family = (os64_font_family_t)f;
        if (source) *source = SIZE_MAX;
        if (specs[f].fallback_count > OS64_FONT_CONFIG_FALLBACK_MAX) {
            status = OS64_FONT_BAD_ARGUMENT; goto fail;
        }
        cache->specs[f].fallback_count = specs[f].fallback_count;
        for (size_t s = 0; s < OS64_FONT_FAMILY_STYLES + specs[f].fallback_count; ++s) {
            if (source) *source = s;
            const os64_font_source_t *in = s < OS64_FONT_FAMILY_STYLES ?
                &specs[f].styles[s] : &specs[f].fallbacks[s - OS64_FONT_FAMILY_STYLES];
            os64_font_source_t *dst = s < OS64_FONT_FAMILY_STYLES ?
                &cache->specs[f].styles[s] : &cache->specs[f].fallbacks[s - OS64_FONT_FAMILY_STYLES];
            if (in->kind == OS64_FONT_SOURCE_BUILTIN) {
                if (in->bytes || in->length) {status = OS64_FONT_BAD_ARGUMENT; goto fail;}
                continue;
            }
            if (in->kind != OS64_FONT_SOURCE_OUTLINE || !in->bytes || !in->length) {
                status = OS64_FONT_BAD_ARGUMENT; goto fail;
            }
            if (in->length > OS64_FONT_FILE_MAX) {status = OS64_FONT_LIMIT; goto fail;}
            size_t i = 0;
            while (i < cache->file_count &&
                   (original[i] != in->bytes || lengths[i] != in->length)) ++i;
            if (i == cache->file_count) {
                uint8_t *copy = text_alloc(text, in->length);
                if (!copy) {status = text->refusal; goto fail;}
                os64_memcpy(copy, in->bytes, in->length);
                cache->files[i] = copy; original[i] = in->bytes; lengths[i] = in->length;
                ++cache->file_count;
                /* Validate unopened styles now; publishing half a family would
                 * defer a bad configured path/face until a page happens to use it. */
                os64_text_font_t *probe = NULL;
                os64_font_source_t check = {OS64_FONT_SOURCE_OUTLINE, copy, in->length};
                status = source_open(text, &check, 16, &probe);
                os64_text_font_release(probe);
                if (status) goto fail;
            }
            *dst = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, cache->files[i], in->length};
        }
    }
    if (family) *family = OS64_FONT_FAMILY_COUNT;
    if (source) *source = SIZE_MAX;
    *out = cache;
    return OS64_FONT_OK;
fail:
    os64_font_family_cache_destroy(cache);
    return status;
}
static os64_font_status_t entry_open(os64_font_family_cache_t *cache, family_entry *entry)
{
    const os64_font_family_spec_t *spec = &cache->specs[entry->family];
    const os64_font_source_t *sources[OS64_FONT_ROLE_FONTS_MAX] = {&spec->styles[entry->style]};
    size_t count = 1;
    bool builtin = sources[0]->kind == OS64_FONT_SOURCE_BUILTIN;
    for (size_t s = 0; s < spec->fallback_count; ++s) {
        const os64_font_source_t *next = &spec->fallbacks[s];
        bool duplicate = false;
        for (size_t i = 0; i < count; ++i)
            if (sources[i]->kind == next->kind && sources[i]->bytes == next->bytes &&
                sources[i]->length == next->length) duplicate = true;
        if (duplicate) continue;
        sources[count++] = next;
        builtin |= next->kind == OS64_FONT_SOURCE_BUILTIN;
    }
    const os64_font_source_t bitmap = {0};
    if (!builtin) sources[count++] = &bitmap;
    for (size_t i = 0; i < count; ++i) {
        os64_font_status_t status = source_open(cache->text, sources[i], entry->size,
                                               &entry->fonts[entry->count]);
        if (status) return status;
        ++entry->count;
    }
    return OS64_FONT_OK;
}
os64_font_status_t os64_font_family_open(os64_font_family_cache_t *cache,
    const os64_font_family_list_t *families, bool bold, bool italic,
    uint32_t size, os64_font_role_view_t *out)
{
    if (out) *out = (os64_font_role_view_t){0};
    if (!cache) return OS64_FONT_BAD_ARGUMENT;
    /* Separate publication storage makes the next-call lifetime observable
     * even on cache hits; the entry and its fonts may remain cached. */
    text_free(cache->text, cache->borrowed);
    cache->borrowed = NULL;
    if (!families || !out || families->generic < OS64_FONT_FAMILY_SERIF ||
        families->generic >= OS64_FONT_FAMILY_COUNT || (families->count && !families->names) ||
        !size || size > OS64_FONT_PIXEL_MAX) return OS64_FONT_BAD_ARGUMENT;
    uint32_t style = (bold ? 1u : 0u) + (italic ? 2u : 0u);
    size_t index = 0;
    while (index < cache->count) {
        family_entry *e = &cache->entries[index];
        if (e->family == families->generic && e->style == style && e->size == size) break;
        ++index;
    }
    family_entry entry = {0};
    if (index < cache->count) {
        entry = cache->entries[index];
        for (size_t i = index; i + 1 < cache->count; ++i) cache->entries[i] = cache->entries[i + 1];
        cache->entries[cache->count - 1] = entry;
    } else {
        if (cache->count == OS64_FONT_FAMILY_CACHE_MAX) evict(cache);
        for (;;) {
            entry = (family_entry){.family = families->generic, .style = style, .size = size};
            os64_font_status_t status = entry_open(cache, &entry);
            if (!status) break;
            entry_release(&entry);
            /* A fallback adds faces too. The engine, rather than entry count,
             * decides whether retained runs leave room for another request. */
            if (status != OS64_FONT_LIMIT || !cache->count) return status;
            evict(cache);
        }
        cache->entries[cache->count++] = entry;
    }
    for (;;) {
        cache->borrowed = text_alloc(cache->text, entry.count * sizeof(*cache->borrowed));
        if (cache->borrowed) break;
        if (cache->text->refusal != OS64_FONT_LIMIT || cache->count <= 1)
            return cache->text->refusal;
        /* The requested entry is newest; free older entries to publish it. */
        evict(cache);
    }
    os64_memcpy(cache->borrowed, entry.fonts, entry.count * sizeof(*cache->borrowed));
    os64_font_face_info_t primary = entry.fonts[0]->info;
    int32_t baseline = (int32_t)(((int64_t)primary.ascent + 63) / 64);
    *out = (os64_font_role_view_t){.text = cache->text, .fonts = cache->borrowed,
        .font_count = entry.count, .primary = primary, .identity = entry.fonts[0]->identity,
        .baseline_px = baseline, .row_height_px = baseline + (int32_t)
            (((int64_t)primary.line_height - primary.ascent + 63) / 64)};
    return OS64_FONT_OK;
}
