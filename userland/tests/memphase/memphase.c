// Separate lazy mapping, page faults, and release costs without image codecs.
// Timings are wall time, including contention when six workers share a heap.
#include "os64/os64.h"
#include "os64/str.h"

#define BYTES (64UL * 1024 * 1024)
#define ROUNDS 3
#define WORKERS 6

typedef struct {
    int64_t allocate, touch, release;
} sample_t;

static sample_t samples[WORKERS][ROUNDS];
static unsigned ready, start;
static bool heap_mode, touch_pages;

static int64_t worker(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    __atomic_fetch_add(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE)) os64_sleep(1);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        int64_t t0 = os64_micros();
        void *p = heap_mode ? os64_malloc(BYTES) : os64_map(BYTES);
        int64_t t1 = os64_micros();
        if (!p) return 1;
        if (touch_pages) {
            volatile unsigned char *bytes = p;
            for (size_t offset = 0; offset < BYTES; offset += 4096)
                bytes[offset] = (unsigned char)round;
        }
        int64_t t2 = os64_micros();
        int64_t result = 0;
        if (heap_mode) os64_free(p);
        else result = os64_unmap(p);
        int64_t t3 = os64_micros();
        samples[id][round] = (sample_t){t1 - t0, t2 - t1, t3 - t2};
        if (result < 0) return 2;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4 || (!os64_streq(argv[1], "map") && !os64_streq(argv[1], "heap")) ||
        (!os64_streq(argv[2], "untouched") && !os64_streq(argv[2], "touch")) ||
        (!os64_streq(argv[3], "1") && !os64_streq(argv[3], "6"))) {
        os64_printf("usage: memphase map|heap untouched|touch 1|6\n");
        return 2;
    }
    heap_mode = os64_streq(argv[1], "heap");
    touch_pages = os64_streq(argv[2], "touch");
    unsigned count = os64_streq(argv[3], "6") ? WORKERS : 1;
    // Raw map/unmap bypasses the heap's serialization of region metadata.
    if (!heap_mode && count != 1) {
        os64_printf("map mode requires one worker; use heap mode for contention.\n");
        return 2;
    }
    int64_t threads[WORKERS];
    unsigned launched = 0;
    int rc = 0;
    os64_printf("memphase mode=%s pages=%s workers=%u bytes=%lu rounds=%u\n",
                argv[1], argv[2], count, BYTES, ROUNDS);
    for (; launched < count; ++launched) {
        threads[launched] = os64_thread(worker, (void *)(uintptr_t)launched);
        if (threads[launched] < 0) { rc = 3; break; }
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) < launched) os64_sleep(1);
    int64_t begin = os64_micros();
    __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < launched; ++i) {
        int64_t answer = -1;
        if (os64_thread_join((int32_t)threads[i], &answer) < 0 || answer) rc = 4;
        os64_close((int32_t)threads[i]);
    }
    int64_t elapsed = os64_micros() - begin;
    for (unsigned i = 0; i < launched; ++i)
        for (unsigned r = 0; r < ROUNDS; ++r)
            os64_printf("worker=%u round=%u allocate_us=%ld touch_us=%ld release_us=%ld\n",
                        i, r, samples[i][r].allocate, samples[i][r].touch,
                        samples[i][r].release);
    uint64_t problems = os64_heap_verify();
    if (problems) rc = 5;
    os64_printf("elapsed_us=%ld heap_problems=%lu result=%d\n", elapsed, problems, rc);
    os64_sync_all();
    return rc;
}
