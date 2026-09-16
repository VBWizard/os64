// Personal named snapshots. The config resolver owns the directory choice;
// filenames and bounded, complete theme contents belong to this collection.
#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/ui.h"
#include "ui_internal.h"

#define SNAPSHOT_MAX 4096

bool os64_ui_theme_name_valid(const char *name)
{
    if (!name || !name[0] || name[0] == ' ') return false;
    size_t n = 0;
    for (; name[n]; ++n) {
        unsigned char c = (unsigned char)name[n];
        if (n == OS64_UI_THEME_NAME_MAX) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
              c == '_' || c == '&' || c == '(' || c == ')')) return false;
    }
    return name[n - 1] != ' ';
}

static int collection(char *path, size_t cap)
{
    return os64_conf_target("themes", path, cap) == 0 ? 0 : OS64_UI_THEME_IO;
}

static int theme_path(const char *name, char *path, size_t cap)
{
    if (!os64_ui_theme_name_valid(name)) return OS64_UI_THEME_INVALID;
    char dir[OS64_CONF_PATH_MAX];
    if (collection(dir, sizeof(dir))) return OS64_UI_THEME_IO;
    int n = os64_snprintf(path, cap, "%s/%s.theme", dir, name);
    return n > 0 && (size_t)n < cap ? 0 : OS64_UI_THEME_INVALID;
}

static bool name_after(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a > (unsigned char)*b;
}

int os64_ui_theme_list(os64_ui_theme_entry_t *entries, size_t cap)
{
    if (!entries || !cap) return OS64_UI_THEME_INVALID;
    char dir[OS64_CONF_PATH_MAX];
    if (collection(dir, sizeof(dir))) return OS64_UI_THEME_IO;
    // Creating the personal directory is harmless if another Workshop won.
    os64_mkdir(dir);
    int64_t fd = os64_opendir(dir);
    if (fd < 0) return OS64_UI_THEME_IO;
    size_t count = 0;
    os64_dirent_t entry;
    int64_t rc;
    while ((rc = os64_readdir((int32_t)fd, &entry)) == 1) {
        if (entry.flags & OS64_DE_DIR) continue;
        size_t n = os64_strlen(entry.name);
        if (n <= 6 || n > OS64_UI_THEME_NAME_MAX + 6 ||
            !os64_streq(entry.name + n - 6, ".theme")) continue;
        entry.name[n - 6] = 0;
        if (!os64_ui_theme_name_valid(entry.name)) continue;
        if (count == cap) { rc = OS64_UI_THEME_LIMIT; break; }
        os64_strcopy(entries[count++].name, sizeof(entries[0].name), entry.name);
    }
    os64_close((int32_t)fd);
    if (rc < 0) return rc == OS64_UI_THEME_LIMIT ? (int)rc : OS64_UI_THEME_IO;
    for (size_t i = 1; i < count; ++i) {
        os64_ui_theme_entry_t value = entries[i];
        size_t j = i;
        while (j && name_after(entries[j - 1].name, value.name)) {
            entries[j] = entries[j - 1]; --j;
        }
        entries[j] = value;
    }
    return (int)count;
}

int os64_ui_theme_load(const char *name, os64_ui_theme_t *theme)
{
    char path[OS64_CONF_PATH_MAX], text[SNAPSHOT_MAX + 1];
    int result = theme_path(name, path, sizeof(path));
    if (result) return result;
    int64_t fd = os64_open(path, "r");
    if (fd < 0) return OS64_UI_THEME_IO;
    size_t got = 0;
    for (;;) {
        int64_t n = os64_read((int32_t)fd, text + got, sizeof(text) - got);
        if (n == OS64_INTERRUPTED) continue;
        if (n < 0) { result = OS64_UI_THEME_IO; break; }
        if (!n) break;
        got += (size_t)n;
        if (got == sizeof(text)) { result = OS64_UI_THEME_INVALID; break; }
    }
    os64_close((int32_t)fd);
    os64_ui_theme_t candidate;
    os64_ui_theme_defaults(&candidate);
    // A saved snapshot is complete, unlike a hand-written startup override.
    // Requiring every schema key is handled by the shared snapshot decoder.
    if (!result) {
        int64_t parsed = os64_ui_theme_parse_saved_status(&candidate, text, got);
        if (parsed < 0) result = parsed == OS64_CONF_NO_MEMORY ?
            OS64_UI_THEME_IO : OS64_UI_THEME_INVALID;
    }
    if (!result) *theme = candidate;
    return result;
}

int os64_ui_theme_save(const char *name, const os64_ui_theme_t *theme, bool replace)
{
    char dir[OS64_CONF_PATH_MAX], path[OS64_CONF_PATH_MAX];
    char temp[OS64_CONF_PATH_MAX], text[SNAPSHOT_MAX];
    int result = theme_path(name, path, sizeof(path));
    if (result) return result;
    int64_t len = os64_ui_theme_encode(theme, text, sizeof(text));
    if (len < 0) return OS64_UI_THEME_INVALID;
    if (collection(dir, sizeof(dir))) return OS64_UI_THEME_IO;
    os64_mkdir(dir);
    static uint64_t sequence;
    uint64_t seq = __atomic_fetch_add(&sequence, 1, __ATOMIC_RELAXED);
    int n = os64_snprintf(temp, sizeof(temp), "%s/.%lu.%lu.new", dir,
                          (unsigned long)os64_taskid(), (unsigned long)seq);
    if (n < 0 || (size_t)n >= sizeof(temp)) return OS64_UI_THEME_IO;
    int64_t fd = os64_open(temp, "w");
    if (fd < 0) return OS64_UI_THEME_IO;
    size_t put = 0;
    while (put < (size_t)len) {
        int64_t wrote = os64_write((int32_t)fd, text + put, (size_t)len - put);
        if (wrote == OS64_INTERRUPTED) continue;
        if (wrote <= 0) { result = OS64_UI_THEME_IO; break; }
        put += (size_t)wrote;
    }
    if (!result && os64_sync((int32_t)fd) != 0) result = OS64_UI_THEME_IO;
    os64_close((int32_t)fd);
    if (!result && os64_rename_with_flags(temp, path, replace ?
        OS64_RENAME_REQUIRE_ATOMIC_REPLACE : OS64_RENAME_NOREPLACE) != 0) {
        os64_dirent_t entry;
        result = !replace && os64_stat(path, &entry) == 0 ?
            OS64_UI_THEME_EXISTS : OS64_UI_THEME_IO;
    }
    if (result) os64_unlink(temp);
    return result;
}
