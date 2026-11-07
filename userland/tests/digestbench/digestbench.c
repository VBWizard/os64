/* digestbench: how long crypto.subtle's digest holds a page (WEB_CRYPTO.md
 * § 5). yonder does the work on the call, so a page's digest blocks it for
 * as long as the hash runs; this times the same code, libdom's
 * subtle_core.c over the BearSSL objects libdom links, on the machine it
 * runs on. digestbench [MiB ...], 1 16 64 128 192 when none is given; each
 * size hashed once by every algorithm, its time and rate printed. */
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"

#include "../../libdom/subtle_core.c"

static void bench(size_t mib)
{
    size_t length = mib << 20;
    uint8_t *data = os64_malloc(length);
    if (data == NULL) {
        os64_printf("%u MiB: could not allocate\n", (unsigned)mib);
        return;
    }
    for (size_t i = 0; i < length; i++) data[i] = (uint8_t)(i * 131 + (i >> 12));
    for (int alg = 0; alg < SUBTLE_ALG_COUNT; alg++) {
        uint8_t out[SUBTLE_DIGEST_MAX];
        int64_t t0 = os64_micros();
        subtle_digest((subtle_alg_t)alg, data, length, out);
        int64_t us = os64_micros() - t0;
        uint64_t rate = us > 0 ? (uint64_t)length * 1000000u / (uint64_t)us >> 20 : 0;
        os64_printf("%4u MiB  %-8s %8u.%03u ms  %5u MiB/s  %02x%02x%02x%02x...\n", (unsigned)mib,
                    subtle_alg_name((subtle_alg_t)alg), (unsigned)(us / 1000), (unsigned)(us % 1000),
                    (unsigned)rate, out[0], out[1], out[2], out[3]);
    }
    os64_free(data);
}

int main(int argc, char **argv)
{
    static const size_t kSizes[] = {1, 16, 64, 128, 192};
    if (argc < 2) {
        for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) bench(kSizes[i]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        size_t mib = 0;
        for (const char *p = argv[i]; *p >= '0' && *p <= '9'; p++) mib = mib * 10 + (size_t)(*p - '0');
        if (mib == 0 || mib > 4096) {
            os64_printf("digestbench: %s is not a size in MiB (1 to 4096)\n", argv[i]);
            return 2;
        }
        bench(mib);
    }
    return 0;
}
