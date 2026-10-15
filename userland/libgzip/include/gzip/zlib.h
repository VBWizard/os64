#ifndef OS64_LIBGZIP_ZLIB_H
#define OS64_LIBGZIP_ZLIB_H

// Streaming zlib framing (RFC 1950) around the raw RFC 1951 decoder: a
// two-byte header — compression method 8, a window of at most 32 KiB, the
// check bits — then the DEFLATE stream, then its Adler-32, big-endian. It
// is what PNG's image data and WOFF's tables are compressed in.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_zlib os64_zlib_t;

typedef enum {
    OS64_ZLIB_NEED_INPUT = 0,
    OS64_ZLIB_NEED_OUTPUT,
    OS64_ZLIB_DONE,
    OS64_ZLIB_TRUNCATED,
    OS64_ZLIB_MALFORMED,
    // A preset dictionary (FDICT): valid zlib, and nothing here hands one
    // in, so the stream cannot be decoded.
    OS64_ZLIB_UNSUPPORTED,
    OS64_ZLIB_CHECKSUM,
    OS64_ZLIB_LIMIT,
    OS64_ZLIB_BAD_ARGUMENT
} os64_zlib_status_t;

// output_limit is the most uncompressed bytes the stream may make;
// UINT64_MAX means unlimited, zero accepts only a stream that makes none.
os64_zlib_t *os64_zlib_create(uint64_t output_limit);
void os64_zlib_reset(os64_zlib_t *stream, uint64_t output_limit);

// Consume and produce as much as possible, the four cursors updated by
// exactly what was taken and made; input and output may be split at any
// byte. end_of_input says no later chunk exists: a stream short of its end
// is then TRUNCATED. Produced bytes are provisional until DONE, since the
// checksum follows them. DONE means the header, the stream and its
// Adler-32 were all whole and right; bytes after it are left untouched, a
// container's to judge.
os64_zlib_status_t os64_zlib_process(os64_zlib_t *stream,
                                     const uint8_t **input,
                                     size_t *input_length,
                                     uint8_t **output,
                                     size_t *output_length,
                                     bool end_of_input);

uint64_t os64_zlib_output_size(const os64_zlib_t *stream);
const char *os64_zlib_status_name(os64_zlib_status_t status);
void os64_zlib_destroy(os64_zlib_t *stream);

#endif // OS64_LIBGZIP_ZLIB_H
