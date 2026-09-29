/* Run the production command/parser against a host filesystem, with injected
 * create/stat and output failures at the syscall boundary. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define main mkdir_main
#include "../userland/apps/mkdir/mkdir.c"
#undef main

static char output[8192], errors[8192];
static size_t output_length, error_length, write_limit;
static unsigned creates, stats;
static const char *denied, *raced;
static bool fail_output, zero_output, fail_stat;

int64_t os64_mkdir(const char *path)
{
    assert(strlen(path) < OS64_PATH_MAX);
    creates++;
    if (denied && strcmp(path, denied) == 0) return -1;
    if (raced && strcmp(path, raced) == 0)
    {
        assert(mkdir(path, 0700) == 0);
        raced = NULL;
        return -1;
    }
    return mkdir(path, 0700);
}

int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    struct stat s;
    stats++;
    if (fail_stat || stat(path, &s) != 0) return -1;
    memset(entry, 0, sizeof(*entry));
    if (S_ISDIR(s.st_mode)) entry->flags = OS64_DE_DIR;
    return 0;
}

int64_t os64_write(int32_t handle, const void *data, size_t size)
{
    assert(handle == OS64_STDOUT || handle == OS64_STDERR);
    if (handle == OS64_STDOUT && fail_output) return -1;
    if (handle == OS64_STDOUT && zero_output) return 0;
    if (handle == OS64_STDOUT && write_limit && size > write_limit) size = write_limit;
    char *buffer = handle == OS64_STDOUT ? output : errors;
    size_t *used = handle == OS64_STDOUT ? &output_length : &error_length;
    assert(*used + size < sizeof(output));
    memcpy(buffer + *used, data, size);
    *used += size;
    buffer[*used] = 0;
    return (int64_t)size;
}

static bool directory(const char *path)
{
    struct stat s;
    return stat(path, &s) == 0 && S_ISDIR(s.st_mode);
}

static void capture(void)
{
    output_length = error_length = 0;
    output[0] = errors[0] = 0;
    creates = stats = 0;
}

#define RUN(expected, ...) do { \
    char *argv[] = {"mkdir", __VA_ARGS__}; \
    capture(); \
    assert(mkdir_main(sizeof(argv) / sizeof(argv[0]), argv) == (expected)); \
} while (0)

int main(int argc, char **argv)
{
    assert(argc == 2 && chdir(argv[1]) == 0);
    RUN(0, "plain"); assert(directory("plain") && !output[0]);
    RUN(1, "plain"); assert(strstr(errors, "cannot create") && stats == 0);
    RUN(1, "missing/child"); assert(!directory("missing"));
    RUN(0, "-v", "verbose");
    assert(strcmp(output, "mkdir: created directory 'verbose'\n") == 0);
    RUN(0, "-pv", "tree/branch/leaf");
    assert(directory("tree/branch/leaf"));
    assert(strcmp(output, "mkdir: created directory 'tree'\n"
                          "mkdir: created directory 'tree/branch'\n"
                          "mkdir: created directory 'tree/branch/leaf'\n") == 0);
    RUN(0, "--parents", "--verbose", "tree/branch/leaf"); assert(!output[0]);
    RUN(0, "tree/branch/other", "-vp");
    assert(strcmp(output, "mkdir: created directory 'tree/branch/other'\n") == 0);
    RUN(0, "-p", "repeat//child///"); assert(directory("repeat/child"));
    RUN(0, "-pv", "/", "///", ".", ".."); assert(!output[0]);
    RUN(0, "-p", "./dots/./child/../sibling");
    assert(directory("dots/child") && directory("dots/sibling"));
    RUN(0, "-p", "created-before-dotdot/../after-dotdot");
    assert(directory("created-before-dotdot") && directory("after-dotdot"));

    FILE *file = fopen("file", "w"); assert(file && fclose(file) == 0);
    RUN(1, "-p", "file");
    RUN(1, "-p", "file/child");
    RUN(1, "-p", "file/../escaped"); assert(!directory("escaped"));
    RUN(1, "-p", "file/.");
    RUN(1, "-pv", "file/child", "after-error/child");
    assert(directory("after-error/child") && strstr(errors, "'file'"));
    assert(!strstr(output, "'file"));

    RUN(0, "--", "-p", "-"); assert(directory("-p") && directory("-"));
    RUN(0, "-p", "--", "-v/child"); assert(directory("-v/child") && !output[0]);
    RUN(1, "no-side-effect", "--bad"); assert(creates == 0 && stats == 0);
    RUN(1, "-pvx", "no-side-effect"); assert(creates == 0);
    RUN(1, "--parents=yes", "no-side-effect"); assert(creates == 0);
    RUN(0, "no-side-effect", "--help");
    assert(creates == 0 && strstr(output, "--parents") && strstr(output, "--verbose"));
    assert(!directory("no-side-effect"));
    RUN(1); assert(creates == 0 && strstr(errors, "missing directory"));
    RUN(1, "-pv"); assert(creates == 0);
    RUN(1, "-p", ""); assert(creates == 0 && stats == 0);

    char path[OS64_PATH_MAX + 1];
    int prefix = snprintf(path, sizeof(path), "%s/limit-", argv[1]);
    assert(prefix > 0 && (size_t)prefix < sizeof(path) - 1);
    memset(path + prefix, 'x', OS64_PATH_MAX - 1 - (size_t)prefix);
    path[OS64_PATH_MAX - 1] = 0;
    RUN(0, "-pv", path); assert(directory(path));
    char expected[OS64_PATH_MAX + 40];
    snprintf(expected, sizeof(expected), "mkdir: created directory '%s'\n", path);
    assert(strcmp(output, expected) == 0);
    memcpy(path, "too-long/", 9);
    memset(path + 9, 'x', sizeof(path) - 10);
    path[sizeof(path) - 1] = 0;
    RUN(1, "-p", path); assert(creates == 0 && !directory("too-long"));
    RUN(1, "-v", path); assert(creates == 0);

    denied = "blocked";
    RUN(1, "-pv", "blocked/child"); assert(creates == 1 && !output[0]);
    denied = NULL;
    raced = "race";
    RUN(0, "-pv", "race/child");
    assert(directory("race/child"));
    assert(strcmp(output, "mkdir: created directory 'race/child'\n") == 0);
    fail_stat = true;
    RUN(1, "-p", "plain/child"); assert(!directory("plain/child"));
    fail_stat = false;

    write_limit = 3;
    RUN(0, "-v", "short-output");
    assert(strcmp(output, "mkdir: created directory 'short-output'\n") == 0);
    write_limit = 0;
    fail_output = true;
    RUN(1, "-v", "failed-output"); assert(directory("failed-output"));
    fail_output = false;
    zero_output = true;
    RUN(1, "-v", "zero-output"); assert(directory("zero-output"));
    zero_output = false;
    puts("mkdir: parents, verbosity, parsing, path bounds, races and failures PASS");
    return 0;
}
