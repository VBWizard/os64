// maprace — threads of one task mapping at once: each gets a range of its
// own (task_va_reserve, the task's one door for its heap range), and the
// task's list of mappings holds while they map, fault, read it and unmap
// (MAPPING_LIST.md). Run by testrun, for two seconds; `maprace long` runs
// the list's race for a minute, and `maprace <seconds>` for that long.
//
//   1. four threads each map 2000 single pages, writing their own tag into
//      every page they were given;
//   2. no base was handed out twice, across all of them, and every page
//      still holds the tag its thread wrote (two threads given one range
//      would overwrite each other's);
//   3. the list's race, for the time asked: two threads map 64-page blocks,
//      tag every page and unmap them again; two fault fresh pages into
//      512-page blocks one at a time, each fault walking the list while the
//      first two link and delist, then check every tag and unmap; one reads
//      /proc/self/maps and /proc/self/status, whose walks take the same
//      lock. Every page keeps the tag written into it.
//
// Exit codes: 0x3A9E0000 success, 0x3A9E00NN the step that failed.

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/thread.h"

#define THREADS 4
#define MAPS 2000
#define FAIL(step) return (int)(0x3A9E0000u | (step))

#define PAGE 4096u
#define CHURN_PAGES 64
#define FAULT_PAGES 512

static uintptr_t s_bases[THREADS][MAPS];
static volatile int s_go;

static int64_t mapper(void *arg)
{
    uintptr_t who = (uintptr_t)arg;
    while (!__atomic_load_n(&s_go, __ATOMIC_ACQUIRE)) {
    }
    for (int i = 0; i < MAPS; i++) {
        uint64_t *page = os64_map(4096);
        if (page == NULL)
            return -1;
        page[0] = (who << 32) | (uint64_t)i;
        s_bases[who][i] = (uintptr_t)page;
    }
    return 0;
}

// Step 3's clock and its counts.
static int64_t s_until_us;
static volatile uint64_t s_blocks, s_faults, s_reads;

static bool time_left(void)
{
    return os64_micros() < s_until_us;
}

// A block mapped, `pages` pages tagged with `tag` and the page's index,
// every tag checked, and the block unmapped. False when a tag was lost or a
// call refused.
static bool block_round(uint64_t tag, uint32_t pages, bool count_faults)
{
    uint8_t *block = os64_map((size_t)pages * PAGE);
    if (block == NULL)
        return false;
    for (uint32_t p = 0; p < pages; p++) {
        ((volatile uint64_t *)(block + (size_t)p * PAGE))[0] = tag | p;
        if (count_faults)
            __atomic_add_fetch(&s_faults, 1, __ATOMIC_RELAXED);
    }
    for (uint32_t p = 0; p < pages; p++)
        if (((volatile uint64_t *)(block + (size_t)p * PAGE))[0] != (tag | p))
            return false;
    if (os64_unmap(block) != 0)
        return false;
    __atomic_add_fetch(&s_blocks, 1, __ATOMIC_RELAXED);
    return true;
}

static int64_t churner(void *arg)
{
    uint64_t tag = ((uint64_t)(uintptr_t)arg << 48);
    for (uint64_t round = 0; time_left(); round++)
        if (!block_round(tag | (round << 16), CHURN_PAGES, false))
            return -1;
    return 0;
}

static int64_t faulter(void *arg)
{
    uint64_t tag = ((uint64_t)(uintptr_t)arg << 48);
    for (uint64_t round = 0; time_left(); round++)
        if (!block_round(tag | (round << 16), FAULT_PAGES, true))
            return -1;
    return 0;
}

static int64_t reader(void *arg)
{
    (void)arg;
    static char buf[16384];
    static const char *const files[] = { "/proc/self/maps", "/proc/self/status" };
    for (uint64_t n = 0; time_left(); n++) {
        int64_t h = os64_open(files[n & 1], "r");
        if (h < 0)
            return -1;
        while (os64_read((int32_t)h, buf, sizeof(buf)) > 0) {
        }
        os64_close((int32_t)h);
        __atomic_add_fetch(&s_reads, 1, __ATOMIC_RELAXED);
    }
    return 0;
}

static bool list_race(int64_t seconds)
{
    static int64_t (*const bodies[])(void *) = { churner, churner, faulter, faulter, reader };
    enum { N = sizeof(bodies) / sizeof(bodies[0]) };
    int64_t th[N];
    s_until_us = os64_micros() + seconds * 1000000;
    for (uintptr_t k = 0; k < N; k++)
        if ((th[k] = os64_thread(bodies[k], (void *)(k + 1))) < 0)
            return false;
    bool ok = true;
    for (int k = 0; k < N; k++) {
        int64_t rc = -1;
        if (os64_thread_join((int32_t)th[k], &rc) < 0 || rc != 0)
            ok = false;
        os64_close((int32_t)th[k]);
    }
    os64_printf("maprace: the list's race, %ld s: %lu blocks mapped, tagged and unmapped, %lu fresh pages "
                "faulted in, %lu reads of /proc\n",
                (long)seconds, (unsigned long)s_blocks, (unsigned long)s_faults, (unsigned long)s_reads);
    return ok;
}

int main(int argc, char **argv)
{
    int64_t seconds = 2;
    if (argc > 1) {
        if (argv[1][0] == 'l')
            seconds = 60;
        else {
            seconds = 0;
            for (const char *c = argv[1]; *c >= '0' && *c <= '9'; c++)
                seconds = seconds * 10 + (*c - '0');
            if (seconds <= 0)
                seconds = 2;
        }
    }

    int64_t th[THREADS];
    for (uintptr_t k = 0; k < THREADS; k++)
        if ((th[k] = os64_thread(mapper, (void *)k)) < 0)
            FAIL(1);
    __atomic_store_n(&s_go, 1, __ATOMIC_RELEASE);
    for (int k = 0; k < THREADS; k++) {
        int64_t rc = -1;
        if (os64_thread_join((int32_t)th[k], &rc) < 0 || rc != 0)
            FAIL(1);
        os64_close((int32_t)th[k]);
    }
    for (uintptr_t k = 0; k < THREADS; k++)
        for (int i = 0; i < MAPS; i++)
            if (((uint64_t *)s_bases[k][i])[0] != ((k << 32) | (uint64_t)i))
                FAIL(2);
    // Every base against every other (a range given twice is two equal
    // bases: every map here asks for the same one page).
    const uintptr_t *all = &s_bases[0][0];
    for (int i = 0; i < THREADS * MAPS; i++)
        for (int j = i + 1; j < THREADS * MAPS; j++)
            if (all[i] == all[j])
                FAIL(2);

    if (!list_race(seconds))
        FAIL(3);
    return (int)0x3A9E0000u;
}
