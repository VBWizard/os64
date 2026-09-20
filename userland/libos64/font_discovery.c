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

static void inspect(os64_text_context_t *context, os64_font_catalog_t *catalog,
                    const char *path, size_t *remaining)
{
    for (size_t i = 0; i < catalog->count; ++i)
        if (os64_streq(path, catalog->entries[i].path)) return;
    if (catalog->count == OS64_FONT_DISCOVERY_MAX) { catalog->limited = true; return; }
    os64_font_catalog_entry_t *entry = &catalog->entries[catalog->count++];
    os64_strcopy(entry->path, sizeof(entry->path), path);
    uint8_t *bytes = NULL;
    size_t length = 0;
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT] = {0};
    if (!os64_streq(path, "builtin")) {
        entry->status = font_source_read(path, remaining, &bytes, &length);
        if (entry->status) {
            if (entry->status == OS64_FONT_CONFIG_LIMIT) catalog->limited = true;
            return;
        }
        specs[0].primary = (os64_font_source_t){OS64_FONT_SOURCE_OUTLINE, bytes, length};
    }
    os64_font_set_t *set = NULL;
    entry->font_status = os64_font_set_prepare(context, specs, &set);
    entry->status = entry->font_status ? OS64_FONT_CONFIG_FACE : OS64_FONT_CONFIG_OK;
    os64_free(bytes);
    if (!entry->status) {
        os64_font_role_view_t view;
        if (os64_font_set_view(set, OS64_FONT_ROLE_UI, &view) == OS64_FONT_OK)
            entry->info = view.primary;
        os64_font_set_release(set);
    }
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
    size_t remaining = OS64_FONT_SOURCE_BYTES_MAX;
    inspect(context, catalog, "builtin", &remaining);
    for (size_t r = 0; r < OS64_FONT_ROLE_COUNT; ++r)
        for (size_t s = 0; s < 3; ++s)
            if (config->roles[r].face[s][0])
                inspect(context, catalog, config->roles[r].face[s], &remaining);

    char folder[OS64_FONT_PATH_CAP];
    size_t n = 0;
    while (n < sizeof(config->path) && config->path[n]) ++n;
    if (n == sizeof(config->path) || (n && config->path[0] != '/')) goto unavailable;
    if (n) os64_strcopy(folder, sizeof(folder), config->path);
    else if (os64_conf_target("fonts.conf", folder, sizeof(folder)) < 0) goto unavailable;
    n = os64_strlen(folder);
    while (n && folder[n - 1] != '/') --n;
    if (!n || n + sizeof("fonts") > sizeof(folder)) goto unavailable;
    os64_strcopy(folder + n, sizeof(folder) - n, "fonts");
    int64_t fd = os64_opendir(folder);
    if (fd < 0) goto unavailable;
    for (size_t scanned = 0; scanned < OS64_FONT_DISCOVERY_SCAN_MAX; ++scanned) {
        os64_dirent_t entry;
        int64_t read = os64_readdir((int32_t)fd, &entry);
        if (!read) break;
        if (read < 0) { catalog->directory_unavailable = true; break; }
        if (catalog->count == OS64_FONT_DISCOVERY_MAX) {
            catalog->limited = true; break;
        }
        if (scanned + 1 == OS64_FONT_DISCOVERY_SCAN_MAX) catalog->limited = true;
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
        inspect(context, catalog, path, &remaining);
    }
    os64_close((int32_t)fd);
    goto sorted;
unavailable:
    catalog->directory_unavailable = true;
sorted:
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
