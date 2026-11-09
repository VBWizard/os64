// test_test_host.c — HOST-side grammar test for /bin/test and /bin/[.
//
// Build & run (one line):
//   gcc -g -Wall -Wextra -Werror -I userland/libos64/include -I abi/include
//       tools/test_test_host.c -o /tmp/os64_test_test && /tmp/os64_test_test

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "os64/io.h"

bool os64_streq(const char *left, const char *right)
{
    return strcmp(left, right) == 0;
}

void *os64_memset(void *destination, int byte, size_t count)
{
    return memset(destination, byte, count);
}

static bool fake_entry(const char *path, os64_dirent_t *entry)
{
    if (strcmp(path, "missing") == 0)
        return false;

    memset(entry, 0, sizeof(*entry));
    if (strcmp(path, "dir") == 0)
        entry->flags = OS64_DE_DIR;
    if (strcmp(path, "empty") != 0 && strcmp(path, "dir") != 0)
        entry->size = 4;
    if (strcmp(path, "new") == 0)
        entry->mtime = 200;
    else if (strcmp(path, "old") == 0)
        entry->mtime = 100;
    else
        entry->mtime = 50;
    return true;
}

int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    return fake_entry(path, entry) ? 0 : -1;
}

int32_t os64_hprintf(int32_t handle, const char *format, ...)
{
    (void)handle;
    (void)format;
    return 0;
}

#define main test_utility_main
#include "../userland/apps/test/test.c"
#undef main

static int failures;

static void expect_status(int line, int expected, int argc, char **argv)
{
    int actual = test_utility_main(argc, argv);
    if (actual != expected)
    {
        printf("FAIL line %d: expected %d, got %d:", line, expected, actual);
        for (int i = 0; i < argc; i++)
            printf(" [%s]", argv[i]);
        printf("\n");
        failures++;
    }
}

#define EXPECT_STATUS(expected, ...) do { \
    char *args[] = { __VA_ARGS__ }; \
    expect_status(__LINE__, (expected), \
                  (int)(sizeof(args) / sizeof(args[0])), args); \
} while (0)

int main(void)
{
    EXPECT_STATUS(1, "test");
    EXPECT_STATUS(0, "test", "word");
    EXPECT_STATUS(1, "test", "");
    EXPECT_STATUS(0, "test", "-n");
    EXPECT_STATUS(0, "test", "-n", "word");
    EXPECT_STATUS(0, "test", "-z", "");
    EXPECT_STATUS(0, "test", "-n", "=", "-n");
    EXPECT_STATUS(0, "test", "abc", "!=", "xyz");
    EXPECT_STATUS(0, "test", "abc", "<", "xyz");

    EXPECT_STATUS(0, "test", "-12", "-lt", "3");
    EXPECT_STATUS(0, "test", "+3", "-eq", "003");
    EXPECT_STATUS(0, "test", "-9223372036854775808", "-lt",
                  "9223372036854775807");
    EXPECT_STATUS(2, "test", "9223372036854775808", "-eq", "0");
    EXPECT_STATUS(2, "test", "12x", "-eq", "12");

    EXPECT_STATUS(0, "test", "!", "");
    EXPECT_STATUS(1, "test", "!", "!");
    EXPECT_STATUS(1, "test", "!", "-n");
    EXPECT_STATUS(0, "test", "!", "!", "word");
    EXPECT_STATUS(1, "test", "word", "-a", "");
    EXPECT_STATUS(0, "test", "", "-o", "word");
    EXPECT_STATUS(0, "test", "word", "-o", "", "-a", "");
    EXPECT_STATUS(0, "test", "(", "", "-o", "word", ")");
    EXPECT_STATUS(2, "test", "(", "word");
    EXPECT_STATUS(2, "test", "word", "extra");

    EXPECT_STATUS(0, "test", "-e", "file");
    EXPECT_STATUS(1, "test", "-e", "missing");
    EXPECT_STATUS(0, "test", "-f", "file");
    EXPECT_STATUS(1, "test", "-f", "dir");
    EXPECT_STATUS(0, "test", "-d", "dir");
    EXPECT_STATUS(1, "test", "-s", "empty");
    EXPECT_STATUS(0, "test", "new", "-nt", "old");
    EXPECT_STATUS(0, "test", "missing", "-ot", "old");
    // Operators os64 has no answer for are UNKNOWN, refused by name: a
    // pasted `[ -x "$f" ]` must say so, not quietly take the else branch.
    EXPECT_STATUS(2, "test", "-w", "file");
    EXPECT_STATUS(2, "test", "-r", "file");
    EXPECT_STATUS(2, "test", "file", "-ef", "file");
    EXPECT_STATUS(2, "test", "!", "-x", "file");
    EXPECT_STATUS(0, "test", "-w");                 // one word is a string, as ever

    EXPECT_STATUS(0, "[", "-d", "dir", "]");
    EXPECT_STATUS(1, "/bin/[", "]");
    EXPECT_STATUS(2, "[", "word");

    if (failures == 0)
    {
        printf("test utility host tests: all passed\n");
        return 0;
    }
    printf("test utility host tests: %d FAILURE(S)\n", failures);
    return 1;
}
