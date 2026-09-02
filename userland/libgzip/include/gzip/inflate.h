#ifndef OS64_LIBGZIP_INFLATE_H
#define OS64_LIBGZIP_INFLATE_H

// Raw DEFLATE decoding: compressed bytes in, uncompressed bytes out.
//
// DEFLATE is the bitstream shared by gzip, zlib (and therefore PNG), and
// several network formats. It has no checksum or file header of its own;
// those belong to the container layered above it. Keeping this door raw is
// what lets all of those containers share one decoder without pretending
// that gzip and zlib are the same format.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_inflate os64_inflate_t;

typedef enum {
    OS64_INFLATE_NEED_INPUT = 0,
    OS64_INFLATE_NEED_OUTPUT,
    OS64_INFLATE_DONE,
    OS64_INFLATE_TRUNCATED,
    OS64_INFLATE_MALFORMED,
    OS64_INFLATE_LIMIT,
    OS64_INFLATE_BAD_ARGUMENT
} os64_inflate_status_t;

// Allocate a decoder. output_limit is the greatest number of uncompressed
// bytes this stream may produce. UINT64_MAX means the caller deliberately
// imposes no limit; zero permits an empty stream and refuses its first output
// byte. The decoder owns a 32 KiB history window and no other allocation.
// NULL means the state could not be allocated.
os64_inflate_t *os64_inflate_create(uint64_t output_limit);

// Return an existing decoder to the start of a new, independent DEFLATE
// stream. Its history and byte count are discarded.
void os64_inflate_reset(os64_inflate_t *stream, uint64_t output_limit);

// Decode until input or output is exhausted, the final block ends, or the
// stream is refused. The four cursor values are updated by exactly what was
// consumed and produced. Input and output chunks may be split at any byte.
//
// end_of_input says no later input chunk exists. A decoder that needs another
// bit then reports TRUNCATED instead of NEED_INPUT. DONE leaves bytes after
// the DEFLATE stream untouched so a container can read its trailer.
os64_inflate_status_t os64_inflate_process(os64_inflate_t *stream,
                                            const uint8_t **input,
                                            size_t *input_length,
                                            uint8_t **output,
                                            size_t *output_length,
                                            bool end_of_input);

uint64_t os64_inflate_output_size(const os64_inflate_t *stream);
const char *os64_inflate_status_name(os64_inflate_status_t status);
void os64_inflate_destroy(os64_inflate_t *stream);

#endif // OS64_LIBGZIP_INFLATE_H
