#include "os64/font_config.h"
#include "os64/conf.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "font_config_internal.h"

static int path_order(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

typedef struct {
    os64_text_context_t *context;
    os64_font_catalog_t *catalog;
    size_t remaining, scanned, opened;
    uint8_t *bytes[OS64_FONT_DISCOVERY_MAX];
    size_t lengths[OS64_FONT_DISCOVERY_MAX];
} discovery_t;

static bool same_bytes(const uint8_t *a, const uint8_t *b, size_t length)
{
    for (size_t i = 0; i < length; ++i) if (a[i] != b[i]) return false;
    return true;
}

static size_t inspect(discovery_t *d, const char *path)
{
    os64_font_catalog_t *catalog = d->catalog;
    for (size_t i = 0; i < catalog->count; ++i)
        if (os64_streq(path, catalog->entries[i].path)) return i;
    if (catalog->count == OS64_FONT_DISCOVERY_MAX) {
        catalog->limited = true; return SIZE_MAX;
    }
    size_t index = catalog->count;
    os64_font_catalog_entry_t *entry = &catalog->entries[index];
    *entry = (os64_font_catalog_entry_t){0};
    os64_strcopy(entry->path, sizeof(entry->path), path);
    uint8_t *bytes = NULL;
    size_t length = 0;
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
    if (!os64_streq(path, "builtin")) {
        entry->status = font_source_read(path, &d->remaining, &bytes, &length);
        if (entry->status) {
            if (entry->status == OS64_FONT_CONFIG_LIMIT) catalog->limited = true;
            ++catalog->count;
            return index;
        }
        /* Compare retained, validated sources exactly. Family/style labels
         * cannot distinguish revisions; a hash alone cannot prove equality.
         * The shared read budget also bounds the retained source memory. */
        for (size_t i = 0; i < index; ++i)
            if (d->bytes[i] && d->lengths[i] == length &&
                same_bytes(d->bytes[i], bytes, length)) {
                os64_free(bytes);
                return i;
            }
        specs[0].primary = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, bytes, length};
    }
    ++catalog->count;
    os64_font_set_t *set = NULL;
    entry->font_status = os64_font_set_prepare(d->context, specs, &set);
    entry->status = entry->font_status ? OS64_FONT_CONFIG_FACE : OS64_FONT_CONFIG_OK;
    if (!entry->status) {
        os64_font_role_view_t view;
        if (os64_font_set_view(set, OS64_FONT_ROLE_UI, &view) == OS64_FONT_OK)
            entry->info = view.primary;
        os64_font_set_release(set);
        d->bytes[index] = bytes;
        d->lengths[index] = length;
    } else os64_free(bytes);
    return index;
}

static void scan(discovery_t *d, const char *folder)
{
    os64_font_catalog_t *catalog = d->catalog;
    int64_t fd = os64_opendir(folder);
    /* Personal/custom folders may not exist. A missing optional directory
     * does not make a successfully scanned system collection unavailable. */
    if (fd < 0) return;
    ++d->opened;
    while (d->scanned < OS64_FONT_DISCOVERY_SCAN_MAX) {
        os64_dirent_t entry;
        int64_t read = os64_readdir((int32_t)fd, &entry);
        if (!read) break;
        if (read < 0) { catalog->directory_unavailable = true; break; }
        ++d->scanned;
        if (catalog->count == OS64_FONT_DISCOVERY_MAX) {
            catalog->limited = true; break;
        }
        if (entry.flags & OS64_DE_DIR) continue;
        if (entry.name[0] == '.') continue; /* staged installation files */
        size_t len = 0;
        while (len < sizeof(entry.name) && entry.name[len]) ++len;
        size_t prefix = os64_strlen(folder);
        if (!len || len == sizeof(entry.name) || len + prefix + 2 > OS64_FONT_PATH_CAP) {
            catalog->limited = true; continue;
        }
        char path[OS64_FONT_PATH_CAP];
        os64_strcopy(path, sizeof(path), folder);
        path[prefix] = '/';
        os64_strcopy(path + prefix + 1, sizeof(path) - prefix - 1, entry.name);
        (void)inspect(d, path);
    }
    if (d->scanned == OS64_FONT_DISCOVERY_SCAN_MAX) catalog->limited = true;
    os64_close((int32_t)fd);
}

static bool beside_config(const char *path, char folder[OS64_FONT_PATH_CAP])
{
    size_t n = 0;
    while (n < OS64_FONT_PATH_CAP && path[n]) ++n;
    if (!n || n == OS64_FONT_PATH_CAP || path[0] != '/') return false;
    os64_strcopy(folder, OS64_FONT_PATH_CAP, path);
    while (n && folder[n - 1] != '/') --n;
    if (!n || n + sizeof("fonts") > OS64_FONT_PATH_CAP) return false;
    os64_strcopy(folder + n, OS64_FONT_PATH_CAP - n, "fonts");
    return true;
}

os64_font_config_status_t os64_font_config_discover(os64_text_context_t *context,
    const os64_font_config_t *config, os64_font_catalog_t **out)
{
    if (out) *out = NULL;
    if (!context || !config || !out) return OS64_FONT_CONFIG_SYNTAX;
    /* Encoding validates edited path arrays before any string traversal. */
    char checked[4096];
    if (os64_font_config_encode(config, checked, sizeof(checked)) < 0)
        return OS64_FONT_CONFIG_PATH;
    os64_font_catalog_t *catalog = os64_malloc(sizeof(*catalog));
    if (!catalog) return OS64_FONT_CONFIG_NO_MEMORY;
    os64_memset(catalog, 0, sizeof(*catalog));
    discovery_t d = {.context = context, .catalog = catalog,
                     .remaining = OS64_FONT_SOURCE_BYTES_MAX};
    (void)inspect(&d, "builtin");
    /* Configured paths win representative selection. Keep aliases so two
     * roles selecting identical copies still highlight the shared row. */
    for (size_t r = 0; r < OS64_FONT_ROLE_COUNT; ++r)
        for (size_t s = 0; s < 3; ++s) {
            const char *path = config->roles[r].face[s];
            if (!path[0]) continue;
            size_t index = inspect(&d, path);
            if (index == SIZE_MAX) continue;
            os64_font_catalog_alias_t *alias = &catalog->aliases[catalog->alias_count++];
            os64_strcopy(alias->path, sizeof(alias->path), path);
            os64_strcopy(alias->canonical, sizeof(alias->canonical), catalog->entries[index].path);
        }

    char folders[4][OS64_FONT_PATH_CAP] = {"/home/fonts", "/etc/fonts"};
    char target[OS64_FONT_PATH_CAP];
    if (os64_conf_target("fonts.conf", target, sizeof(target)) >= 0)
        (void)beside_config(target, folders[2]);
    (void)beside_config(config->path, folders[3]);
    for (size_t i = 0; i < 4; ++i) {
        if (!folders[i][0]) continue;
        bool seen = false;
        for (size_t j = 0; j < i; ++j)
            if (os64_streq(folders[i], folders[j])) seen = true;
        if (!seen) scan(&d, folders[i]);
    }
    if (!d.opened) catalog->directory_unavailable = true;
    for (size_t i = 0; i < catalog->count; ++i) os64_free(d.bytes[i]);
    /* Keep builtin first. Stable asset ordering makes repeated refreshes
     * navigable even when two files carry identical family/style labels. */
    for (size_t i = 2; i < catalog->count; ++i) {
        os64_font_catalog_entry_t entry = catalog->entries[i];
        size_t j = i;
        while (j > 1 && path_order(catalog->entries[j - 1].path, entry.path) > 0) {
            catalog->entries[j] = catalog->entries[j - 1]; --j;
        }
        catalog->entries[j] = entry;
    }
    *out = catalog;
    return OS64_FONT_CONFIG_OK;
}

void os64_font_catalog_release(os64_font_catalog_t *catalog) { os64_free(catalog); }

int os64_font_catalog_find(const os64_font_catalog_t *catalog, const char *path)
{
    if (!catalog || !path) return -1;
    for (size_t i = 0; i < catalog->alias_count; ++i)
        if (os64_streq(catalog->aliases[i].path, path)) {
            path = catalog->aliases[i].canonical;
            break;
        }
    for (size_t i = 0; i < catalog->count; ++i)
        if (os64_streq(catalog->entries[i].path, path)) return (int)i;
    return -1;
}
