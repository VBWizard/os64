#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "webp/webp.h"

typedef union { size_t size; max_align_t alignment; } host_allocation;
static _Thread_local size_t live, calls, fail_at, charged, peak;
static unsigned waits, concurrent;
void *os64_malloc(size_t n)
{
    if (++calls == fail_at) return NULL;
    if (__atomic_load_n(&concurrent, __ATOMIC_RELAXED)) {
        struct timespec t = {0, 100000}; nanosleep(&t, NULL);
    }
    assert(n <= SIZE_MAX - sizeof(host_allocation));
    host_allocation *p = malloc(sizeof(*p) + n);
    if (!p) return NULL;
    p->size = n; live++; charged += n;
    if (charged > peak) peak = charged;
    return p + 1;
}
void os64_free(void *p)
{
    if (p) {
        host_allocation *h = (host_allocation *)p - 1;
        assert(live && charged >= h->size); live--; charged -= h->size; free(h);
    }
}
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *os64_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *os64_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
int64_t os64_sleep(uint64_t ms)
{
    assert(ms == 1); __atomic_add_fetch(&waits, 1, __ATOMIC_RELAXED);
    struct timespec t = {0, 1000000}; nanosleep(&t, NULL); return 0;
}
static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); assert(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f); assert(size >= 0); *n = (size_t)size; rewind(f);
    uint8_t *p = malloc(*n + 1); assert(p && fread(p, 1, *n, f) == *n); fclose(f); return p;
}
static void empty(const os64_webp_image_t *im)
{ assert(!im->pixels && !im->width && !im->height && !live); }
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
typedef struct { const uint8_t *p; size_t n; unsigned id; pthread_barrier_t *barrier; } job;
static void *worker(void *arg)
{
    job *j = arg;
    pthread_barrier_wait(j->barrier);
    for (unsigned i = 0; i < 5; ++i) {
        os64_webp_image_t im, retained;
        assert(os64_webp_decode(j->p, j->n, 0, 0, &retained) == OS64_WEBP_OK);
        uint32_t first = retained.pixels[0];
        fail_at = j->id % 2 ? calls + 2 : 0;
        os64_webp_status_t s = os64_webp_decode(j->p, j->n, 0, j->id % 2 ? 0 : 1, &im);
        assert(s == (j->id % 2 ? OS64_WEBP_NO_MEMORY : OS64_WEBP_LIMIT));
        assert(!im.pixels && retained.pixels[0] == first && live == 1);
        fail_at = 0; os64_webp_free(&retained); assert(!live);
    }
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    size_t n, rn;
    uint8_t *p = load(argv[1], &n), *ref = load(argv[2], &rn);
    os64_webp_image_t im;
    int large = argc == 4 && !strcmp(argv[3],"large");
    if (argc == 4 && !large) {
        /* Race the first call: the initialization and active allocation context
         * have not been touched before the barrier releases six callers. */
        pthread_barrier_t barrier; pthread_t threads[6]; job jobs[6];
        assert(!pthread_barrier_init(&barrier, NULL, 6));
        __atomic_store_n(&concurrent, 1, __ATOMIC_RELAXED);
        for (unsigned i = 0; i < 6; ++i) {
            jobs[i] = (job){p, n, i, &barrier};
            assert(!pthread_create(&threads[i], NULL, worker, &jobs[i]));
        }
        for (unsigned i = 0; i < 6; ++i) assert(!pthread_join(threads[i], NULL));
        assert(waits); pthread_barrier_destroy(&barrier);
        __atomic_store_n(&concurrent, 0, __ATOMIC_RELAXED);
    }
    os64_webp_status_t s = os64_webp_decode(p, n, 0, 0, &im);
    if (rn == 1) {
        if (s != ref[0]) fprintf(stderr, "%s got %s expected %u\n", argv[1], os64_webp_status_name(s), ref[0]);
        assert(s == ref[0]); empty(&im); free(p); free(ref); return 0;
    }
    if (s != OS64_WEBP_OK) fprintf(stderr, "%s: %s\n", argv[1], os64_webp_status_name(s));
    assert(s == OS64_WEBP_OK && rn >= 8);
    uint32_t w = le32(ref), h = le32(ref + 4);
    if (large) {
        assert(rn == 16 && im.width == w && im.height == h);
        uint64_t expected = le32(ref+8) | (uint64_t)le32(ref+12)<<32;
        uint64_t hash = UINT64_C(14695981039346656037);
        const uint8_t *pixels = (const uint8_t *)im.pixels;
        for (size_t i=0;i<(size_t)w*h*4;i++) hash=(hash^pixels[i])*UINT64_C(1099511628211);
        assert(hash == expected && peak <= OS64_WEBP_MEMORY_DEFAULT);
        os64_webp_free(&im); empty(&im); assert(!charged);
        printf("PASS wrapper %s peak=%zu bytes pixel_hash=%016llx\n",argv[1],peak,(unsigned long long)hash);
        free(p); free(ref); return 0;
    }
    assert(im.width == w && im.height == h && rn == 8 + (size_t)w*h*4);
    assert(!memcmp(im.pixels, ref + 8, rn - 8));
    os64_webp_free(&im); os64_webp_free(&im); empty(&im);
    assert(os64_webp_decode(p, n, (uint64_t)w*h-1, 0, &im) == OS64_WEBP_LIMIT); empty(&im);
    assert(os64_webp_decode(p, OS64_WEBP_INPUT_MAX + 1, 0, 0, &im) == OS64_WEBP_LIMIT); empty(&im);
    assert(os64_webp_decode(NULL, n, 0, 0, &im) == OS64_WEBP_BAD_ARGUMENT); empty(&im);
    assert(os64_webp_decode(p, n, 0, 0, NULL) == OS64_WEBP_BAD_ARGUMENT);
    size_t extent = (size_t)le32(p + 4) + 8;
    for (size_t end = 0; end < extent; ++end) {
        assert(os64_webp_decode(p, end, 0, 0, &im) != OS64_WEBP_OK); empty(&im);
    }
    for (fail_at = 1; fail_at < 500; ++fail_at) {
        calls = 0; s = os64_webp_decode(p, n, 0, 0, &im);
        if (s == OS64_WEBP_OK) { os64_webp_free(&im); empty(&im); break; }
        assert(s == OS64_WEBP_NO_MEMORY); empty(&im);
    }
    assert(fail_at < 500); fail_at = 0;
    size_t low = 1, high = OS64_WEBP_MEMORY_DEFAULT;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        s = os64_webp_decode(p, n, 0, mid, &im);
        if (s == OS64_WEBP_OK) { high = mid; os64_webp_free(&im); }
        else { assert(s == OS64_WEBP_LIMIT); low = mid + 1; }
        empty(&im);
    }
    assert(low > 1 && os64_webp_decode(p, n, 0, low-1, &im) == OS64_WEBP_LIMIT); empty(&im);
    assert(os64_webp_decode(p, n, 0, low, &im) == OS64_WEBP_OK); os64_webp_free(&im); empty(&im);
    uint8_t *mutated = malloc(n); assert(mutated); uint32_t seed = 0x64abc;
    for (unsigned i = 0; i < 150; ++i) {
        memcpy(mutated, p, n); seed = seed*1664525u+1013904223u;
        mutated[seed % n] ^= (uint8_t)(1u << ((seed >> 24) & 7));
        s = os64_webp_decode(mutated, n, 4096, 4u << 20, &im);
        if (s == OS64_WEBP_OK) os64_webp_free(&im);
        empty(&im);
    }
    assert(strstr(os64_webp_license(), "Redistribution") && strstr(os64_webp_license(), "Additional IP Rights Grant"));
    free(mutated); free(ref); free(p); return 0;
}
