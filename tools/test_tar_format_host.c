// test_tar_format_host.c — host-side ustar codec and path-gate tests.
//
// Run through tools/test_tar_host.sh. That script also places a header from
// this codec in front of the host tar and feeds a host-created ustar archive
// back through this decoder, so both directions cross an implementation seam.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "tar_format.h"

static int failures;

static void check(int condition, const char *name)
{
    printf("%-58s %s\n", name, condition ? "PASS" : "FAIL");
    if (!condition)
        failures++;
}

static int write_archive(const char *path)
{
    static const uint8_t contents[] = "hello os64\n";
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return 1;

    tar_entry_t directory = {
        .path = "tree", .mtime = 123, .mode = 0755,
        .type = TAR_TYPE_DIRECTORY
    };
    tar_entry_t regular = {
        .path = "tree/hello.txt", .size = sizeof(contents) - 1,
        .mtime = 456, .mode = 0644, .type = TAR_TYPE_REGULAR
    };
    uint8_t block[TAR_BLOCK_SIZE];
    uint8_t zero[TAR_BLOCK_SIZE] = {0};

    int failed = tar_header_encode(block, &directory) != TAR_FORMAT_OK ||
                 fwrite(block, 1, sizeof(block), file) != sizeof(block) ||
                 tar_header_encode(block, &regular) != TAR_FORMAT_OK ||
                 fwrite(block, 1, sizeof(block), file) != sizeof(block) ||
                 fwrite(contents, 1, sizeof(contents) - 1, file) !=
                    sizeof(contents) - 1 ||
                 fwrite(zero, 1,
                        TAR_BLOCK_SIZE - (sizeof(contents) - 1), file) !=
                    TAR_BLOCK_SIZE - (sizeof(contents) - 1) ||
                 fwrite(zero, 1, sizeof(zero), file) != sizeof(zero) ||
                 fwrite(zero, 1, sizeof(zero), file) != sizeof(zero);
    if (fclose(file) != 0)
        failed = 1;
    return failed;
}

static int read_archive(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return 1;

    uint8_t block[TAR_BLOCK_SIZE];
    int zero_count = 0;
    int failed = 0;
    while (fread(block, 1, sizeof(block), file) == sizeof(block)) {
        if (tar_block_is_zero(block)) {
            zero_count++;
            if (zero_count == 2)
                break;
            continue;
        }
        zero_count = 0;

        tar_entry_t entry = {0};
        tar_format_result_t result = tar_header_decode(block, &entry);
        if (result != TAR_FORMAT_OK) {
            fprintf(stderr, "decode: %s\n", tar_format_error(result));
            failed = 1;
            break;
        }
        printf("%c %llu %s\n", entry.type,
               (unsigned long long)entry.size, entry.path);

        uint64_t padded = entry.size +
            (TAR_BLOCK_SIZE - entry.size % TAR_BLOCK_SIZE) % TAR_BLOCK_SIZE;
        if (padded > (uint64_t)LONG_MAX || fseek(file, (long)padded, SEEK_CUR) != 0) {
            failed = 1;
            break;
        }
    }
    if (zero_count != 2)
        failed = 1;
    fclose(file);
    return failed;
}

static void unit_tests(void)
{
    tar_entry_t original = {
        .path = "directory/hello.txt", .size = 12345, .mtime = 987654,
        .mode = 0644, .type = TAR_TYPE_REGULAR
    };
    uint8_t block[TAR_BLOCK_SIZE];
    tar_entry_t decoded = {0};

    check(tar_header_encode(block, &original) == TAR_FORMAT_OK,
          "encode a regular ustar header");
    check(memcmp(block + 257, "ustar\0", 6) == 0,
          "encoded header carries POSIX ustar magic");
    check(tar_header_decode(block, &decoded) == TAR_FORMAT_OK,
          "decode the encoded header");
    check(strcmp(decoded.path, original.path) == 0 &&
          decoded.size == original.size && decoded.mtime == original.mtime &&
          decoded.mode == original.mode && decoded.type == original.type,
          "header fields survive an encode/decode round trip");

    tar_entry_t long_name = {
        .path = "this-prefix-is-longer-than-the-short-name-field-can-hold/"
                "another-directory/and-one-more-directory/"
                "a-file-name-that-still-fits.txt",
        .size = 7, .mtime = 8, .mode = 0600, .type = TAR_TYPE_REGULAR
    };
    check(strlen(long_name.path) > 100,
          "long-name fixture reaches the ustar prefix field");
    check(tar_header_encode(block, &long_name) == TAR_FORMAT_OK &&
          tar_header_decode(block, &decoded) == TAR_FORMAT_OK &&
          strcmp(decoded.path, long_name.path) == 0,
          "prefix and name fields reconstruct a long path");

    block[0] ^= 1;
    check(tar_header_decode(block, &decoded) == TAR_FORMAT_BAD_CHECKSUM,
          "a changed header byte is rejected by its checksum");

    char path[TAR_PATH_CAP];
    check(tar_member_path("/alpha/./beta/../gamma", path) == TAR_FORMAT_OK &&
          strcmp(path, "alpha/gamma") == 0,
          "source paths become safe relative member names");
    check(tar_extract_path("safe/./child", path) == TAR_FORMAT_OK &&
          strcmp(path, "safe/child") == 0,
          "safe extraction paths are normalized");
    check(tar_extract_path("../escape", path) == TAR_FORMAT_PATH_TOO_LONG,
          "a leading parent component cannot escape extraction");
    check(tar_extract_path("safe/../escape", path) == TAR_FORMAT_PATH_TOO_LONG,
          "an embedded parent component cannot escape extraction");
    check(tar_extract_path("/absolute", path) == TAR_FORMAT_PATH_TOO_LONG,
          "an absolute member cannot escape extraction");
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--write") == 0)
        return write_archive(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--read") == 0)
        return read_archive(argv[2]);
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--write ARCHIVE | --read ARCHIVE]\n", argv[0]);
        return 2;
    }

    unit_tests();
    printf("\n%d tar format test(s) failed\n", failures);
    return failures != 0;
}
