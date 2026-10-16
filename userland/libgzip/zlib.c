// zlib.c — RFC 1950 framing around the shared raw DEFLATE decoder.

#include "gzip/zlib.h"
#include "gzip/inflate.h"
#include "os64/mem.h"
#include "os64/str.h"

#define ZLIB_ADLER_MOD 65521u

typedef enum {
    ZLIB_MODE_HEADER,
    ZLIB_MODE_BODY,
    ZLIB_MODE_TRAILER,
    ZLIB_MODE_DONE,
    ZLIB_MODE_ERROR
} zlib_mode_t;

struct os64_zlib {
    os64_inflate_t *inflate;
    zlib_mode_t mode;
    os64_zlib_status_t terminal;
    uint32_t adler_first, adler_second;
    uint8_t header[2];
    uint8_t header_have;
    uint8_t trailer[4];
    uint8_t trailer_have;
};

static void adler_update(os64_zlib_t *stream, const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        stream->adler_first += bytes[i];
        if (stream->adler_first >= ZLIB_ADLER_MOD)
            stream->adler_first -= ZLIB_ADLER_MOD;
        stream->adler_second += stream->adler_first;
        if (stream->adler_second >= ZLIB_ADLER_MOD)
            stream->adler_second -= ZLIB_ADLER_MOD;
    }
}

static os64_zlib_status_t refuse(os64_zlib_t *stream, os64_zlib_status_t status)
{
    stream->mode = ZLIB_MODE_ERROR;
    stream->terminal = status;
    return status;
}

static os64_zlib_status_t input_short(os64_zlib_t *stream, bool end_of_input)
{
    return end_of_input ? refuse(stream, OS64_ZLIB_TRUNCATED) : OS64_ZLIB_NEED_INPUT;
}

static os64_zlib_status_t map_inflate_status(os64_inflate_status_t status)
{
    switch (status) {
        case OS64_INFLATE_NEED_INPUT:   return OS64_ZLIB_NEED_INPUT;
        case OS64_INFLATE_NEED_OUTPUT:  return OS64_ZLIB_NEED_OUTPUT;
        case OS64_INFLATE_DONE:         return OS64_ZLIB_DONE;
        case OS64_INFLATE_TRUNCATED:    return OS64_ZLIB_TRUNCATED;
        case OS64_INFLATE_MALFORMED:    return OS64_ZLIB_MALFORMED;
        case OS64_INFLATE_LIMIT:        return OS64_ZLIB_LIMIT;
        case OS64_INFLATE_BAD_ARGUMENT: return OS64_ZLIB_BAD_ARGUMENT;
    }
    return OS64_ZLIB_MALFORMED;
}

os64_zlib_t *os64_zlib_create(uint64_t output_limit)
{
    os64_zlib_t *stream = os64_malloc(sizeof(*stream));
    if (stream == NULL)
        return NULL;
    os64_memset(stream, 0, sizeof(*stream));
    stream->inflate = os64_inflate_create(output_limit);
    if (stream->inflate == NULL) {
        os64_free(stream);
        return NULL;
    }
    os64_zlib_reset(stream, output_limit);
    return stream;
}

void os64_zlib_reset(os64_zlib_t *stream, uint64_t output_limit)
{
    if (stream == NULL)
        return;
    os64_inflate_t *inflate = stream->inflate;
    os64_memset(stream, 0, sizeof(*stream));
    stream->inflate = inflate;
    os64_inflate_reset(inflate, output_limit);
    stream->mode = ZLIB_MODE_HEADER;
    stream->terminal = OS64_ZLIB_NEED_INPUT;
    stream->adler_first = 1;
}

os64_zlib_status_t os64_zlib_process(os64_zlib_t *stream,
                                     const uint8_t **input,
                                     size_t *input_length,
                                     uint8_t **output,
                                     size_t *output_length,
                                     bool end_of_input)
{
    if (stream == NULL || input == NULL || input_length == NULL ||
        output == NULL || output_length == NULL ||
        (*input_length != 0 && *input == NULL) ||
        (*output_length != 0 && *output == NULL))
        return OS64_ZLIB_BAD_ARGUMENT;
    for (;;) {
        switch (stream->mode) {
            case ZLIB_MODE_HEADER:
                if (*input_length == 0)
                    return input_short(stream, end_of_input);
                stream->header[stream->header_have++] = *(*input)++;
                (*input_length)--;
                if (stream->header_have < 2)
                    break;
                if ((stream->header[0] & 15u) != 8 || (stream->header[0] >> 4) > 7 ||
                    ((uint32_t)stream->header[0] << 8 | stream->header[1]) % 31u != 0)
                    return refuse(stream, OS64_ZLIB_MALFORMED);
                if ((stream->header[1] & 0x20u) != 0)
                    return refuse(stream, OS64_ZLIB_UNSUPPORTED);
                stream->mode = ZLIB_MODE_BODY;
                break;

            case ZLIB_MODE_BODY: {
                uint8_t *output_start = *output;
                size_t output_before = *output_length;
                os64_inflate_status_t inflate_status = os64_inflate_process(
                    stream->inflate, input, input_length, output, output_length, end_of_input);
                adler_update(stream, output_start, output_before - *output_length);
                if (inflate_status == OS64_INFLATE_DONE) {
                    stream->mode = ZLIB_MODE_TRAILER;
                    break;
                }
                os64_zlib_status_t status = map_inflate_status(inflate_status);
                if (status == OS64_ZLIB_NEED_INPUT || status == OS64_ZLIB_NEED_OUTPUT)
                    return status;
                return refuse(stream, status);
            }

            case ZLIB_MODE_TRAILER:
                if (*input_length == 0)
                    return input_short(stream, end_of_input);
                stream->trailer[stream->trailer_have++] = *(*input)++;
                (*input_length)--;
                if (stream->trailer_have < 4)
                    break;
                if (((uint32_t)stream->trailer[0] << 24 | (uint32_t)stream->trailer[1] << 16 |
                     (uint32_t)stream->trailer[2] << 8 | stream->trailer[3]) !=
                    (stream->adler_second << 16 | stream->adler_first))
                    return refuse(stream, OS64_ZLIB_CHECKSUM);
                stream->mode = ZLIB_MODE_DONE;
                return OS64_ZLIB_DONE;

            case ZLIB_MODE_DONE:
                return OS64_ZLIB_DONE;
            case ZLIB_MODE_ERROR:
                return stream->terminal;
        }
    }
}

uint64_t os64_zlib_output_size(const os64_zlib_t *stream)
{
    return stream == NULL ? 0 : os64_inflate_output_size(stream->inflate);
}

const char *os64_zlib_status_name(os64_zlib_status_t status)
{
    switch (status) {
        case OS64_ZLIB_NEED_INPUT:   return "needs input";
        case OS64_ZLIB_NEED_OUTPUT:  return "needs output space";
        case OS64_ZLIB_DONE:         return "done";
        case OS64_ZLIB_TRUNCATED:    return "truncated stream";
        case OS64_ZLIB_MALFORMED:    return "malformed stream";
        case OS64_ZLIB_UNSUPPORTED:  return "preset dictionary";
        case OS64_ZLIB_CHECKSUM:     return "checksum mismatch";
        case OS64_ZLIB_LIMIT:        return "output limit exceeded";
        case OS64_ZLIB_BAD_ARGUMENT: return "bad argument";
    }
    return "unknown";
}

void os64_zlib_destroy(os64_zlib_t *stream)
{
    if (stream == NULL)
        return;
    os64_inflate_destroy(stream->inflate);
    os64_free(stream);
}
