#ifndef OS64_LIBGZIP_DEFLATE_H
#define OS64_LIBGZIP_DEFLATE_H

// Raw DEFLATE encoding: uncompressed bytes in, compressed bytes out.
//
// This is the container-free counterpart to <gzip/inflate.h>. The encoder
// emits RFC 1951 fixed-Huffman blocks backed by a 32 KiB LZ77 window. gzip
// framing, timestamps, and checksums belong to the wrapper in <gzip/gzip.h>.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct os64_deflate os64_deflate_t;

typedef enum {
    OS64_DEFLATE_NEED_INPUT = 0,
    OS64_DEFLATE_NEED_OUTPUT,
    OS64_DEFLATE_DONE,
    OS64_DEFLATE_BAD_ARGUMENT
} os64_deflate_status_t;

// Allocate an encoder. Its memory use is bounded and independent of the
// stream length: one 32 KiB input block, one 32 KiB history window, bounded
// match indexes, and one worst-case fixed-Huffman output block.
os64_deflate_t *os64_deflate_create(void);

// Return an existing encoder to the start of a new, independent stream.
void os64_deflate_reset(os64_deflate_t *stream);

// Encode until input or output is exhausted. The four cursor values advance
// by exactly what was consumed and produced; either chunk may end at any byte.
// end_of_input says no later input exists. DONE is returned only after the
// final block and every compressed byte have reached the caller.
os64_deflate_status_t os64_deflate_process(os64_deflate_t *stream,
                                            const uint8_t **input,
                                            size_t *input_length,
                                            uint8_t **output,
                                            size_t *output_length,
                                            bool end_of_input);

uint64_t os64_deflate_input_size(const os64_deflate_t *stream);
uint64_t os64_deflate_output_size(const os64_deflate_t *stream);
const char *os64_deflate_status_name(os64_deflate_status_t status);
void os64_deflate_destroy(os64_deflate_t *stream);

#endif // OS64_LIBGZIP_DEFLATE_H
