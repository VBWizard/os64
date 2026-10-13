#include "image/icon_theme.h"
#include "image/icons.h"
#include "os64/conf.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

#ifndef OS64_ICON_DEFAULT_ROOT
#define OS64_ICON_DEFAULT_ROOT "/etc/icons/default"
#endif
#define MANIFEST_CAP 4096u
#define LABEL_CAP 96u
#define THEME_METADATA_BUDGET (64u * 1024u)
#define THEME_ALLOC_HEADER 16u

typedef struct {
    char name[LABEL_CAP];
    char author[LABEL_CAP];
    bool format;
    bool bad;
} theme_manifest_t;

typedef struct {
    const os64_ui_icon_provider_t *source;
    os64_ui_icon_lease_t inner;
    uint32_t charge;
} theme_lease_t;

struct os64_icon_theme {
    os64_ui_icon_provider_t provider;
    os64_icon_catalog_t *selected;
    os64_icon_catalog_t *fallback;
    uint32_t leases;
    uint32_t metadata_bytes;
    char path[OS64_CONF_PATH_MAX];
    char name[LABEL_CAP];
    char author[LABEL_CAP];
};

enum {
    THEME_ROUTE_CHARGE = (sizeof(theme_lease_t) + THEME_ALLOC_HEADER + 15u) & ~15u,
    THEME_OBJECT_CHARGE = (sizeof(os64_icon_theme_t) + THEME_ALLOC_HEADER + 15u) & ~15u,
    THEME_MAX_LEASES = (THEME_METADATA_BUDGET - THEME_OBJECT_CHARGE) /
                       THEME_ROUTE_CHARGE
};

_Static_assert(THEME_OBJECT_CHARGE < THEME_METADATA_BUDGET &&
               THEME_MAX_LEASES > 0 &&
               THEME_OBJECT_CHARGE + THEME_MAX_LEASES * THEME_ROUTE_CHARGE <=
                   THEME_METADATA_BUDGET,
               "icon theme metadata exceeds its budget");

static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    if (!s) return cap;
    while (n < cap && s[n]) ++n;
    return n;
}

static uint32_t metadata_charge(const void *allocation)
{
    size_t payload = os64_malloc_size(allocation);
    /* The heap can reuse a larger block than requested. Charge its actual
     * capacity plus the allocator header, or refuse an unbounded result. */
    return payload > THEME_METADATA_BUDGET - THEME_ALLOC_HEADER ?
        THEME_METADATA_BUDGET + 1u : (uint32_t)payload + THEME_ALLOC_HEADER;
}

static bool label_copy(char out[LABEL_CAP], const char *value)
{
    size_t n = bounded_len(value, LABEL_CAP);
    if (!n || n == LABEL_CAP) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)value[i] < 32 || (unsigned char)value[i] == 127)
            return false;
    os64_memcpy(out, value, n + 1);
    return true;
}

static bool manifest_line(const char *key, const char *value, void *user)
{
    theme_manifest_t *m = user;
    if (!key) { m->bad = true; return true; }
    if (os64_streq(key, "format")) {
        if (m->format || !os64_streq(value, "1")) m->bad = true;
        m->format = true;
    } else if (os64_streq(key, "name")) {
        if (m->name[0] || !label_copy(m->name, value)) m->bad = true;
    } else if (os64_streq(key, "author")) {
        if (m->author[0] || !label_copy(m->author, value)) m->bad = true;
    } else m->bad = true;
    return true;
}

static bool read_manifest(const char *root, theme_manifest_t *out)
{
    size_t n = bounded_len(root, OS64_CONF_PATH_MAX);
    /* The catalog needs room for its longest validated key and filename. */
    if (!n || n == OS64_CONF_PATH_MAX || root[0] != '/' ||
        n + (root[n - 1] == '/' ? 0u : 1u) + 55u > OS64_CONF_PATH_MAX)
        return false;
    char path[OS64_CONF_PATH_MAX];
    os64_memcpy(path, root, n);
    if (path[n - 1] != '/') path[n++] = '/';
    os64_memcpy(path + n, "theme.conf", sizeof("theme.conf"));
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (os64_slurp(path, MANIFEST_CAP, &bytes, &length) != OS64_SLURP_OK)
        return false;
    *out = (theme_manifest_t){0};
    int64_t parsed = os64_conf_parse((const char *)bytes, length,
                                     manifest_line, out);
    os64_free(bytes);
    return parsed >= 0 && !out->bad && out->format &&
           out->name[0] && out->author[0];
}

static os64_ui_icon_status_t theme_acquire(void *user, const char *key,
    uint32_t size, os64_ui_icon_lease_t *out)
{
    os64_icon_theme_t *theme = user;
    if (!out) return OS64_UI_ICON_INVALID;
    *out = (os64_ui_icon_lease_t){0};
    if (!theme) return OS64_UI_ICON_INVALID;
    const os64_ui_icon_provider_t *source = theme->selected ?
        os64_icon_catalog_provider(theme->selected) : NULL;
    os64_ui_icon_lease_t inner = {0};
    os64_ui_icon_status_t status = source ?
        source->acquire(source->user, key, size, &inner) : OS64_UI_ICON_MISSING;
    if (status == OS64_UI_ICON_MISSING) {
        source = os64_icon_catalog_provider(theme->fallback);
        status = source->acquire(source->user, key, size, &inner);
    }
    if (status != OS64_UI_ICON_OK) return status;
    if (theme->leases >= THEME_MAX_LEASES) {
        source->release(source->user, &inner);
        return OS64_UI_ICON_LIMIT;
    }
    theme_lease_t *route = os64_malloc(sizeof(*route));
    if (!route) {
        source->release(source->user, &inner);
        return OS64_UI_ICON_NO_MEMORY;
    }
    uint32_t charge = metadata_charge(route);
    if (charge > THEME_METADATA_BUDGET - theme->metadata_bytes) {
        os64_free(route);
        source->release(source->user, &inner);
        return OS64_UI_ICON_LIMIT;
    }
    route->source = source;
    route->inner = inner;
    route->charge = charge;
    *out = inner;
    out->cookie = route;
    ++theme->leases;
    theme->metadata_bytes += charge;
    return OS64_UI_ICON_OK;
}

static void theme_release(void *user, os64_ui_icon_lease_t *lease)
{
    os64_icon_theme_t *theme = user;
    if (!theme || !lease || !lease->cookie) return;
    theme_lease_t *route = lease->cookie;
    route->source->release(route->source->user, &route->inner);
    theme->metadata_bytes -= route->charge;
    os64_free(route);
    --theme->leases;
    *lease = (os64_ui_icon_lease_t){0};
}

os64_ui_status_code_t os64_icon_theme_create(os64_icon_theme_t **out)
{
    if (!out) return OS64_UI_INVALID;
    *out = NULL;
    os64_icon_theme_t *theme = os64_malloc(sizeof(*theme));
    if (!theme) return OS64_UI_NO_MEMORY;
    os64_memset(theme, 0, sizeof(*theme));
    theme->metadata_bytes = metadata_charge(theme);
    if (theme->metadata_bytes > THEME_METADATA_BUDGET) {
        os64_free(theme);
        return OS64_UI_LIMIT;
    }
    theme->provider = (os64_ui_icon_provider_t){theme, theme_acquire, theme_release};
    os64_ui_status_code_t status = os64_icon_catalog_create(OS64_ICON_DEFAULT_ROOT, 1,
                                                              &theme->fallback);
    if (status != OS64_UI_OK) { os64_free(theme); return status; }
    os64_strcopy(theme->path, sizeof(theme->path), OS64_ICON_DEFAULT_ROOT);
    os64_strcopy(theme->name, sizeof(theme->name), "Default");
    os64_strcopy(theme->author, sizeof(theme->author), "os64");
    theme_manifest_t manifest;
    if (read_manifest(OS64_ICON_DEFAULT_ROOT, &manifest)) {
        os64_strcopy(theme->name, sizeof(theme->name), manifest.name);
        os64_strcopy(theme->author, sizeof(theme->author), manifest.author);
    }

    char selected[OS64_CONF_PATH_MAX];
    int64_t config = os64_conf_get("icons.conf", "set", selected,
                                   sizeof(selected));
    if (config == 0 && os64_streq(selected, OS64_ICON_DEFAULT_ROOT)) {
        *out = theme;
        return OS64_UI_OK;
    }
    if (config == 0 && read_manifest(selected, &manifest)) {
        status = os64_icon_catalog_create(selected, 2, &theme->selected);
        if (status == OS64_UI_OK) {
            os64_strcopy(theme->path, sizeof(theme->path), selected);
            os64_strcopy(theme->name, sizeof(theme->name), manifest.name);
            os64_strcopy(theme->author, sizeof(theme->author), manifest.author);
        } else if (status == OS64_UI_NO_MEMORY) {
            (void)os64_icon_catalog_destroy(theme->fallback);
            os64_free(theme);
            return status;
        }
    }
    if (config != OS64_CONF_NO_FILE && (config != 0 || !theme->selected))
        os64_hprintf(2, "icons: icons.conf set is invalid; using %s\n", OS64_ICON_DEFAULT_ROOT);
    *out = theme;
    return OS64_UI_OK;
}

const os64_ui_icon_provider_t *os64_icon_theme_provider(os64_icon_theme_t *theme)
{ return theme ? &theme->provider : NULL; }

os64_ui_status_code_t os64_icon_theme_destroy(os64_icon_theme_t *theme)
{
    if (!theme) return OS64_UI_INVALID;
    /* Refuse before touching either catalog, so a retained lease leaves the
     * entire theme usable for its eventual release. */
    if (theme->leases) return OS64_UI_BUSY;
    if (theme->selected) (void)os64_icon_catalog_destroy(theme->selected);
    (void)os64_icon_catalog_destroy(theme->fallback);
    os64_free(theme);
    return OS64_UI_OK;
}

const char *os64_icon_theme_path(const os64_icon_theme_t *theme)
{ return theme ? theme->path : NULL; }
const char *os64_icon_theme_name(const os64_icon_theme_t *theme)
{ return theme ? theme->name : NULL; }
const char *os64_icon_theme_author(const os64_icon_theme_t *theme)
{ return theme ? theme->author : NULL; }
