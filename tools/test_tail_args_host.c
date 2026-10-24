// Exercise tail's argument storage and follow arrays through the real main.
#define main tail_main
#include "../userland/apps/tail/tail.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int allocation_calls, fail_at, live_allocations;
static int file_count, stats, opens, closes;
static bool following;
static size_t stdout_bytes, stderr_bytes;

static void reset(int count)
{
    assert(live_allocations == 0);
    allocation_calls = fail_at = stats = opens = closes = 0;
    file_count = count;
    following = false;
    stdout_bytes = stderr_bytes = 0;
}
void *os64_malloc(size_t size)
{
    if (++allocation_calls == fail_at) return NULL;
    void *p = malloc(size);
    if (p) live_allocations++;
    return p;
}
void *os64_calloc(size_t count, size_t size)
{
    void *p = os64_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void os64_free(void *p)
{
    if (p) { assert(live_allocations > 0); live_allocations--; }
    free(p);
}
void *os64_map(size_t size) { return os64_calloc(1, size); }
int64_t os64_unmap(void *p) { os64_free(p); return 0; }
int64_t os64_write(int32_t handle, const void *data, size_t length)
{
    (void)data;
    assert(handle == OS64_STDOUT || handle == OS64_STDERR);
    if (handle == OS64_STDOUT) stdout_bytes += length;
    else stderr_bytes += length;
    return (int64_t)length;
}
int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    char expected[32];
    snprintf(expected, sizeof(expected), "file%05d", stats % file_count);
    assert(strcmp(path, expected) == 0);
    ++stats;
    memset(entry, 0, sizeof(*entry));
    // Stop following at the final operand, after visiting the full array.
    if (following && stats == 2 * file_count) return -1;
    return 0;
}
int64_t os64_open(const char *path, const char *mode)
{
    (void)path; (void)mode;
    opens++;
    return 3;
}
int64_t os64_close(int32_t handle)
{
    assert(handle == 3);
    closes++;
    return 0;
}
int64_t os64_seek(int32_t handle, int64_t offset, int32_t whence)
{
    assert(handle == 3 && offset == 0);
    (void)whence;
    return 0;
}
int64_t os64_read(int32_t handle, void *data, size_t length)
{
    assert(handle == 3 || handle == OS64_STDIN);
    (void)data; (void)length;
    return 0;
}
int64_t os64_sleep(uint64_t ms) { (void)ms; abort(); }

int main(void)
{
    const int counts[] = {513, 4094, 5000};
    for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        int count = counts[c];
        char **argv = calloc((size_t)count + 3, sizeof(*argv));
        char (*names)[32] = calloc((size_t)count, sizeof(*names));
        assert(argv && names);
        argv[0] = "tail";
        argv[1] = "-f";
        for (int i = 0; i < count; i++) {
            snprintf(names[i], sizeof(names[i]), "file%05d", i);
            argv[i + 2] = names[i];
        }
        reset(count);
        following = true;
        assert(tail_main(count + 2, argv) == 1);
        assert(stats == 2 * count && opens == count && closes == count);
        assert(live_allocations == 0 && stderr_bytes > 0);
        argv[1] = "--";
        reset(count);
        assert(tail_main(count + 2, argv) == 0);
        assert(stats == count && opens == count && closes == count);
        assert(live_allocations == 0 && stderr_bytes == 0);
        free(names);
        free(argv);
    }
    char *args[] = {"tail", "--help", NULL};
    for (int i = 1; i <= 3; i++) {
        reset(0);
        fail_at = i;
        assert(tail_main(2, args) == 2);
        assert(live_allocations == 0 && stderr_bytes > 0 && stdout_bytes == 0);
    }
    reset(0);
    assert(tail_main(2, args) == 0);
    assert(live_allocations == 0 && stderr_bytes == 0 && stdout_bytes > 0);
    args[1] = "--bogus";
    reset(0);
    assert(tail_main(2, args) == 2);
    assert(live_allocations == 0 && stderr_bytes > 0 && stdout_bytes == 0);
    char *bad_count[] = {"tail", "-n", "bad", NULL};
    reset(0);
    assert(tail_main(3, bad_count) == 2);
    assert(live_allocations == 0 && stderr_bytes > 0 && stdout_bytes == 0);
    puts("tail: large argument lists, follow state and allocation cleanup PASS");
}
