// test_gzip_host.c — host-side streaming harness for libgzip.
//
// The companion shell script generates independent stored, fixed-Huffman,
// and dynamic-Huffman streams with Python's zlib, then drives each one across
// deliberately hostile input/output chunk boundaries under ASan and UBSan.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gzip/gzip.h"
#include "gzip/inflate.h"

void *os64_malloc(size_t size) { return malloc(size); }
void os64_free(void *memory) { free(memory); }

static uint8_t *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    size_t size = (size_t)end;
    uint8_t *bytes = malloc(size == 0 ? 1 : size);
    if (bytes == NULL || fread(bytes, 1, size, file) != size) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length = size;
    return bytes;
}

static uint64_t parse_u64(const char *text)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        fprintf(stderr, "bad integer: %s\n", text);
        exit(2);
    }
    return (uint64_t)value;
}

static int run_raw(const uint8_t *compressed, size_t compressed_length,
                   const uint8_t *expected, size_t expected_length,
                   size_t input_chunk, size_t output_chunk, uint64_t limit,
                   const char *wanted_status)
{
    os64_inflate_t *stream = os64_inflate_create(limit);
    uint8_t *actual = calloc(1, expected_length + output_chunk + 16);
    if (stream == NULL || actual == NULL) {
        os64_inflate_destroy(stream);
        free(actual);
        return 2;
    }

    size_t input_position = 0;
    size_t output_position = 0;
    os64_inflate_status_t status = OS64_INFLATE_NEED_INPUT;
    for (size_t turns = 0; turns < 10000000; turns++) {
        size_t offered_input = compressed_length - input_position;
        if (offered_input > input_chunk)
            offered_input = input_chunk;
        size_t offered_output = output_chunk;
        const uint8_t *input = compressed + input_position;
        uint8_t *output = actual + output_position;
        size_t input_left = offered_input;
        size_t output_left = offered_output;
        bool finish = input_position + offered_input == compressed_length;

        status = os64_inflate_process(stream, &input, &input_left,
                                      &output, &output_left, finish);
        input_position += offered_input - input_left;
        output_position += offered_output - output_left;
        if (status != OS64_INFLATE_NEED_INPUT &&
            status != OS64_INFLATE_NEED_OUTPUT)
            break;
        if (status == OS64_INFLATE_NEED_INPUT && input_left != 0) {
            fprintf(stderr, "raw decoder requested input with bytes unconsumed\n");
            status = OS64_INFLATE_BAD_ARGUMENT;
            break;
        }
    }

    const char *name = os64_inflate_status_name(status);
    int failed = strcmp(name, wanted_status) != 0;
    if (!failed && status == OS64_INFLATE_DONE)
        failed = input_position != compressed_length ||
                 output_position != expected_length ||
                 memcmp(actual, expected, expected_length) != 0;
    if (failed)
        fprintf(stderr,
                "raw failed: status=%s input=%zu/%zu output=%zu/%zu chunks=%zu/%zu\n",
                name, input_position, compressed_length, output_position,
                expected_length, input_chunk, output_chunk);

    os64_inflate_destroy(stream);
    free(actual);
    return failed;
}

static int run_gzip(const uint8_t *compressed, size_t compressed_length,
                    const uint8_t *expected, size_t expected_length,
                    size_t input_chunk, size_t output_chunk, uint64_t limit,
                    const char *wanted_status, uint32_t wanted_members)
{
    os64_gzip_t *stream = os64_gzip_create(limit);
    uint8_t *actual = calloc(1, expected_length + output_chunk + 16);
    if (stream == NULL || actual == NULL) {
        os64_gzip_destroy(stream);
        free(actual);
        return 2;
    }

    size_t input_position = 0;
    size_t output_position = 0;
    os64_gzip_status_t status = OS64_GZIP_NEED_INPUT;
    for (size_t turns = 0; turns < 10000000; turns++) {
        size_t offered_input = compressed_length - input_position;
        if (offered_input > input_chunk)
            offered_input = input_chunk;
        size_t offered_output = output_chunk;
        const uint8_t *input = compressed + input_position;
        uint8_t *output = actual + output_position;
        size_t input_left = offered_input;
        size_t output_left = offered_output;
        bool finish = input_position + offered_input == compressed_length;

        status = os64_gzip_process(stream, &input, &input_left,
                                   &output, &output_left, finish);
        input_position += offered_input - input_left;
        output_position += offered_output - output_left;
        if (status != OS64_GZIP_NEED_INPUT &&
            status != OS64_GZIP_NEED_OUTPUT)
            break;
        if (status == OS64_GZIP_NEED_INPUT && input_left != 0) {
            fprintf(stderr, "gzip decoder requested input with bytes unconsumed\n");
            status = OS64_GZIP_BAD_ARGUMENT;
            break;
        }
    }

    const char *name = os64_gzip_status_name(status);
    int failed = strcmp(name, wanted_status) != 0;
    if (!failed && status == OS64_GZIP_DONE)
        failed = input_position != compressed_length ||
                 output_position != expected_length ||
                 memcmp(actual, expected, expected_length) != 0 ||
                 os64_gzip_member_count(stream) != wanted_members;
    if (failed)
        fprintf(stderr,
                "gzip failed: status=%s input=%zu/%zu output=%zu/%zu members=%u chunks=%zu/%zu\n",
                name, input_position, compressed_length, output_position,
                expected_length, os64_gzip_member_count(stream), input_chunk,
                output_chunk);

    os64_gzip_destroy(stream);
    free(actual);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 9) {
        fprintf(stderr,
                "usage: %s raw|gzip COMPRESSED EXPECTED IN_CHUNK OUT_CHUNK LIMIT STATUS MEMBERS\n",
                argv[0]);
        return 2;
    }

    size_t compressed_length = 0;
    size_t expected_length = 0;
    uint8_t *compressed = read_file(argv[2], &compressed_length);
    uint8_t *expected = read_file(argv[3], &expected_length);
    if (compressed == NULL || expected == NULL) {
        fprintf(stderr, "could not read test input\n");
        free(compressed);
        free(expected);
        return 2;
    }

    size_t input_chunk = (size_t)parse_u64(argv[4]);
    size_t output_chunk = (size_t)parse_u64(argv[5]);
    uint64_t limit = parse_u64(argv[6]);
    uint32_t members = (uint32_t)parse_u64(argv[8]);
    if (input_chunk == 0 || output_chunk == 0) {
        fprintf(stderr, "chunk sizes must be nonzero\n");
        return 2;
    }

    int result;
    if (strcmp(argv[1], "raw") == 0)
        result = run_raw(compressed, compressed_length, expected,
                         expected_length, input_chunk, output_chunk, limit,
                         argv[7]);
    else if (strcmp(argv[1], "gzip") == 0)
        result = run_gzip(compressed, compressed_length, expected,
                          expected_length, input_chunk, output_chunk, limit,
                          argv[7], members);
    else {
        fprintf(stderr, "unknown codec: %s\n", argv[1]);
        result = 2;
    }

    free(compressed);
    free(expected);
    return result;
}
