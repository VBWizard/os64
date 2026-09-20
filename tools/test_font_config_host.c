#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "os64/font_config.h"
#include "os64/conf.h"
#include "os64/io.h"

static size_t checks, calls, deny, live, opens;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef union { max_align_t alignment; size_t bytes; } allocation;
void *os64_malloc(size_t n)
{
    if (++calls == deny) return NULL;
    allocation *p = malloc(sizeof(*p) + n);
    CHECK(p != NULL); p->bytes = n; live += n; return p + 1;
}
void os64_free(void *v)
{
    if (!v) return;
    allocation *p = (allocation *)v - 1;
    CHECK(live >= p->bytes); live -= p->bytes; free(p);
}
static void *allocate(void *u, size_t n) { (void)u; return os64_malloc(n); }
static void release(void *u, void *p, size_t n) { (void)u; (void)n; os64_free(p); }
int os64_snprintf(char *p, size_t n, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); int result = vsnprintf(p, n, fmt, args);
    va_end(args); return result;
}

typedef struct { const char *path; unsigned char *data; size_t length, at; } file;
static file files[10];
static bool config_found, fail_read, fail_open;
static int64_t advertised = -1;
static size_t directory_at, directory_count = 3;
static bool missing_directory, multi_directory;
static size_t directory_kind, total_entries;
int64_t __wrap_os64_conf_target(const char *name, char *out, size_t cap)
{ CHECK(!strcmp(name,"fonts.conf") && cap > 15); strcpy(out,"/cfg/fonts.conf"); return 0; }
int64_t os64_opendir(const char *path)
{
    directory_at = 0;
    if (missing_directory) return -1;
    if (!strcmp(path,"/cfg/fonts")) directory_kind = 0;
    else if (multi_directory && !strcmp(path,"/home/fonts")) directory_kind = 1;
    else if (multi_directory && !strcmp(path,"/etc/fonts")) directory_kind = 2;
    else return -1;
    return 30;
}
int64_t os64_readdir(int32_t h, os64_dirent_t *e)
{
    CHECK(h == 30);
    size_t count = directory_kind ? 2 : directory_count;
    if (directory_at == count) return 0;
    *e = (os64_dirent_t){0};
    const char *names[] = {"sans", "mono", "bad"};
    const char *personal[] = {"copy", "revision"};
    const char *system[] = {"sans", "extra"};
    if (directory_kind) {
        strcpy(e->name, directory_kind == 1 ? personal[directory_at] : system[directory_at]);
        ++directory_at; ++total_entries; return 1;
    }
    if (directory_at < 3) strcpy(e->name,names[directory_at]);
    else { strcpy(e->name,"folder"); e->flags = OS64_DE_DIR; }
    ++directory_at; ++total_entries; return 1;
}
int64_t __wrap_os64_conf_find(const char *name, char *out, size_t cap)
{
    CHECK(!strcmp(name, "fonts.conf"));
    if (!config_found) return OS64_CONF_NO_FILE;
    CHECK(cap > strlen("/cfg/fonts.conf")); strcpy(out, "/cfg/fonts.conf"); return 0;
}
int64_t os64_open(const char *path, const char *mode)
{
    CHECK(!strcmp(mode, "r")); ++opens;
    if (fail_open) return -1;
    for (size_t i = 0; i < 10; ++i) if (files[i].path && !strcmp(files[i].path, path)) {
        files[i].at = 0; return (int64_t)i + 10;
    }
    return -1;
}
int64_t os64_close(int32_t h) { CHECK((h >= 10 && h < 20) || h == 30); return 0; }
int64_t os64_read(int32_t h, void *out, size_t n)
{
    CHECK(h >= 10 && h < 20);
    file *f = &files[h - 10];
    if (fail_read && f->at) return -1;
    if (n > 32768) n = 32768; /* exercise legal short reads */
    if (n > f->length - f->at) n = f->length - f->at;
    memcpy(out, f->data + f->at, n); f->at += n; return (int64_t)n;
}
int64_t os64_stat(const char *path, os64_dirent_t *e)
{
    for (size_t i = 0; i < 10; ++i) if (files[i].path && !strcmp(files[i].path, path)) {
        *e = (os64_dirent_t){.size = advertised >= 0 ? (uint64_t)advertised : files[i].length}; return 0;
    }
    return -1;
}
static void load(file *f, const char *root, const char *name, const char *path)
{
    char full[1024]; snprintf(full, sizeof(full), "%s/%s", root, name);
    FILE *in = fopen(full, "rb"); CHECK(in != NULL);
    CHECK(fseek(in, 0, SEEK_END) == 0); long n = ftell(in); CHECK(n > 0);
    rewind(in); f->data = malloc((size_t)n); CHECK(f->data);
    CHECK(fread(f->data, 1, (size_t)n, in) == (size_t)n); fclose(in);
    f->length = (size_t)n; f->path = path;
}
static os64_font_config_status_t decode(const char *s, os64_font_config_t *c,
                                       os64_font_config_error_t *e)
{ return os64_font_config_decode(s, strlen(s), "/cfg/fonts.conf", c, e); }
static const char choices[] =
    "# role choices\nUI.face = fonts/sans\nui.size = 18\n"
    "terminal.face = fonts/mono\nterminal.size = 20\n"
    "document.face = fonts/sans\ndocument.size = 24\n";

static void parsing(void)
{
    os64_font_config_t c, saved;
    os64_font_config_error_t e;
    CHECK(decode("", &c, &e) == 0);
    CHECK(!strcmp(c.roles[2].face[0], "builtin") && c.roles[0].size == 16);
    CHECK(decode(choices, &c, &e) == 0);
    CHECK(!strcmp(c.roles[0].face[0], "/cfg/fonts/sans"));
    CHECK(c.roles[2].size == 24 && c.roles[0].source_line[0] == 2);
    saved = c;
    struct { const char *s; os64_font_config_status_t status; size_t line; } bad[] = {
        {"# comment\nui.what = 1", OS64_FONT_CONFIG_SYNTAX, 2},
        {"\n= no key", OS64_FONT_CONFIG_SYNTAX, 2},
        {"ui.face", OS64_FONT_CONFIG_SYNTAX, 1},
        {"ui.size = 17", OS64_FONT_CONFIG_SIZE, 1},
        {"ui.size = 99999999999999", OS64_FONT_CONFIG_SIZE, 1},
        {"ui.face = ../sans", OS64_FONT_CONFIG_PATH, 1},
        {"ui.face = /a/./sans", OS64_FONT_CONFIG_PATH, 1},
        {"ui.face = /a/../sans", OS64_FONT_CONFIG_PATH, 1},
        {"ui.face = ", OS64_FONT_CONFIG_PATH, 1},
        {"ui.face = /fonts/", OS64_FONT_CONFIG_PATH, 1},
        {"ui.face = /a//sans\nui.fallback.2 = /a/sans", OS64_FONT_CONFIG_DUPLICATE, 2},
        {"ui.fallback.2 = builtin", OS64_FONT_CONFIG_DUPLICATE, 1},
        {"ui.size = 7\nui.size = 16", OS64_FONT_CONFIG_SIZE, 1}
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        CHECK(decode(bad[i].s, &c, &e) == bad[i].status);
        CHECK(e.line == bad[i].line && !memcmp(&c, &saved, sizeof(c)));
    }
    CHECK(os64_font_config_decode("x\0y", 3, "/cfg/fonts.conf", &c, &e) == OS64_FONT_CONFIG_SYNTAX);
    CHECK(decode("UI.face = fonts/old\nui.face = fonts/sans\nui.size = 12\nUI.size = 18", &c, &e) == 0);
    CHECK(c.roles[0].size == 18 && c.roles[0].source_line[0] == 2);
    CHECK(!strcmp(c.roles[0].face[0], "/cfg/fonts/sans"));
    char encoded[4096];
    int64_t n = os64_font_config_encode(&c, encoded, sizeof(encoded)); CHECK(n > 0);
    os64_font_config_t moved;
    CHECK(os64_font_config_decode(encoded, (size_t)n, "/home/fonts.conf", &moved, &e) == 0);
    CHECK(!strcmp(moved.roles[0].face[0], c.roles[0].face[0]));
    char small[5] = "keep";
    CHECK(os64_font_config_encode(&c, small, sizeof(small)) < 0 && !strcmp(small,"keep"));
    char maximal[8192]; memset(maximal, ' ', sizeof(maximal)); maximal[0] = '#';
    CHECK(os64_font_config_decode(maximal, 8191, "/cfg/fonts.conf", &c, &e) == 0);
    CHECK(os64_font_config_decode(maximal, 8192, "/cfg/fonts.conf", &c, &e) == OS64_FONT_CONFIG_LIMIT);
    saved = c; deny = calls + 1;
    CHECK(decode(choices, &c, &e) == OS64_FONT_CONFIG_NO_MEMORY);
    deny = 0; CHECK(!memcmp(&c, &saved, sizeof(c)) && live == 0);
}

static void discovery(os64_text_context_t *ctx, const os64_font_config_t *c)
{
    os64_font_catalog_t *catalog = NULL;
    CHECK(os64_font_config_discover(ctx,c,&catalog) == 0);
    CHECK(catalog && catalog->count == 4 && !catalog->limited && !catalog->directory_unavailable);
    CHECK(!strcmp(catalog->entries[0].path,"builtin"));
    CHECK(!strcmp(catalog->entries[1].path,"/cfg/fonts/bad") && catalog->entries[1].status == OS64_FONT_CONFIG_FACE);
    CHECK(catalog->entries[2].info.flags & OS64_FONT_FACE_FIXED_WIDTH);
    CHECK(!strcmp(catalog->entries[3].path,"/cfg/fonts/sans") && catalog->entries[3].status == 0);
    os64_font_catalog_release(catalog);
    missing_directory = true;
    CHECK(os64_font_config_discover(ctx,c,&catalog) == 0);
    CHECK(catalog->directory_unavailable && catalog->count == 3);
    os64_font_catalog_release(catalog); missing_directory = false;
    directory_count = 300; total_entries = 0;
    CHECK(os64_font_config_discover(ctx,c,&catalog) == 0);
    CHECK(catalog->limited && total_entries == OS64_FONT_DISCOVERY_SCAN_MAX);
    os64_font_catalog_release(catalog); directory_count = 3;
    /* Saved personal settings must not hide unselected system files. Identical
     * copies collapse across names, while changed bytes with the same family
     * metadata remain separate. Both configured aliases resolve after sorting. */
    multi_directory = true;
    os64_font_config_t saved = *c;
    strcpy(saved.path, "/home/fonts.conf");
    strcpy(saved.roles[0].face[0], "/etc/fonts/sans");
    strcpy(saved.roles[2].face[0], "/home/fonts/copy");
    CHECK(os64_font_config_discover(ctx,&saved,&catalog) == 0);
    CHECK(catalog->count == 6 && !catalog->directory_unavailable);
    int first = os64_font_catalog_find(catalog,"/etc/fonts/sans");
    CHECK(first > 0 && first == os64_font_catalog_find(catalog,"/home/fonts/copy"));
    CHECK(os64_font_catalog_find(catalog,"/etc/fonts/extra") > 0);
    int revision = os64_font_catalog_find(catalog,"/home/fonts/revision");
    CHECK(revision > 0 && revision != first);
    CHECK(!strcmp(catalog->entries[first].info.family, catalog->entries[revision].info.family));
    CHECK(os64_font_catalog_find(catalog,"/absent") == -1);
    os64_font_catalog_release(catalog);
    /* Changing which config wins changes neither the union nor alias lookup. */
    strcpy(saved.path, "/etc/fonts.conf");
    CHECK(os64_font_config_discover(ctx,&saved,&catalog) == 0);
    CHECK(catalog->count == 6);
    CHECK(os64_font_catalog_find(catalog,"/etc/fonts/sans") ==
          os64_font_catalog_find(catalog,"/home/fonts/copy"));
    os64_font_catalog_release(catalog);
    directory_count = 300; total_entries = 0;
    CHECK(os64_font_config_discover(ctx,&saved,&catalog) == 0);
    CHECK(catalog->limited && total_entries == OS64_FONT_DISCOVERY_SCAN_MAX);
    os64_font_catalog_release(catalog); directory_count = 3;
    multi_directory = false;
    deny = calls + 1;
    CHECK(os64_font_config_discover(ctx,c,&catalog) == OS64_FONT_CONFIG_NO_MEMORY && !catalog);
    deny = 0;
}

static void loading(void)
{
    os64_font_config_t c, old;
    os64_font_config_error_t e;
    config_found = false;
    CHECK(os64_font_config_read(&c, &e) == 0 && !c.path[0]);
    config_found = true;
    files[0] = (file){"/cfg/fonts.conf", (unsigned char *)choices, sizeof(choices)-1, 0};
    CHECK(os64_font_config_read(&c, &e) == 0);
    old = c;
    fail_open = true; CHECK(os64_font_config_read(&c, &e) == OS64_FONT_CONFIG_IO);
    fail_open = false;
    fail_read = true; CHECK(os64_font_config_read(&c, &e) == OS64_FONT_CONFIG_IO);
    fail_read = false; CHECK(!memcmp(&c, &old, sizeof(c)));
    os64_text_options_t options = {.memory = {NULL, allocate, release}};
    os64_text_context_t *ctx = NULL;
    CHECK(os64_font_context_create(&options, &ctx) == OS64_FONT_OK);
    os64_font_set_t *first = NULL, *next = NULL;
    opens = 0;
    CHECK(os64_font_config_prepare(ctx, &c, &first, &e) == 0);
    CHECK(opens == 2); /* sans is shared across roles, bytes loaded once */
    os64_font_role_view_t a, b;
    CHECK(os64_font_set_view(first, OS64_FONT_ROLE_DOCUMENT, &a) == OS64_FONT_OK);
    CHECK(a.primary.family[0] && a.row_height_px > 16);
    CHECK(os64_font_config_prepare(ctx, &c, &next, &e) == 0);
    CHECK(os64_font_set_view(next, OS64_FONT_ROLE_DOCUMENT, &b) == OS64_FONT_OK);
    CHECK(a.identity != b.identity); /* explicit reload, never pathname identity */
    os64_font_set_release(next);
    strcpy(c.roles[1].face[0], "/cfg/fonts/sans");
    CHECK(os64_font_config_prepare(ctx, &c, &next, &e) == OS64_FONT_CONFIG_FACE);
    CHECK(!next && e.role == OS64_FONT_ROLE_TERMINAL && e.line == 4);
    c = old;
    strcpy(c.roles[0].face[2], "/cfg/fonts/bad"); c.roles[0].source_line[2] = 9;
    CHECK(os64_font_config_prepare(ctx, &c, &next, &e) == OS64_FONT_CONFIG_FACE);
    CHECK(!next && e.source == 2 && e.line == 9);
    CHECK(os64_font_set_view(first, OS64_FONT_ROLE_DOCUMENT, &b) == OS64_FONT_OK && b.identity == a.identity);
    c = old; advertised = OS64_FONT_FILE_MAX + 1;
    CHECK(os64_font_config_prepare(ctx, &c, &next, &e) == OS64_FONT_CONFIG_LIMIT && !next);
    advertised = 1;
    CHECK(os64_font_config_prepare(ctx, &c, &next, &e) == OS64_FONT_CONFIG_LIMIT && !next);
    advertised = -1;
    fail_read = true;
    CHECK(os64_font_config_prepare(ctx, &c, &next, &e) == OS64_FONT_CONFIG_IO && !next);
    fail_read = false;
    discovery(ctx, &c);
    os64_font_set_release(first);
    CHECK(os64_text_destroy(ctx) == OS64_FONT_OK && live == 0);

    /* Every allocation in the exercised preparation, refused separately.
     * A fresh context gives each iteration the same cold-cache path. */
    size_t failures = 0;
    for (size_t pos = 1; pos < 2000; ++pos) {
        CHECK(os64_font_context_create(&options, &ctx) == OS64_FONT_OK);
        deny = calls + pos;
        os64_font_config_status_t s = os64_font_config_prepare(ctx, &c, &next, &e);
        bool fired = calls >= deny; deny = 0;
        if (s) { ++failures; CHECK(next == NULL); }
        os64_font_set_release(next);
        CHECK(os64_text_destroy(ctx) == OS64_FONT_OK && live == 0);
        if (!fired) break;
        CHECK(pos < 1999);
    }
    CHECK(failures > 10);
    /* Discovery owns temporary retained source buffers in addition to provider
     * allocations. Refuse allocation positions on fresh contexts and verify
     * that releasing partial catalogs leaves nothing live. */
    size_t discovery_failures = 0;
    multi_directory = true;
    for (size_t pos = 1; pos < 4000; ++pos) {
        CHECK(os64_font_context_create(&options, &ctx) == OS64_FONT_OK);
        deny = calls + pos;
        os64_font_catalog_t *catalog = NULL;
        os64_font_config_status_t result = os64_font_config_discover(ctx, &c, &catalog);
        bool fired = calls >= deny; deny = 0;
        CHECK(result == OS64_FONT_CONFIG_OK || result == OS64_FONT_CONFIG_NO_MEMORY);
        if (fired) ++discovery_failures;
        os64_font_catalog_release(catalog);
        CHECK(os64_text_destroy(ctx) == OS64_FONT_OK && live == 0);
        if (!fired) break;
        CHECK(pos < 3999);
    }
    multi_directory = false;
    printf("Discovery allocation-denial positions: %zu\n", discovery_failures);
    printf("Preparation allocation-denial cases: %zu\n", failures);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    load(&files[1], argv[1], "DejaVuSans.ttf", "/cfg/fonts/sans");
    load(&files[2], argv[1], "DejaVuSansMono.ttf", "/cfg/fonts/mono");
    files[3] = (file){"/cfg/fonts/bad", (unsigned char *)"bad", 3, 0};
    files[4] = files[1]; files[4].path = "/etc/fonts/sans";
    files[5] = files[1]; files[5].path = "/home/fonts/copy";
    load(&files[6], argv[1], "SourceSans3-Regular.otf", "/etc/fonts/extra");
    files[7] = files[1]; files[7].path = "/home/fonts/revision";
    files[7].data = malloc(files[1].length + 1); CHECK(files[7].data);
    memcpy(files[7].data, files[1].data, files[1].length);
    files[7].data[files[1].length] = 0; ++files[7].length;
    parsing(); loading();
    free(files[6].data); free(files[7].data);
    free(files[1].data); free(files[2].data);
    printf("font_config: %zu checks, 0 failures; no live allocations\n", checks);
    return 0;
}
