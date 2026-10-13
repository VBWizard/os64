/* Host harness for the resolver. Catalog and filesystem stubs make each
 * fallback and lease route observable without coupling this test to PNG I/O. */
#include "image/icon_theme.h"
#include "image/icons.h"
#include "os64/conf.h"
#include "os64/io.h"
#include "os64/slurp.h"
#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct os64_icon_catalog {
    os64_ui_icon_provider_t provider;
    uint64_t generation;
    unsigned leases;
    bool selected;
};

static const char *configured_set;
static const char *selected_manifest = "format = 1\nname = Quaint\nauthor = Fixture author\n";
static unsigned selected_calls, default_calls, releases;
static const char *fault_root;
static os64_slurp_status_t read_fault;
static int64_t parse_fault, config_fault;
static uint32_t pixel = 0xff123456;

void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
size_t os64_malloc_size(const void *p) { return malloc_usable_size((void *)p); }
void *os64_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
bool os64_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
size_t os64_strcopy(char *d, size_t n, const char *s)
{
    size_t len = strlen(s);
    if (n) { size_t copy = len < n - 1 ? len : n - 1; memcpy(d, s, copy); d[copy] = 0; }
    return len;
}
int32_t os64_hprintf(int32_t handle, const char *fmt, ...)
{ (void)handle; (void)fmt; return 0; }
int64_t os64_conf_get(const char *name, const char *key, char *out, size_t cap)
{
    assert(!strcmp(name, "icons.conf") && !strcmp(key, "set"));
    if (config_fault) return config_fault;
    if (!configured_set) return OS64_CONF_NO_FILE;
    if (strlen(configured_set) >= cap) return OS64_CONF_TRUNCATED;
    strcpy(out, configured_set);
    return 0;
}
os64_slurp_status_t os64_slurp(const char *path, size_t cap,
                               uint8_t **out, size_t *length)
{
    (void)cap;
    *out = NULL; *length = 0;
    if (fault_root && strstr(path, fault_root) && read_fault)
        return read_fault;
    const char *data = strstr(path, "/quaint/") ? selected_manifest :
                       strstr(path, "/default/") ?
                       "format = 1\nname = Default\nauthor = os64\n" : NULL;
    if (!data) return OS64_SLURP_NO_FILE;
    *length = strlen(data);
    *out = malloc(*length + 1);
    memcpy(*out, data, *length + 1);
    return OS64_SLURP_OK;
}
int64_t os64_conf_parse(const char *text, size_t length, os64_conf_fn fn, void *user)
{
    if (parse_fault && fault_root &&
        ((strstr(fault_root, "quaint") && strstr(text, "Quaint")) ||
         (strstr(fault_root, "default") && strstr(text, "Default"))))
        return parse_fault;
    char *copy = malloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = 0;
    int64_t count = 0;
    for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) { fn(NULL, line, user); continue; }
        *eq++ = 0;
        while (*eq == ' ') ++eq;
        char *end = line + strlen(line);
        while (end > line && end[-1] == ' ') *--end = 0;
        fn(line, eq, user);
        ++count;
    }
    free(copy);
    return count;
}
int64_t os64_stat(const char *path, os64_dirent_t *out)
{
    (void)out;
    return strstr(path, "/default/") ||
           (strstr(path, "/quaint/") && selected_manifest) ? 0 : -1;
}

static void transient_creation_failures(void)
{
    const char *roots[] = {"/quaint/", "/default/"};
    const os64_slurp_status_t failures[] = {
        OS64_SLURP_NO_FILE, OS64_SLURP_IO_ERROR, OS64_SLURP_NO_MEMORY
    };
    for (size_t r = 0; r < 2; ++r) {
        fault_root = roots[r];
        for (size_t i = 0; i < 3; ++i) {
            os64_icon_theme_t *theme = (void *)1;
            read_fault = failures[i];
            os64_ui_status_code_t want = failures[i] == OS64_SLURP_NO_MEMORY ?
                OS64_UI_NO_MEMORY : OS64_UI_ERROR;
            assert(os64_icon_theme_create(&theme) == want && !theme);
            read_fault = OS64_SLURP_OK;
            assert(os64_icon_theme_create(&theme) == OS64_UI_OK);
            assert(!strcmp(os64_icon_theme_name(theme), "Quaint"));
            assert(os64_icon_theme_destroy(theme) == OS64_UI_OK);
        }
        os64_icon_theme_t *theme = (void *)1;
        parse_fault = OS64_CONF_NO_MEMORY;
        assert(os64_icon_theme_create(&theme) == OS64_UI_NO_MEMORY && !theme);
        parse_fault = 0;
        assert(os64_icon_theme_create(&theme) == OS64_UI_OK);
        assert(!strcmp(os64_icon_theme_name(theme), "Quaint"));
        assert(os64_icon_theme_destroy(theme) == OS64_UI_OK);
    }
    fault_root = NULL;
    const int64_t failures_config[] = {OS64_CONF_NO_MEMORY, OS64_CONF_IO_ERROR};
    for (size_t i = 0; i < 2; ++i) {
        os64_icon_theme_t *theme = (void *)1;
        config_fault = failures_config[i];
        assert(os64_icon_theme_create(&theme) ==
               (i ? OS64_UI_ERROR : OS64_UI_NO_MEMORY) && !theme);
        config_fault = 0;
        assert(os64_icon_theme_create(&theme) == OS64_UI_OK);
        assert(!strcmp(os64_icon_theme_name(theme), "Quaint"));
        assert(os64_icon_theme_destroy(theme) == OS64_UI_OK);
    }
}

static os64_ui_icon_status_t acquire(void *user, const char *key, uint32_t size,
                                      os64_ui_icon_lease_t *out)
{
    struct os64_icon_catalog *c = user;
    assert(size == 16);
    if (c->selected) ++selected_calls; else ++default_calls;
    if (!strcmp(key, "missing") || (c->selected && !strcmp(key, "default-only")))
        return OS64_UI_ICON_MISSING;
    if (c->selected && !strcmp(key, "bad")) return OS64_UI_ICON_MALFORMED;
    ++c->leases;
    *out = (os64_ui_icon_lease_t){&pixel, 16, 16, 16, c->generation, c};
    return OS64_UI_ICON_OK;
}
static void release(void *user, os64_ui_icon_lease_t *lease)
{
    struct os64_icon_catalog *c = user;
    assert(lease->cookie == c && c->leases);
    --c->leases;
    ++releases;
    *lease = (os64_ui_icon_lease_t){0};
}
os64_ui_status_code_t os64_icon_catalog_create(const char *root, uint64_t generation,
                                                os64_icon_catalog_t **out)
{
    struct os64_icon_catalog *c = calloc(1, sizeof(*c));
    assert(root[0] == '/');
    c->selected = strstr(root, "quaint") != NULL;
    c->generation = generation;
    c->provider = (os64_ui_icon_provider_t){c, acquire, release};
    *out = c;
    return OS64_UI_OK;
}
const os64_ui_icon_provider_t *os64_icon_catalog_provider(os64_icon_catalog_t *c)
{ return &c->provider; }
os64_ui_status_code_t os64_icon_catalog_destroy(os64_icon_catalog_t *c)
{ if (c->leases) return OS64_UI_BUSY; free(c); return OS64_UI_OK; }

int main(void)
{
    configured_set = "/home/icons/quaint";
    transient_creation_failures();
    os64_icon_theme_t *theme = NULL;
    assert(os64_icon_theme_create(&theme) == OS64_UI_OK);
    assert(!strcmp(os64_icon_theme_path(theme), configured_set));
    assert(!strcmp(os64_icon_theme_name(theme), "Quaint"));
    assert(!strcmp(os64_icon_theme_author(theme), "Fixture author"));
    const os64_ui_icon_provider_t *provider = os64_icon_theme_provider(theme);
    os64_ui_icon_lease_t selected = {0}, fallback = {0};
    assert(provider->acquire(provider->user, "selected", 16, &selected) == OS64_UI_ICON_OK);
    assert(provider->acquire(provider->user, "default-only", 16, &fallback) == OS64_UI_ICON_OK);
    assert(selected.generation == 2 && fallback.generation == 1);
    assert(os64_icon_theme_destroy(theme) == OS64_UI_BUSY);
    assert(provider->acquire(provider->user, "bad", 16, &(os64_ui_icon_lease_t){0}) == OS64_UI_ICON_MALFORMED);
    assert(default_calls == 1);
    os64_ui_icon_lease_t missing = {0};
    assert(provider->acquire(provider->user, "missing", 16, &missing) == OS64_UI_ICON_MISSING);
    assert(!missing.cookie && default_calls == 2);
    provider->release(provider->user, &selected);
    provider->release(provider->user, &fallback);
    assert(releases == 2 && selected_calls == 4);
    /* The wrapper's route records are bounded even if a catalog can pin
     * the same pixels many times. Refusal must unpin its inner lease. */
    os64_ui_icon_lease_t many[1024] = {0};
    size_t count = 0;
    while (count < 1024 &&
           provider->acquire(provider->user, "selected", 16, &many[count]) == OS64_UI_ICON_OK)
        ++count;
    assert(count >= 500 && count < 1024);
    os64_ui_icon_lease_t refused = {0};
    assert(provider->acquire(provider->user, "selected", 16, &refused) == OS64_UI_ICON_LIMIT);
    assert(!refused.cookie);
    assert(os64_icon_theme_destroy(theme) == OS64_UI_BUSY);
    provider->release(provider->user, &many[0]);
    assert(provider->acquire(provider->user, "selected", 16, &many[0]) == OS64_UI_ICON_OK);
    for (size_t i = 0; i < count; ++i)
        provider->release(provider->user, &many[i]);
    assert(os64_icon_theme_destroy(theme) == OS64_UI_OK);

    selected_manifest = "format = 2\nname = Quaint\nauthor = Fixture author\n";
    assert(os64_icon_theme_create(&theme) == OS64_UI_OK);
    assert(!strcmp(os64_icon_theme_path(theme), "/etc/icons/default"));
    assert(os64_icon_theme_destroy(theme) == OS64_UI_OK);
    configured_set = NULL;
    assert(os64_icon_theme_create(&theme) == OS64_UI_OK);
    assert(!strcmp(os64_icon_theme_path(theme), "/etc/icons/default"));
    assert(os64_icon_theme_destroy(theme) == OS64_UI_OK);
    puts("icon theme resolver: OK");
    return 0;
}
