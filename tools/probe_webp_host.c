/* Allocation and pixel probe for the pinned decoder. Host-only instrumentation. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "src/webp/decode.h"

typedef union { struct { size_t bytes; } record; max_align_t alignment; } header;
static size_t live, peak, calls;
static unsigned alpha_paths, transforms;
void webp_probe_alpha(int small) { alpha_paths |= small ? 1u : 2u; }
void webp_probe_transform(int type) { transforms |= 1u << type; }
void *webp_port_malloc(size_t n)
{
    assert(n <= SIZE_MAX - sizeof(header));
    header *h = malloc(sizeof(*h) + n);
    if (!h) return NULL;
    h->record.bytes = n + sizeof(*h);
    live += h->record.bytes;
    if (live > peak) peak = live;
    calls++;
    return h + 1;
}
void *webp_port_calloc(size_t n, size_t size)
{
    assert(!n || size <= SIZE_MAX / n);
    void *p = webp_port_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}
void webp_port_free(void *p)
{
    if (!p) return;
    header *h = (header *)p - 1;
    assert(live >= h->record.bytes);
    live -= h->record.bytes;
    free(h);
}
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *os64_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static double now(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    WebPDecoderConfig config;
    assert(WebPInitDecoderConfig(&config));
    assert(WebPGetFeatures(data, (size_t)size, &config.input) == VP8_STATUS_OK);
    size_t output = (size_t)config.input.width * config.input.height * 4;
    uint8_t *pixels = malloc(output);
    assert(pixels);
    config.output.colorspace = MODE_BGRA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = pixels;
    config.output.u.RGBA.size = output;
    config.output.u.RGBA.stride = config.input.width * 4;
    config.options.no_fancy_upsampling = 0;
    config.options.use_threads = 0;
    double start = now();
    VP8StatusCode status = WebPDecode(data, (size_t)size, &config);
    double ms = (now() - start) * 1000;
    assert(status == VP8_STATUS_OK);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < output; ++i) hash = (hash ^ pixels[i]) * UINT64_C(1099511628211);
    WebPFreeDecBuffer(&config.output);
    assert(live == 0);
    printf("{\"width\":%d,\"height\":%d,\"input\":%ld,\"output\":%zu,"
           "\"temporary_peak\":%zu,\"total_peak\":%zu,\"allocations\":%zu,"
           "\"alpha_paths\":%u,\"transforms\":%u,\"milliseconds\":%.3f,"
           "\"pixel_hash\":\"%016llx\"}\n",
           config.input.width, config.input.height, size, output, peak, output + peak,
           calls, alpha_paths, transforms, ms, (unsigned long long)hash);
    free(pixels);
    free(data);
    return 0;
}
