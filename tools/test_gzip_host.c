// test_gzip_host.c — host-side streaming harness for libgzip.
//
// The companion shell script checks encoder output with Python's zlib, then
// checks independently generated stored, fixed-Huffman, and dynamic-Huffman
// input. Both directions cross hostile chunk boundaries under ASan and UBSan.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gzip/gzip.h"
#include "gzip/deflate.h"
#include "gzip/inflate.h"
#include "os64/str.h"

void *os64_malloc(size_t size) { return malloc(size); }
void os64_free(void *memory) { free(memory); }
void *os64_memset(void *memory, int value, size_t length)
{
    return memset(memory, value, length);
}
void *os64_memcpy(void *destination, const void *source, size_t length)
{
    return memcpy(destination, source, length);
}

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

static size_t output_offer(size_t chunk, size_t position, uint64_t limit)
{
    if ((uint64_t)position >= limit)
        return 0;
    uint64_t remaining = limit - (uint64_t)position;
    return remaining < chunk ? (size_t)remaining : chunk;
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
        size_t offered_output = output_offer(output_chunk, output_position,
                                             limit);
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
        size_t offered_output = output_offer(output_chunk, output_position,
                                             limit);
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

static int write_file(const char *path, const uint8_t *bytes, size_t length)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return -1;
    int failed = fwrite(bytes, 1, length, file) != length;
    if (fclose(file) != 0)
        failed = 1;
    return failed ? -1 : 0;
}

static int run_encode(const char *format, const uint8_t *input_bytes,
                      size_t input_length, const char *output_path,
                      size_t input_chunk, size_t output_chunk, uint32_t mtime)
{
    size_t output_capacity = input_length * 2 + 1024;
    uint8_t *actual = calloc(1, output_capacity);
    os64_deflate_t *raw = NULL;
    os64_gzip_encoder_t *gzip = NULL;
    if (strcmp(format, "raw") == 0)
        raw = os64_deflate_create();
    else if (strcmp(format, "gzip") == 0)
        gzip = os64_gzip_encoder_create(mtime);
    else {
        fprintf(stderr, "unknown encoder format: %s\n", format);
        free(actual);
        return 2;
    }
    if (actual == NULL || (raw == NULL && gzip == NULL)) {
        os64_deflate_destroy(raw);
        os64_gzip_encoder_destroy(gzip);
        free(actual);
        return 2;
    }

    size_t input_position = 0;
    size_t output_position = 0;
    int done = 0;
    for (size_t turns = 0; turns < 10000000; turns++) {
        size_t offered_input = input_length - input_position;
        if (offered_input > input_chunk)
            offered_input = input_chunk;
        size_t offered_output = output_capacity - output_position;
        if (offered_output > output_chunk)
            offered_output = output_chunk;
        if (offered_output == 0) {
            fprintf(stderr, "%s encoder exceeded test output capacity\n",
                    format);
            break;
        }
        const uint8_t *input = input_bytes + input_position;
        uint8_t *output = actual + output_position;
        size_t input_left = offered_input;
        size_t output_left = offered_output;
        bool finish = input_position + offered_input == input_length;

        int status;
        int need_input;
        int need_output;
        if (raw != NULL) {
            status = os64_deflate_process(raw, &input, &input_left, &output,
                                          &output_left, finish);
            need_input = OS64_DEFLATE_NEED_INPUT;
            need_output = OS64_DEFLATE_NEED_OUTPUT;
            done = status == OS64_DEFLATE_DONE;
        } else {
            status = os64_gzip_encoder_process(gzip, &input, &input_left,
                                                &output, &output_left, finish);
            need_input = OS64_GZIP_ENCODE_NEED_INPUT;
            need_output = OS64_GZIP_ENCODE_NEED_OUTPUT;
            done = status == OS64_GZIP_ENCODE_DONE;
        }
        input_position += offered_input - input_left;
        output_position += offered_output - output_left;
        if (done)
            break;
        if (status != need_input && status != need_output) {
            fprintf(stderr, "%s encoder failed with status %d\n", format,
                    status);
            break;
        }
        if (status == need_input && input_left != 0) {
            fprintf(stderr, "%s encoder left input behind\n", format);
            break;
        }
    }

    uint64_t counted_input = raw != NULL
        ? os64_deflate_input_size(raw)
        : os64_gzip_encoder_input_size(gzip);
    uint64_t counted_output = raw != NULL
        ? os64_deflate_output_size(raw)
        : os64_gzip_encoder_output_size(gzip);
    int failed = !done || input_position != input_length ||
                 counted_input != input_length ||
                 counted_output != output_position ||
                 write_file(output_path, actual, output_position) < 0;
    if (failed)
        fprintf(stderr,
                "%s encode failed: input=%zu/%zu output=%zu counted=%llu chunks=%zu/%zu\n",
                format, input_position, input_length, output_position,
                (unsigned long long)counted_output, input_chunk, output_chunk);

    os64_deflate_destroy(raw);
    os64_gzip_encoder_destroy(gzip);
    free(actual);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc == 8 && strcmp(argv[1], "encode") == 0) {
        size_t input_length = 0;
        uint8_t *input = read_file(argv[3], &input_length);
        if (input == NULL) {
            fprintf(stderr, "could not read encoder input\n");
            return 2;
        }
        size_t input_chunk = (size_t)parse_u64(argv[5]);
        size_t output_chunk = (size_t)parse_u64(argv[6]);
        uint32_t mtime = (uint32_t)parse_u64(argv[7]);
        if (input_chunk == 0 || output_chunk == 0) {
            fprintf(stderr, "chunk sizes must be nonzero\n");
            free(input);
            return 2;
        }
        int result = run_encode(argv[2], input, input_length, argv[4],
                                input_chunk, output_chunk, mtime);
        free(input);
        return result;
    }

    if (argc != 9) {
        fprintf(stderr,
                "usage: %s raw|gzip COMPRESSED EXPECTED IN_CHUNK OUT_CHUNK LIMIT STATUS MEMBERS\n"
                "       %s encode raw|gzip INPUT OUTPUT IN_CHUNK OUT_CHUNK MTIME\n",
                argv[0],
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
