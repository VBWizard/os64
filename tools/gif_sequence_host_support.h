// Host adapters shared by the GIF sequence oracle and decoder benchmark.
#ifndef GIF_SEQUENCE_HOST_SUPPORT_H
#define GIF_SEQUENCE_HOST_SUPPORT_H
#include <assert.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "image/sequence.h"
#include "png/png.h"
#include "jpeg/jpeg.h"
#include "webp/webp.h"
#include "os64/slurp.h"

typedef union { size_t n; max_align_t align; } allocation;
static size_t live, bytes, peak, attempts, fail_at;
void *os64_malloc(size_t n)
{
    attempts++;
    if (fail_at && attempts == fail_at) return NULL;
    allocation *h = malloc(sizeof(*h) + n); assert(h);
    h->n = n; live++; bytes += n;
    if (bytes > peak) peak = bytes;
    return h + 1;
}
void os64_free(void *p)
{
    if (!p) return;
    allocation *h = (allocation *)p - 1;
    assert(live && bytes >= h->n); live--; bytes -= h->n; free(h);
}
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d,c,n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a,b,n); }
os64_png_status_t os64_png_decode(const uint8_t *p, size_t n, uint64_t c, os64_png_image_t *o)
{ (void)p; (void)n; (void)c; (void)o; abort(); }
os64_jpeg_status_t os64_jpeg_decode(const uint8_t *p, size_t n, uint64_t c, size_t m, os64_jpeg_image_t *o)
{ (void)p; (void)n; (void)c; (void)m; (void)o; abort(); }
os64_webp_status_t os64_webp_decode(const uint8_t *p, size_t n, uint64_t c, size_t m, os64_webp_image_t *o)
{ (void)p; (void)n; (void)c; (void)m; (void)o; abort(); }
os64_slurp_status_t os64_slurp(const char *p, size_t c, uint8_t **o, size_t *n)
{ (void)p; (void)c; (void)o; (void)n; abort(); }

#endif
