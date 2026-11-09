// Exercise the real ls reader/sorter and husk glob expansion across the old
// 512-entry boundary, with a directory that returns names in reverse order.
#define main ls_app_main
#include "../userland/apps/ls/ls.c"
#undef main
#define main husk_app_main
#include "../userland/apps/husk/husk.c"
#undef main

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int directory_count;
static int directory_next;
static int directory_closed;
static bool fail_allocation;
static char complaint[256];

int64_t os64_opendir(const char *path)
{
    assert(strcmp(path, "/fixture") == 0);
    directory_next = directory_count;
    directory_closed = 0;
    return 3;
}

int64_t os64_readdir(int32_t handle, os64_dirent_t *entry)
{
    assert(handle == 3);
    if (directory_next == 0)
        return 0;
    int n = --directory_next;
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "file%04d", n);
    entry->size = (uint64_t)n;
    entry->mtime = (uint64_t)n;
    return 1;
}

int64_t os64_close(int32_t handle)
{
    assert(handle == 3);
    directory_closed++;
    return 0;
}

int64_t os64_getcwd(char *out, size_t capacity)
{
    if (capacity < sizeof("/fixture"))
        return -1;
    memcpy(out, "/fixture", sizeof("/fixture"));
    return 0;
}

void *os64_realloc(void *ptr, size_t size)
{
    return fail_allocation ? NULL : realloc(ptr, size);
}
void os64_free(void *ptr) { free(ptr); }
size_t os64_strlen(const char *text) { return strlen(text); }
size_t os64_strcopy(char *out, size_t capacity, const char *text)
{
    size_t len = strlen(text);
    if (capacity)
    {
        size_t copied = len < capacity ? len : capacity - 1;
        memcpy(out, text, copied);
        out[copied] = 0;
    }
    return len;
}
bool os64_glob_match(const char *pattern, const char *text)
{
    (void)text;
    assert(strcmp(pattern, "*") == 0);
    return true;
}
int32_t os64_hprintf(int32_t handle, const char *format, ...)
{
    assert(handle == OS64_STDERR);
    va_list args;
    va_start(args, format);
    int n = vsnprintf(complaint, sizeof(complaint), format, args);
    va_end(args);
    return n;
}

int main(void)
{
    directory_count = 700;
    os64_dirent_t *entries = NULL;
    int32_t count = 0;
    assert(get_directory_listing("/fixture", &entries, &count) == 0);
    assert(count == 700 && directory_closed == 1);
    sort_entries(entries, count, LS_SORT_NAME);
    for (int i = 0; i < count; i++)
    {
        char expected[20];
        snprintf(expected, sizeof(expected), "file%04d", i);
        assert(strcmp(entries[i].name, expected) == 0);
    }
    sort_entries(entries, count, LS_SORT_SIZE);
    assert(entries[0].size == 699 && entries[699].size == 0);
    sort_entries(entries, count, LS_SORT_TIME);
    assert(entries[0].mtime == 699 && entries[699].mtime == 0);
    os64_free(entries);

    entries = NULL;
    count = 0;
    fail_allocation = true;
    complaint[0] = 0;
    assert(get_directory_listing("/fixture", &entries, &count) != 0);
    assert(entries == NULL && count == 0 && directory_closed == 1);
    assert(strstr(complaint, "out of memory"));
    fail_allocation = false;

    char **argv = calloc(ARGS_MAX, sizeof(*argv));
    assert(argv);
    argv[0] = "echo";
    s_glob_used = 0;
    assert(glob_expand("/fixture/*", argv, 1, ARGS_MAX) == 701);
    assert(directory_closed == 1);
    for (int i = 0; i < 700; i++)
    {
        char expected[32];
        snprintf(expected, sizeof(expected), "/fixture/file%04d", i);
        assert(strcmp(argv[i + 1], expected) == 0);
    }

    directory_count = OS64_SPAWN_MAX_ARGS - 1;
    s_glob_used = 0;
    assert(glob_expand("*", argv, 1, ARGS_MAX) == OS64_SPAWN_MAX_ARGS);
    assert(directory_closed == 1);
    assert(strcmp(argv[1], "file0000") == 0);
    directory_count++;
    s_glob_used = 0;
    complaint[0] = 0;
    assert(glob_expand("*", argv, 1, ARGS_MAX) < 0);
    assert(directory_closed == 1 && strstr(complaint, "argument limit"));
    free(argv);
    puts("large directory: ls and husk 700-entry ordering, glob count boundary PASS");
    return 0;
}
