// Host test of the handle table's arithmetic and counts
// (kernel/src/handle_blocks.c), beside a model that walks the blocks one
// entry at a time: every number from below 0 to past the ceiling lands
// where the model says, in a block of the model's size, and no two numbers
// share an entry; the room after each block is the model's sum; and the
// counts, taken and given from several threads at once, end where they
// began with `most` the highest `held` any thread saw. Run by
// tools/test_handle_blocks_host.sh.

#include "handle_blocks.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failed;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            s_failed++;                                                                \
        }                                                                              \
    } while (0)

// The model: block 0 holds 64, each block after it twice the last.
static uint32_t model_size(unsigned k) { return k == 0 ? 64u : 64u << (k - 1); }

static void arithmetic_cases(void)
{
    CHECK(OS64_TASK_HANDLES == 64 && TASK_HANDLES_CEILING == 4096 && TASK_HANDLE_BLOCKS == 6);
    uint32_t first = 0;
    for (unsigned k = 0; k <= TASK_HANDLE_BLOCKS; k++) {
        CHECK(handle_block_size(k) == model_size(k));
        CHECK(handle_block_first(k) == first);
        first += model_size(k);
        CHECK(handle_room(k) == first);
    }
    CHECK(first == TASK_HANDLES_CEILING);
    // Every number, walked entry by entry beside the model.
    static uint8_t seen[TASK_HANDLES_CEILING];
    memset(seen, 0, sizeof(seen));
    unsigned block = 0;
    uint32_t slot = 0;
    for (int64_t n = 0; n < TASK_HANDLES_CEILING; n++) {
        unsigned b = 99;
        uint32_t s = 99;
        CHECK(handle_place(n, &b, &s));
        CHECK(b == block && s == slot && s < handle_block_size(b));
        uint32_t back = handle_block_first(b) + s;
        CHECK(back == (uint32_t)n && !seen[back]);
        seen[back] = 1;
        if (++slot == model_size(block)) {
            block++;
            slot = 0;
        }
    }
    // Numbers no task can hold, and their outputs untouched.
    static const int64_t outside[] = {-1, -64, INT32_MIN, TASK_HANDLES_CEILING, TASK_HANDLES_CEILING + 1, 1ll << 40};
    for (unsigned i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
        unsigned b = 77;
        uint32_t s = 77;
        CHECK(!handle_place(outside[i], &b, &s) && b == 77 && s == 77);
    }
    // The edges by name: each block's first and last.
    unsigned b;
    uint32_t s;
    CHECK(handle_place(63, &b, &s) && b == 0 && s == 63);
    CHECK(handle_place(64, &b, &s) && b == 1 && s == 0);
    CHECK(handle_place(127, &b, &s) && b == 1 && s == 63);
    CHECK(handle_place(128, &b, &s) && b == 2 && s == 0);
    CHECK(handle_place(2047, &b, &s) && b == 5 && s == 1023);
    CHECK(handle_place(2048, &b, &s) && b == 6 && s == 0);
    CHECK(handle_place(4095, &b, &s) && b == 6 && s == 2047);
}

static handle_counts_t s_counts;
static uint32_t s_seen_most;

static void *taker(void *arg)
{
    unsigned rounds = (unsigned)(uintptr_t)arg;
    for (unsigned r = 0; r < rounds; r++) {
        for (int i = 0; i < 8; i++)
            handle_counts_took(&s_counts);
        uint32_t held = __atomic_load_n(&s_counts.held, __ATOMIC_RELAXED);
        uint32_t seen = __atomic_load_n(&s_seen_most, __ATOMIC_RELAXED);
        while (held > seen && !__atomic_compare_exchange_n(&s_seen_most, &seen, held, true, __ATOMIC_RELAXED,
                                                           __ATOMIC_RELAXED)) {
        }
        for (int i = 0; i < 8; i++)
            handle_counts_gave(&s_counts);
        handle_counts_refused(&s_counts);
    }
    return NULL;
}

static void counts_cases(void)
{
    handle_counts_t c = {.held = 3, .most = 3};
    handle_counts_took(&c);
    CHECK(c.held == 4 && c.most == 4 && c.failed == 0);
    handle_counts_gave(&c);
    handle_counts_gave(&c);
    CHECK(c.held == 2 && c.most == 4);
    handle_counts_took(&c);
    handle_counts_took(&c);
    CHECK(c.held == 4 && c.most == 4);
    handle_counts_took(&c);
    CHECK(c.held == 5 && c.most == 5);
    handle_counts_refused(&c);
    handle_counts_refused(&c);
    CHECK(c.failed == 2 && c.held == 5 && c.most == 5);
    // From several threads at once: everything taken is given back, every
    // refusal counted, and most is at least the highest held any saw and
    // never more than all of them holding together.
    enum { THREADS = 8, ROUNDS = 20000 };
    s_counts = (handle_counts_t){.held = 3, .most = 3};
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        CHECK(pthread_create(&th[i], NULL, taker, (void *)(uintptr_t)ROUNDS) == 0);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    CHECK(s_counts.held == 3);
    CHECK(s_counts.failed == (uint64_t)THREADS * ROUNDS);
    CHECK(s_counts.most >= s_seen_most && s_counts.most >= 11 && s_counts.most <= 3 + 8 * THREADS);
}

int main(void)
{
    arithmetic_cases();
    counts_cases();
    if (s_failed != 0) {
        fprintf(stderr, "handle blocks: %d failed\n", s_failed);
        return 1;
    }
    printf("handle blocks: all cases passed\n");
    return 0;
}
