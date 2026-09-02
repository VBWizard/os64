// gzip.c — RFC 1952 framing around the shared raw DEFLATE decoder.
//
// A gzip trailer authenticates both the bytes and their length. Neither
// check is optional here: returning plausible output from a damaged stream is
// worse than refusing it, because the caller cannot recover the distinction.

#include "gzip/gzip.h"
#include "gzip/inflate.h"
#include "os64/crc32.h"
#include "os64/mem.h"
#include "os64/str.h"

#define GZIP_FLAG_HEADER_CRC 0x02u
#define GZIP_FLAG_EXTRA      0x04u
#define GZIP_FLAG_NAME       0x08u
#define GZIP_FLAG_COMMENT    0x10u
#define GZIP_FLAG_RESERVED   0xe0u

typedef enum {
    GZIP_MODE_FIXED_HEADER,
    GZIP_MODE_EXTRA_LENGTH,
    GZIP_MODE_EXTRA,
    GZIP_MODE_NAME,
    GZIP_MODE_COMMENT,
    GZIP_MODE_HEADER_CRC,
    GZIP_MODE_BODY,
    GZIP_MODE_TRAILER,
    GZIP_MODE_BETWEEN_MEMBERS,
    GZIP_MODE_DONE,
    GZIP_MODE_ERROR
} gzip_mode_t;

struct os64_gzip {
    os64_inflate_t *inflate;
    gzip_mode_t mode;
    os64_gzip_status_t terminal;
    uint64_t output_limit;
    uint64_t output_size;
    uint32_t members;
    uint32_t header_crc;
    uint32_t data_crc;
    uint32_t member_size;
    uint32_t extra_remaining;
    uint8_t flags;
    uint8_t fixed_header[10];
    uint8_t fixed_have;
    uint8_t short_field[2];
    uint8_t short_have;
    uint8_t trailer[8];
    uint8_t trailer_have;
};

static uint16_t read_le16(const uint8_t bytes[2])
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t bytes[4])
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void refuse(os64_gzip_t *stream, os64_gzip_status_t status)
{
    stream->mode = GZIP_MODE_ERROR;
    stream->terminal = status;
}

static os64_gzip_status_t input_short(os64_gzip_t *stream, bool end_of_input)
{
    if (end_of_input) {
        refuse(stream, OS64_GZIP_TRUNCATED);
        return stream->terminal;
    }
    return OS64_GZIP_NEED_INPUT;
}

static void start_member(os64_gzip_t *stream)
{
    stream->mode = GZIP_MODE_FIXED_HEADER;
    stream->header_crc = os64_crc32_begin();
    stream->data_crc = os64_crc32_begin();
    stream->member_size = 0;
    stream->extra_remaining = 0;
    stream->flags = 0;
    stream->fixed_have = 0;
    stream->short_have = 0;
    stream->trailer_have = 0;
}

static void begin_body(os64_gzip_t *stream)
{
    uint64_t remaining = stream->output_limit - stream->output_size;
    os64_inflate_reset(stream->inflate, remaining);
    stream->mode = GZIP_MODE_BODY;
}

static void advance_optional_header(os64_gzip_t *stream)
{
    stream->short_have = 0;
    if ((stream->flags & GZIP_FLAG_EXTRA) != 0) {
        stream->flags &= (uint8_t)~GZIP_FLAG_EXTRA;
        stream->mode = GZIP_MODE_EXTRA_LENGTH;
    } else if ((stream->flags & GZIP_FLAG_NAME) != 0) {
        stream->flags &= (uint8_t)~GZIP_FLAG_NAME;
        stream->mode = GZIP_MODE_NAME;
    } else if ((stream->flags & GZIP_FLAG_COMMENT) != 0) {
        stream->flags &= (uint8_t)~GZIP_FLAG_COMMENT;
        stream->mode = GZIP_MODE_COMMENT;
    } else if ((stream->flags & GZIP_FLAG_HEADER_CRC) != 0) {
        stream->flags &= (uint8_t)~GZIP_FLAG_HEADER_CRC;
        stream->mode = GZIP_MODE_HEADER_CRC;
    } else {
        begin_body(stream);
    }
}

static bool take_byte(const uint8_t **input, size_t *length, uint8_t *byte)
{
    if (*length == 0)
        return false;
    *byte = *(*input)++;
    (*length)--;
    return true;
}

static os64_gzip_status_t map_inflate_status(os64_inflate_status_t status)
{
    switch (status) {
        case OS64_INFLATE_NEED_INPUT:   return OS64_GZIP_NEED_INPUT;
        case OS64_INFLATE_NEED_OUTPUT:  return OS64_GZIP_NEED_OUTPUT;
        case OS64_INFLATE_DONE:         return OS64_GZIP_DONE;
        case OS64_INFLATE_TRUNCATED:    return OS64_GZIP_TRUNCATED;
        case OS64_INFLATE_MALFORMED:    return OS64_GZIP_MALFORMED;
        case OS64_INFLATE_LIMIT:        return OS64_GZIP_LIMIT;
        case OS64_INFLATE_BAD_ARGUMENT: return OS64_GZIP_BAD_ARGUMENT;
    }
    return OS64_GZIP_MALFORMED;
}

os64_gzip_t *os64_gzip_create(uint64_t output_limit)
{
    os64_gzip_t *stream = os64_malloc(sizeof(*stream));
    if (stream == NULL)
        return NULL;
    os64_memset(stream, 0, sizeof(*stream));
    stream->inflate = os64_inflate_create(output_limit);
    if (stream->inflate == NULL) {
        os64_free(stream);
        return NULL;
    }
    os64_gzip_reset(stream, output_limit);
    return stream;
}

void os64_gzip_reset(os64_gzip_t *stream, uint64_t output_limit)
{
    if (stream == NULL)
        return;
    os64_inflate_t *inflate = stream->inflate;
    os64_memset(stream, 0, sizeof(*stream));
    stream->inflate = inflate;
    stream->output_limit = output_limit;
    stream->terminal = OS64_GZIP_NEED_INPUT;
    start_member(stream);
}

void os64_gzip_destroy(os64_gzip_t *stream)
{
    if (stream == NULL)
        return;
    os64_inflate_destroy(stream->inflate);
    os64_free(stream);
}

uint64_t os64_gzip_output_size(const os64_gzip_t *stream)
{
    return stream == NULL ? 0 : stream->output_size;
}

uint32_t os64_gzip_member_count(const os64_gzip_t *stream)
{
    return stream == NULL ? 0 : stream->members;
}

const char *os64_gzip_status_name(os64_gzip_status_t status)
{
    switch (status) {
        case OS64_GZIP_NEED_INPUT:   return "needs input";
        case OS64_GZIP_NEED_OUTPUT:  return "needs output space";
        case OS64_GZIP_DONE:         return "done";
        case OS64_GZIP_TRUNCATED:    return "truncated stream";
        case OS64_GZIP_MALFORMED:    return "malformed stream";
        case OS64_GZIP_UNSUPPORTED:  return "unsupported compression method";
        case OS64_GZIP_CHECKSUM:     return "checksum or size mismatch";
        case OS64_GZIP_TRAILING_DATA: return "trailing data after final member";
        case OS64_GZIP_LIMIT:        return "output limit exceeded";
        case OS64_GZIP_BAD_ARGUMENT: return "bad argument";
    }
    return "unknown";
}

os64_gzip_status_t os64_gzip_process(os64_gzip_t *stream,
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
        return OS64_GZIP_BAD_ARGUMENT;
    if (stream->mode == GZIP_MODE_DONE)
        return OS64_GZIP_DONE;
    if (stream->mode == GZIP_MODE_ERROR)
        return stream->terminal;

    for (;;) {
        uint8_t byte;

        switch (stream->mode) {
            case GZIP_MODE_FIXED_HEADER:
                if (!take_byte(input, input_length, &byte))
                    return input_short(stream, end_of_input);
                stream->fixed_header[stream->fixed_have++] = byte;
                stream->header_crc = os64_crc32_update(stream->header_crc,
                                                        &byte, 1);
                if ((stream->fixed_have == 1 && byte != 0x1f) ||
                    (stream->fixed_have == 2 && byte != 0x8b)) {
                    refuse(stream, stream->members == 0 ? OS64_GZIP_MALFORMED :
                                                         OS64_GZIP_TRAILING_DATA);
                    return stream->terminal;
                }
                if (stream->fixed_have != sizeof(stream->fixed_header))
                    break;
                if (stream->fixed_header[2] != 8) {
                    refuse(stream, OS64_GZIP_UNSUPPORTED);
                    return stream->terminal;
                }
                stream->flags = stream->fixed_header[3];
                if ((stream->flags & GZIP_FLAG_RESERVED) != 0) {
                    refuse(stream, OS64_GZIP_MALFORMED);
                    return stream->terminal;
                }
                advance_optional_header(stream);
                break;

            case GZIP_MODE_EXTRA_LENGTH:
                if (!take_byte(input, input_length, &byte))
                    return input_short(stream, end_of_input);
                stream->short_field[stream->short_have++] = byte;
                stream->header_crc = os64_crc32_update(stream->header_crc,
                                                        &byte, 1);
                if (stream->short_have == 2) {
                    stream->extra_remaining = read_le16(stream->short_field);
                    stream->mode = GZIP_MODE_EXTRA;
                }
                break;

            case GZIP_MODE_EXTRA:
                if (stream->extra_remaining == 0) {
                    advance_optional_header(stream);
                    break;
                }
                if (!take_byte(input, input_length, &byte))
                    return input_short(stream, end_of_input);
                stream->header_crc = os64_crc32_update(stream->header_crc,
                                                        &byte, 1);
                stream->extra_remaining--;
                break;

            case GZIP_MODE_NAME:
            case GZIP_MODE_COMMENT:
                if (!take_byte(input, input_length, &byte))
                    return input_short(stream, end_of_input);
                stream->header_crc = os64_crc32_update(stream->header_crc,
                                                        &byte, 1);
                if (byte == 0)
                    advance_optional_header(stream);
                break;

            case GZIP_MODE_HEADER_CRC:
                if (!take_byte(input, input_length, &byte))
                    return input_short(stream, end_of_input);
                stream->short_field[stream->short_have++] = byte;
                if (stream->short_have == 2) {
                    uint16_t expected = read_le16(stream->short_field);
                    uint16_t actual = (uint16_t)os64_crc32_end(stream->header_crc);
                    if (expected != actual) {
                        refuse(stream, OS64_GZIP_CHECKSUM);
                        return stream->terminal;
                    }
                    begin_body(stream);
                }
                break;

            case GZIP_MODE_BODY: {
                uint8_t *output_start = *output;
                size_t output_before = *output_length;
                os64_inflate_status_t inflate_status = os64_inflate_process(
                    stream->inflate, input, input_length, output, output_length,
                    end_of_input);
                size_t produced = output_before - *output_length;
                if (produced != 0) {
                    stream->data_crc = os64_crc32_update(stream->data_crc,
                                                         output_start, produced);
                    stream->member_size += (uint32_t)produced;
                    stream->output_size += produced;
                }
                if (inflate_status == OS64_INFLATE_DONE) {
                    stream->trailer_have = 0;
                    stream->mode = GZIP_MODE_TRAILER;
                    break;
                }
                os64_gzip_status_t status = map_inflate_status(inflate_status);
                if (status == OS64_GZIP_NEED_INPUT ||
                    status == OS64_GZIP_NEED_OUTPUT)
                    return status;
                refuse(stream, status);
                return stream->terminal;
            }

            case GZIP_MODE_TRAILER:
                if (!take_byte(input, input_length, &byte))
                    return input_short(stream, end_of_input);
                stream->trailer[stream->trailer_have++] = byte;
                if (stream->trailer_have == sizeof(stream->trailer)) {
                    uint32_t expected_crc = read_le32(stream->trailer);
                    uint32_t expected_size = read_le32(stream->trailer + 4);
                    if (expected_crc != os64_crc32_end(stream->data_crc) ||
                        expected_size != stream->member_size) {
                        refuse(stream, OS64_GZIP_CHECKSUM);
                        return stream->terminal;
                    }
                    stream->members++;
                    stream->mode = GZIP_MODE_BETWEEN_MEMBERS;
                }
                break;

            case GZIP_MODE_BETWEEN_MEMBERS:
                if (*input_length == 0) {
                    if (end_of_input) {
                        stream->mode = GZIP_MODE_DONE;
                        return OS64_GZIP_DONE;
                    }
                    return OS64_GZIP_NEED_INPUT;
                }
                start_member(stream);
                break;

            case GZIP_MODE_DONE:
                return OS64_GZIP_DONE;
            case GZIP_MODE_ERROR:
                return stream->terminal;
        }
    }
}
