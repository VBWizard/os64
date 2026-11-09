#ifndef OS64_LIBGZIP_GZIP_H
#define OS64_LIBGZIP_GZIP_H

// Streaming gzip framing around the raw RFC 1951 engines. The decoder checks
// integrity around <gzip/inflate.h>; the encoder writes integrity metadata
// around <gzip/deflate.h>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_gzip os64_gzip_t;

typedef enum {
    OS64_GZIP_NEED_INPUT = 0,
    OS64_GZIP_NEED_OUTPUT,
    OS64_GZIP_DONE,
    OS64_GZIP_TRUNCATED,
    OS64_GZIP_MALFORMED,
    OS64_GZIP_UNSUPPORTED,
    OS64_GZIP_CHECKSUM,
    OS64_GZIP_TRAILING_DATA,
    OS64_GZIP_LIMIT,
    OS64_GZIP_BAD_ARGUMENT
} os64_gzip_status_t;

// output_limit applies across every member in a concatenated gzip stream.
// UINT64_MAX means unlimited; zero accepts only streams producing no bytes.
os64_gzip_t *os64_gzip_create(uint64_t output_limit);
void os64_gzip_reset(os64_gzip_t *stream, uint64_t output_limit);

// Consume and produce as much as possible. end_of_input distinguishes a
// temporarily empty input chunk from the actual end of the gzip stream.
// Produced bytes remain provisional until DONE because a member's checksum is
// stored after its compressed data. Callers that require atomic publication
// must stage those bytes until the stream has been verified.
// DONE means every member and trailer was verified and no trailing bytes
// remain. Bytes after a valid member must begin another gzip member; other
// bytes, including zero padding, produce OS64_GZIP_TRAILING_DATA. A partial
// next-member header that begins with gzip's magic produces OS64_GZIP_TRUNCATED.
os64_gzip_status_t os64_gzip_process(os64_gzip_t *stream,
                                     const uint8_t **input,
                                     size_t *input_length,
                                     uint8_t **output,
                                     size_t *output_length,
                                     bool end_of_input);

uint64_t os64_gzip_output_size(const os64_gzip_t *stream);
uint32_t os64_gzip_member_count(const os64_gzip_t *stream);
const char *os64_gzip_status_name(os64_gzip_status_t status);
void os64_gzip_destroy(os64_gzip_t *stream);

typedef struct os64_gzip_encoder os64_gzip_encoder_t;

typedef enum {
    OS64_GZIP_ENCODE_NEED_INPUT = 0,
    OS64_GZIP_ENCODE_NEED_OUTPUT,
    OS64_GZIP_ENCODE_DONE,
    OS64_GZIP_ENCODE_BAD_ARGUMENT
} os64_gzip_encode_status_t;

// Allocate an encoder for one gzip member. modification_time is written to
// the RFC 1952 header as epoch seconds; zero means no timestamp is available.
// The portable header does not claim an original filename or host OS.
os64_gzip_encoder_t *os64_gzip_encoder_create(uint32_t modification_time);
void os64_gzip_encoder_reset(os64_gzip_encoder_t *stream,
                             uint32_t modification_time);

// Consume uncompressed bytes and produce one gzip member. end_of_input says
// no later input chunk exists. DONE means the header, compressed body, CRC32,
// and modulo-2^32 input size have all reached the caller.
os64_gzip_encode_status_t os64_gzip_encoder_process(
    os64_gzip_encoder_t *stream,
    const uint8_t **input,
    size_t *input_length,
    uint8_t **output,
    size_t *output_length,
    bool end_of_input);

uint64_t os64_gzip_encoder_input_size(const os64_gzip_encoder_t *stream);
uint64_t os64_gzip_encoder_output_size(const os64_gzip_encoder_t *stream);
const char *os64_gzip_encode_status_name(os64_gzip_encode_status_t status);
void os64_gzip_encoder_destroy(os64_gzip_encoder_t *stream);

#endif // OS64_LIBGZIP_GZIP_H
