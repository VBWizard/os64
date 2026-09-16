#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/ui.h"

static char root[256];
static bool bad_read, bad_write, bad_sync, no_atomic, fail_alloc;
static DIR *dirs[8];
void *os64_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }
void os64_free(void *p) { free(p); }
uint64_t os64_taskid(void) { return (uint64_t)getpid(); }
int os64_ui_theme_preserve_session(void) { return 0; }
uint64_t os64_syscall6(uint64_t nr, uint64_t name, uint64_t out, uint64_t cap,
                      uint64_t from, uint64_t any, uint64_t unused)
{
    (void)unused;
    assert(nr == SYSCALL_CONF_RESOLVE);
    if (from) return (uint64_t)-1;
    int n = snprintf((char *)out, (size_t)cap, "%s/%s", root, (char *)name);
    if (n < 0 || (uint64_t)n >= cap) return (uint64_t)-1;
    return any || access((char *)out, F_OK) == 0 ? 1 : (uint64_t)-1;
}
int64_t os64_open(const char *path, const char *mode)
{ return open(path, *mode == 'w' ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0600); }
int64_t os64_read(int32_t fd, void *p, size_t n)
{ return bad_read ? -1 : read(fd, p, n > 17 ? 17 : n); }
int64_t os64_write(int32_t fd, const void *p, size_t n)
{ return bad_write ? -1 : write(fd, p, n > 23 ? 23 : n); }
int64_t os64_sync(int32_t fd) { return bad_sync ? -1 : fsync(fd); }
int64_t os64_close(int32_t fd)
{
    if (fd >= 10000) { int i = fd - 10000; int rc = closedir(dirs[i]); dirs[i] = NULL; return rc; }
    return close(fd);
}
int64_t os64_mkdir(const char *p) { return mkdir(p, 0700); }
int64_t os64_unlink(const char *p) { return unlink(p); }
int64_t os64_stat(const char *p, os64_dirent_t *entry)
{
    struct stat st;
    if (stat(p, &st)) return -1;
    memset(entry, 0, sizeof(*entry));
    entry->size = (uint64_t)st.st_size;
    entry->flags = S_ISDIR(st.st_mode) ? OS64_DE_DIR : 0;
    return 0;
}
int64_t os64_opendir(const char *p)
{
    for (int i = 0; i < 8; ++i) if (!dirs[i]) {
        dirs[i] = opendir(p); return dirs[i] ? i + 10000 : -1;
    }
    abort();
}
int64_t os64_readdir(int32_t fd, os64_dirent_t *entry)
{
    struct dirent *de;
    errno = 0;
    while ((de = readdir(dirs[fd - 10000]))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        memset(entry, 0, sizeof(*entry));
        strcpy(entry->name, de->d_name);
        entry->flags = de->d_type == DT_DIR ? OS64_DE_DIR : 0;
        return 1;
    }
    return errno ? -1 : 0;
}
int64_t os64_rename(const char *a, const char *b) { return rename(a, b); }
int64_t os64_rename_with_flags(const char *a, const char *b, uint64_t flags)
{
    if (flags == OS64_RENAME_NOREPLACE) {
        if (link(a, b)) return -1;
        return unlink(a);
    }
    assert(flags == OS64_RENAME_REQUIRE_ATOMIC_REPLACE);
    return no_atomic ? -1 : rename(a, b);
}
static void raw(const char *relative, const void *bytes, size_t n)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s", root, relative);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, bytes, n) == (ssize_t)n && close(fd) == 0);
}
static void unchanged(const os64_ui_theme_t *a, const os64_ui_theme_t *b)
{ assert(memcmp(a, b, sizeof(*a)) == 0); }
struct race { const os64_ui_theme_t *theme; int result; };
static void *save_racer(void *p)
{
    struct race *r = p; r->result = os64_ui_theme_save("Concurrent", r->theme, false); return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 2 && strlen(argv[1]) < sizeof(root)); strcpy(root, argv[1]);
    os64_ui_theme_entry_t entries[8];
    assert(os64_ui_theme_list(entries, 8) == 0);
    const char *bad[] = {"", "../escape", "a/b", "a#b", "a=b", " white", "white ", "a\nb", "."};
    os64_ui_theme_t electric, paper, loaded, sentinel;
    os64_ui_theme_defaults(&electric); os64_ui_theme_palette(&electric, OS64_UI_PALETTE_ELECTRIC);
    electric.button_bevel = 3; electric.pad = 13; electric.control_radius = 6;
    paper = electric; os64_ui_theme_palette(&paper, OS64_UI_PALETTE_PAPER);
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        assert(os64_ui_theme_save(bad[i], &electric, false) == OS64_UI_THEME_INVALID);
    assert(os64_ui_theme_save("Electric Workstation", &electric, false) == 0);
    assert(os64_ui_theme_save("Electric Workstation", &paper, false) == OS64_UI_THEME_EXISTS);
    assert(os64_ui_theme_load("Electric Workstation", &loaded) == 0); unchanged(&loaded, &electric);
    for (int failure = 0; failure < 3; ++failure) {
        bad_write = failure == 0; bad_sync = failure == 1; no_atomic = failure == 2;
        assert(os64_ui_theme_save("Electric Workstation", &paper, true) == OS64_UI_THEME_IO);
        bad_write = bad_sync = no_atomic = false;
        assert(os64_ui_theme_load("Electric Workstation", &loaded) == 0); unchanged(&loaded, &electric);
    }
    assert(os64_ui_theme_save("Electric Workstation", &paper, true) == 0);
    assert(os64_ui_theme_load("Electric Workstation", &loaded) == 0); unchanged(&loaded, &paper);
    sentinel = electric; loaded = sentinel;
    bad_read = true; assert(os64_ui_theme_load("Electric Workstation", &loaded) == OS64_UI_THEME_IO);
    bad_read = false; unchanged(&loaded, &sentinel);
    fail_alloc = true; assert(os64_ui_theme_load("Electric Workstation", &loaded) == OS64_UI_THEME_IO);
    fail_alloc = false; unchanged(&loaded, &sentinel);
    raw("themes/Partial.theme", "panel.bg = 112233\n", 18);
    assert(os64_ui_theme_load("Partial", &loaded) == OS64_UI_THEME_INVALID); unchanged(&loaded, &sentinel);
    char huge[4097]; memset(huge, 'a', sizeof(huge)); raw("themes/Huge.theme", huge, sizeof(huge));
    assert(os64_ui_theme_load("Huge", &loaded) == OS64_UI_THEME_INVALID); unchanged(&loaded, &sentinel);
    assert(os64_ui_theme_list(entries, 1) == OS64_UI_THEME_LIMIT);
    assert(os64_ui_theme_list(entries, 8) == 3);
    assert(!strcmp(entries[0].name, "Electric Workstation") && !strcmp(entries[2].name, "Partial"));
    struct race racers[2] = {{&electric, 0}, {&paper, 0}};
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i) assert(!pthread_create(&threads[i], NULL, save_racer, &racers[i]));
    for (int i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
    assert((racers[0].result == 0) != (racers[1].result == 0));
    assert(racers[0].result == OS64_UI_THEME_EXISTS || racers[1].result == OS64_UI_THEME_EXISTS);
    assert(os64_ui_theme_load("Concurrent", &loaded) == 0);
    unchanged(&loaded, racers[0].result == 0 ? &electric : &paper);
    raw("theme.conf", "# Keep my comment\n", 18);
    assert(os64_ui_theme_set_startup(&electric) == 0);
    os64_ui_theme_startup(&loaded); unchanged(&loaded, &electric);
    no_atomic = true; assert(os64_ui_theme_set_startup(&paper) < 0); no_atomic = false;
    os64_ui_theme_startup(&loaded); unchanged(&loaded, &electric);
    bad_read = true; assert(os64_ui_theme_set_startup(&paper) < 0); bad_read = false;
    os64_ui_theme_startup(&loaded); unchanged(&loaded, &electric);
    assert(os64_ui_theme_set_startup(&paper) == 0);
    os64_ui_theme_startup(&loaded); unchanged(&loaded, &paper);
    char path[512], bytes[4096]; snprintf(path, sizeof(path), "%s/theme.conf", root);
    int fd = open(path, O_RDONLY); ssize_t n = read(fd, bytes, sizeof(bytes)); close(fd);
    assert(n > 18 && !memcmp(bytes, "# Keep my comment\n", 18));
    // Unknown/malformed preserved lines cannot produce a successful but
    // unreadable startup choice; validation runs over the merged output.
    raw("theme.conf", "unknown = 4\n", 12);
    assert(os64_ui_theme_set_startup(&electric) == OS64_CONF_BAD_SETTING);
    fd = open(path, O_RDONLY); n = read(fd, bytes, sizeof(bytes)); close(fd);
    assert(n == 12 && !memcmp(bytes, "unknown = 4\n", 12));
    os64_conf_pair_t pairs[OS64_CONF_WRITE_MAX + 1];
    assert(os64_conf_write("unused.conf", pairs, OS64_CONF_WRITE_MAX + 1) == OS64_CONF_TOO_MANY);
    puts("appearance saved: round trips, full schema, names, I/O failures, atomic publication, concurrent create, startup merge passed");
}
