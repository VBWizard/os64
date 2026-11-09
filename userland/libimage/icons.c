#include "image/icons.h"
#include "os64/conf.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"
#include "png/png.h"

enum { ICON_METADATA_LIMIT = 64 * 1024, ICON_ALLOC_HEADER = 16 };

typedef struct {
    char key[48];
    uint32_t *pixels;
    uint64_t used;
    uint32_t charge;
    uint32_t pins;
    uint8_t requested;
    uint8_t source;
    uint8_t status;
    uint8_t occupied;
} icon_record_t;

struct os64_icon_catalog {
    os64_ui_icon_provider_t provider;
    uint64_t generation;
    uint64_t clock;
    uint64_t file_attempts;
    uint32_t records;
    uint32_t leases;
    uint32_t retained_bytes;
    char root[OS64_CONF_PATH_MAX];
    icon_record_t entries[OS64_ICON_CATALOG_RECORD_LIMIT];
};

_Static_assert(sizeof(os64_icon_catalog_t) + ICON_ALLOC_HEADER + 48 <= ICON_METADATA_LIMIT,
    "icon catalog metadata exceeds its budget");

static const uint8_t sizes[] = {16, 24, 32, 48, 64};

static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    if (!s) return cap;
    while (n < cap && s[n]) ++n;
    return n;
}

static void discard(os64_icon_catalog_t *c, icon_record_t *e)
{
    if (!e->occupied) return;
    os64_free(e->pixels);
    c->retained_bytes -= e->charge;
    --c->records;
    os64_memset(e, 0, sizeof(*e));
}

static icon_record_t *oldest_unleased(os64_icon_catalog_t *c)
{
    icon_record_t *oldest = NULL;
    for (size_t i = 0; i < OS64_ICON_CATALOG_RECORD_LIMIT; ++i) {
        icon_record_t *e = &c->entries[i];
        if (e->occupied && !e->pins && (!oldest || e->used < oldest->used))
            oldest = e;
    }
    return oldest;
}

static icon_record_t *free_record(os64_icon_catalog_t *c)
{
    for (size_t i = 0; i < OS64_ICON_CATALOG_RECORD_LIMIT; ++i)
        if (!c->entries[i].occupied) return &c->entries[i];
    icon_record_t *e = oldest_unleased(c);
    if (e) discard(c, e);
    return e;
}

static bool make_path(const os64_icon_catalog_t *c, const char *key,
                      uint32_t size, char path[OS64_CONF_PATH_MAX])
{
    size_t root = bounded_len(c->root, OS64_CONF_PATH_MAX);
    size_t key_len = bounded_len(key, 48);
    const char *digits = size == 16 ? "16" : size == 24 ? "24" :
                         size == 32 ? "32" : size == 48 ? "48" : "64";
    bool slash = c->root[root - 1] != '/';
    size_t total = root + (size_t)slash + key_len + 1 + 2 + 4 + 1;
    if (total > OS64_CONF_PATH_MAX) return false;
    os64_memcpy(path, c->root, root);
    size_t at = root;
    if (slash) path[at++] = '/';
    os64_memcpy(path + at, key, key_len); at += key_len;
    path[at++] = '-';
    path[at++] = digits[0]; path[at++] = digits[1];
    os64_memcpy(path + at, ".png", 4); at += 4;
    path[at] = 0;
    return true;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static os64_ui_icon_status_t load_variant(os64_icon_catalog_t *c,
    const char *key, uint32_t size, os64_png_image_t *image)
{
    char path[OS64_CONF_PATH_MAX];
    if (!make_path(c, key, size, path)) return OS64_UI_ICON_LIMIT;
    uint8_t *bytes = NULL;
    size_t len = 0;
    ++c->file_attempts;
    os64_slurp_status_t read = os64_slurp(path, OS64_ICON_FILE_CAP, &bytes, &len);
    if (read == OS64_SLURP_NO_FILE) {
        /* Open can fail on an existing asset (for example, exhausted handles).
         * stat success proves that case is I/O, so it stays retryable and a
         * selected theme cannot silently fall back to the default artwork.
         * A failed stat is still ambiguous: this ABI gives no distinct
         * missing-path error, so it cannot prove absence. */
        os64_dirent_t entry;
        return os64_stat(path, &entry) == 0 ? OS64_UI_ICON_IO :
                                               OS64_UI_ICON_MISSING;
    }
    if (read == OS64_SLURP_TOO_BIG) return OS64_UI_ICON_LIMIT;
    if (read == OS64_SLURP_NO_MEMORY) return OS64_UI_ICON_NO_MEMORY;
    if (read != OS64_SLURP_OK) return OS64_UI_ICON_IO;

    static const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    os64_ui_icon_status_t result = OS64_UI_ICON_MALFORMED;
    if (len >= 24 && !os64_memcmp(bytes, signature, 8) &&
        be32(bytes + 8) == 13 && !os64_memcmp(bytes + 12, "IHDR", 4) &&
        be32(bytes + 16) == size && be32(bytes + 20) == size) {
        os64_png_status_t png = os64_png_decode(bytes, len, 64u * 64u, image);
        if (png == OS64_PNG_OK) {
            if (image->width == size && image->height == size)
                result = OS64_UI_ICON_OK;
            else os64_png_free(image);
        } else if (png == OS64_PNG_LIMIT) result = OS64_UI_ICON_LIMIT;
        else if (png == OS64_PNG_NO_MEMORY) result = OS64_UI_ICON_NO_MEMORY;
    }
    os64_free(bytes);
    return result;
}

static bool persistent(os64_ui_icon_status_t status)
{
    return status == OS64_UI_ICON_MISSING || status == OS64_UI_ICON_MALFORMED;
}

static os64_ui_icon_status_t catalog_acquire(void *user, const char *key,
    uint32_t size, os64_ui_icon_lease_t *out)
{
    os64_icon_catalog_t *c = user;
    if (!out) return OS64_UI_ICON_INVALID;
    *out = (os64_ui_icon_lease_t){0};
    if (!c || !os64_ui_icon_key_valid(key)) return OS64_UI_ICON_INVALID;
    int index = -1;
    for (int i = 0; i < 5; ++i) if (sizes[i] == size) index = i;
    if (index < 0) return OS64_UI_ICON_INVALID;

    for (size_t i = 0; i < OS64_ICON_CATALOG_RECORD_LIMIT; ++i) {
        icon_record_t *e = &c->entries[i];
        if (!e->occupied || e->requested != size || !os64_streq(e->key, key)) continue;
        e->used = ++c->clock;
        if (e->status != OS64_UI_ICON_OK) return (os64_ui_icon_status_t)e->status;
        if (e->pins == UINT32_MAX || c->leases == UINT32_MAX) return OS64_UI_ICON_LIMIT;
        ++e->pins; ++c->leases;
        *out = (os64_ui_icon_lease_t){e->pixels, e->source, e->source,
            e->source, c->generation, e};
        return OS64_UI_ICON_OK;
    }

    os64_png_image_t image = {0};
    os64_ui_icon_status_t status = OS64_UI_ICON_MISSING;
    for (int i = index; i >= 0; --i) {
        status = load_variant(c, key, sizes[i], &image);
        if (status != OS64_UI_ICON_MISSING) break;
    }
    if (status == OS64_UI_ICON_OK) {
        /* A reused heap block may provide more space than requested. Charge
         * its actual payload capacity and the heap's 16-byte block header. */
        size_t capacity = os64_malloc_size(image.pixels);
        if (capacity > OS64_ICON_CATALOG_PIXEL_BUDGET - ICON_ALLOC_HEADER) {
            os64_png_free(&image); return OS64_UI_ICON_LIMIT;
        }
        uint32_t charge = (uint32_t)capacity + ICON_ALLOC_HEADER;
        while (c->retained_bytes > OS64_ICON_CATALOG_PIXEL_BUDGET - charge) {
            icon_record_t *victim = oldest_unleased(c);
            if (!victim) { os64_png_free(&image); return OS64_UI_ICON_LIMIT; }
            discard(c, victim);
        }
        icon_record_t *e = free_record(c);
        if (!e) { os64_png_free(&image); return OS64_UI_ICON_LIMIT; }
        os64_memset(e, 0, sizeof(*e));
        os64_memcpy(e->key, key, bounded_len(key, 48) + 1);
        e->occupied = 1; e->requested = (uint8_t)size;
        e->source = (uint8_t)image.width; e->status = OS64_UI_ICON_OK;
        e->pixels = image.pixels; e->charge = charge; e->pins = 1;
        e->used = ++c->clock; ++c->records; ++c->leases;
        c->retained_bytes += charge;
        *out = (os64_ui_icon_lease_t){e->pixels, e->source, e->source,
            e->source, c->generation, e};
        return OS64_UI_ICON_OK;
    }
    if (persistent(status)) {
        icon_record_t *e = free_record(c);
        if (e) {
            os64_memset(e, 0, sizeof(*e));
            os64_memcpy(e->key, key, bounded_len(key, 48) + 1);
            e->occupied = 1; e->requested = (uint8_t)size;
            e->status = (uint8_t)status; e->used = ++c->clock;
            ++c->records;
        }
    }
    return status;
}

static void catalog_release(void *user, os64_ui_icon_lease_t *lease)
{
    os64_icon_catalog_t *c = user;
    if (!c || !lease || !lease->cookie) return;
    uintptr_t address = (uintptr_t)lease->cookie;
    uintptr_t start = (uintptr_t)c->entries;
    uintptr_t end = (uintptr_t)(c->entries + OS64_ICON_CATALOG_RECORD_LIMIT);
    if (address < start || address >= end ||
        (address - start) % sizeof(icon_record_t) != 0) return;
    icon_record_t *e = (icon_record_t *)address;
    if (
        !e->occupied || e->pixels != lease->pixels || !e->pins ||
        lease->generation != c->generation) return;
    --e->pins; --c->leases;
    *lease = (os64_ui_icon_lease_t){0};
}

os64_ui_status_code_t os64_icon_catalog_create(const char *root,
    uint64_t generation, os64_icon_catalog_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    size_t n = bounded_len(root, OS64_CONF_PATH_MAX);
    if (!n || n == OS64_CONF_PATH_MAX || root[0] != '/') return OS64_UI_INVALID;
    /* A full-size key needs 55 bytes after a directory separator. */
    if (n + (root[n - 1] == '/' ? 0u : 1u) + 47u + 7u + 1u >
        OS64_CONF_PATH_MAX) return OS64_UI_LIMIT;
    os64_icon_catalog_t *c = os64_malloc(sizeof(*c));
    if (!c) return OS64_UI_NO_MEMORY;
    if (os64_malloc_size(c) > ICON_METADATA_LIMIT - ICON_ALLOC_HEADER) {
        os64_free(c); return OS64_UI_LIMIT;
    }
    os64_memset(c, 0, sizeof(*c));
    os64_memcpy(c->root, root, n + 1);
    c->generation = generation;
    c->provider = (os64_ui_icon_provider_t){c, catalog_acquire, catalog_release};
    *out = c;
    return OS64_UI_OK;
}

const os64_ui_icon_provider_t *os64_icon_catalog_provider(os64_icon_catalog_t *c)
{ return c ? &c->provider : NULL; }

os64_ui_status_code_t os64_icon_catalog_refresh(os64_icon_catalog_t *c)
{
    if (!c) return OS64_UI_INVALID;
    if (c->leases) return OS64_UI_BUSY;
    for (size_t i = 0; i < OS64_ICON_CATALOG_RECORD_LIMIT; ++i)
        discard(c, &c->entries[i]);
    return OS64_UI_OK;
}

os64_ui_status_code_t os64_icon_catalog_destroy(os64_icon_catalog_t *c)
{
    if (!c) return OS64_UI_INVALID;
    if (c->leases) return OS64_UI_BUSY;
    (void)os64_icon_catalog_refresh(c);
    os64_free(c);
    return OS64_UI_OK;
}

void os64_icon_catalog_stats(const os64_icon_catalog_t *c,
    os64_icon_catalog_stats_t *out)
{
    if (!out) return;
    *out = c ? (os64_icon_catalog_stats_t){c->records, c->leases,
        c->retained_bytes, c->file_attempts} : (os64_icon_catalog_stats_t){0};
}
