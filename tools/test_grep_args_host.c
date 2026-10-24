// Drive grep's real entry point and parser against an in-memory file fixture.
// Each filename is checked in order so losing or duplicating an operand fails.
#define main grep_main
#include "../userland/apps/grep/grep.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char output[2][4096];
static size_t output_length[2];
static int files_seen, expected_files, files_closed;
static bool fail_allocation, match_last;
static size_t allocations;

static void reset(int count)
{
    memset(output, 0, sizeof(output));
    memset(output_length, 0, sizeof(output_length));
    files_seen = files_closed = 0;
    expected_files = count;
    fail_allocation = false;
    match_last = true;
    assert(allocations == 0);
}

void *os64_malloc(size_t size)
{
    if (fail_allocation) return NULL;
    void *p = malloc(size);
    if (p) allocations++;
    return p;
}
void os64_free(void *p)
{
    if (p) { assert(allocations > 0); allocations--; }
    free(p);
}
int64_t os64_write(int32_t handle, const void *data, size_t length)
{
    assert(handle == OS64_STDOUT || handle == OS64_STDERR);
    int slot = handle - 1;
    assert(output_length[slot] + length < sizeof(output[slot]));
    memcpy(output[slot] + output_length[slot], data, length);
    output_length[slot] += length;
    return (int64_t)length;
}
int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    char expected[32];
    snprintf(expected, sizeof(expected), "file%05d", files_seen++);
    assert(strcmp(path, expected) == 0);
    memset(entry, 0, sizeof(*entry));
    return 0;
}
int64_t os64_linereader_open(os64_linereader_t *reader, const char *path)
{
    (void)path;
    memset(reader, 0, sizeof(*reader));
    return 0;
}
int64_t os64_linereader_line(os64_linereader_t *reader, char *buf, size_t cap)
{
    if (reader->eof) return 0;
    reader->eof = true;
    snprintf(buf, cap, "%s", match_last && files_seen == expected_files
             ? "requestIdle" : "unrelated");
    return 1;
}
void os64_linereader_close(os64_linereader_t *reader)
{
    (void)reader;
    files_closed++;
}
int64_t os64_readline(int32_t handle, char *buf, size_t cap)
{
    assert(handle == OS64_STDIN);
    (void)buf; (void)cap;
    return 0;
}
int64_t os64_opendir(const char *path) { (void)path; abort(); }
int64_t os64_readdir(int32_t handle, os64_dirent_t *entry)
{ (void)handle; (void)entry; abort(); }
int64_t os64_close(int32_t handle) { (void)handle; abort(); }

static void many_files(int count)
{
    int argc = count + 3;
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    char (*names)[32] = calloc((size_t)count, sizeof(*names));
    char **snapshot = malloc(((size_t)argc + 1) * sizeof(*snapshot));
    assert(argv && names && snapshot);
    argv[0] = "grep";
    argv[1] = "requestIdle";
    for (int i = 0; i < count; i++) {
        snprintf(names[i], sizeof(names[i]), "file%05d", i);
        argv[i + 2] = names[i];
    }
    argv[argc - 1] = "-n"; // Options after operands must still take effect.
    memcpy(snapshot, argv, ((size_t)argc + 1) * sizeof(*snapshot));
    reset(count);
    int status = grep_main(argc, argv);
    if (count + 1 > OS64_SPAWN_MAX_ARGS) {
        assert(status == 2 && allocations == 0 && files_seen == 0);
        assert(output[0][0] == 0 && strstr(output[1], "too many operands"));
        assert(strstr(output[1], "usage:") == NULL);
        assert(memcmp(snapshot, argv, ((size_t)argc + 1) * sizeof(*snapshot)) == 0);
        free(snapshot);
        free(names);
        free(argv);
        return;
    }
    assert(status == 0);
    assert(allocations == 0 && files_seen == count && files_closed == count);
    char expected[64];
    snprintf(expected, sizeof(expected), "file%05d:1:requestIdle\n", count - 1);
    assert(strcmp(output[0], expected) == 0 && output[1][0] == 0);
    assert(memcmp(snapshot, argv, ((size_t)argc + 1) * sizeof(*snapshot)) == 0);
    reset(count);
    match_last = false;
    assert(grep_main(argc, argv) == 1);
    assert(allocations == 0 && files_seen == count && files_closed == count);
    assert(output[0][0] == 0 && output[1][0] == 0);
    free(snapshot);
    free(names);
    free(argv);
}

int main(void)
{
    const int counts[] = {512, 513, 1305, OS64_SPAWN_MAX_ARGS - 3,
                          OS64_SPAWN_MAX_ARGS - 1, OS64_SPAWN_MAX_ARGS};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
        many_files(counts[i]);
    char *help[] = {"grep", "--help", NULL};
    reset(0);
    assert(grep_main(2, help) == 0);
    assert(strstr(output[0], "usage: grep") && output[1][0] == 0);
    char *bad[] = {"grep", "requestIdle", "--bogus", NULL};
    reset(0);
    assert(grep_main(3, bad) == 2);
    assert(output[0][0] == 0 && strstr(output[1], "--bogus"));
    char *missing[] = {"grep", NULL};
    reset(0);
    assert(grep_main(1, missing) == 2);
    assert(output[0][0] == 0 && strstr(output[1], "missing pattern"));
    char *stdin_args[] = {"grep", "requestIdle", NULL};
    reset(0);
    assert(grep_main(2, stdin_args) == 1);
    reset(0);
    fail_allocation = true;
    assert(grep_main(2, stdin_args) == 2);
    assert(output[0][0] == 0 && strstr(output[1], "out of memory"));
    assert(allocations == 0 && files_seen == 0);
    puts("grep: large argument lists, matching, errors, help and allocation failure PASS");
}
