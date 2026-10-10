#ifndef OS64_WEBP_BENCH_H
#define OS64_WEBP_BENCH_H
/* Private instrumentation for the statically linked guest benchmarks.
 * These fields and entry point are absent from the production library. */
#include "webp/webp.h"
typedef struct {
    int64_t gate_us, allocate_us, scratch_free_us;
} webp_bench_profile_t;
os64_webp_status_t webp_bench_decode(const uint8_t *data, size_t length,
    uint64_t pixel_cap, size_t memory_cap, os64_webp_image_t *out,
    webp_bench_profile_t *profile);
#endif
