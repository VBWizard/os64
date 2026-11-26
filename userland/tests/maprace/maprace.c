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
//   4. unmap racing a fault in its own block, for the time asked: a thread
//      faults fresh pages into a 256-page block one at a time, from the top
//      down, while this one unmaps it, and dies of SIGSEGV at its first
//      touch after (the handler ends the thread). Then a thread reads the
//      page it was faulting in when it stopped, and another the page above
//      (where it was, if that fault went through and it went on), and each
//      must die of SIGSEGV too: a page that reads was installed by a fault
//      after unmap passed it, left mapped in a range no mapping covers, its
//      frame lost. At most RACE_ROUNDS rounds: a thread that ends keeps its
//      stacks until the task does (DEBTS, "No PER-THREAD teardown"), and the
//      task's stack addresses run into its fixed windows after about 1,400
//      (DEBTS, "Thread stacks have no ceiling").
//      From the top down because unmap walks up: the pages below the one
//      being faulted are not there yet, so unmap's walk reaches it while
//      that fault may still be under way, which is the race.
//
// Exit codes: 0x3A9E0000 success, 0x3A9E00NN the step that failed.

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/signal.h"
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

// Step 4. A thread's touch of a page with no mapping is a SIGSEGV, whose
// handler ends that thread with SEGV_EXIT; this thread never touches.
#define RACE_PAGES 256
#define RACE_ROUNDS 300   // three threads a round, well under the stacks' ceiling
#define SEGV_EXIT 0x5E6F
#define TOUCHED_ALL 1
#define PAGE_READ 2

static uint8_t *volatile s_race_block;
static volatile uint32_t s_race_at, s_race_page;   // pages touched; the page being touched
static uint64_t s_race_rounds, s_race_cut, s_race_probes;

static void on_segv(int signo)
{
    (void)signo;
    os64_thread_exit(SEGV_EXIT);
}

static int64_t race_toucher(void *arg)
{
    (void)arg;
    uint8_t *block = s_race_block;
    for (uint32_t n = 0; n < RACE_PAGES; n++) {
        uint32_t p = RACE_PAGES - 1 - n;
        __atomic_store_n(&s_race_page, p, __ATOMIC_RELEASE);
        __atomic_store_n(&s_race_at, n, __ATOMIC_RELEASE);
        ((volatile uint64_t *)(block + (size_t)p * PAGE))[0] = p;
    }
    return TOUCHED_ALL;
}

static int64_t race_prober(void *arg)
{
    (void)((volatile uint64_t *)arg)[0];
    return PAGE_READ;
}

static int64_t join_one(int64_t (*fn)(void *), void *arg)
{
    int64_t h = os64_thread(fn, arg);
    if (h < 0)
        return -1;
    int64_t rc = -1;
    if (os64_thread_join((int32_t)h, &rc) < 0)
        rc = -1;
    os64_close((int32_t)h);
    return rc;
}

static bool unmap_race(int64_t seconds)
{
    if (os64_signal_set_handler(OS64_SIGSEGV, on_segv) < 0)
        return false;
    int64_t until = os64_micros() + seconds * 1000000;
    bool ok = true;
    for (uint64_t round = 0; ok && round < RACE_ROUNDS && os64_micros() < until; round++) {
        uint8_t *block = os64_map((size_t)RACE_PAGES * PAGE);
        if (block == NULL)
            return false;
        s_race_block = block;
        __atomic_store_n(&s_race_at, 0, __ATOMIC_RELEASE);
        int64_t h = os64_thread(race_toucher, NULL);
        if (h < 0)
            return false;
        // Let it get some way in, a different way each round, and most
        // rounds near the bottom, where unmap's walk reaches it soonest.
        uint32_t want = (round % 4) != 0 ? RACE_PAGES - 2 - (uint32_t)((round * 7) % 24)
                                         : 8 + (uint32_t)((round * 37) % (RACE_PAGES - 16));
        int64_t give_up = os64_micros() + 200000;
        while (__atomic_load_n(&s_race_at, __ATOMIC_ACQUIRE) < want && os64_micros() < give_up) {
        }
        if (os64_unmap(block) != 0)
            ok = false;
        int64_t rc = -1;
        if (os64_thread_join((int32_t)h, &rc) < 0)
            ok = false;
        os64_close((int32_t)h);
        if (rc == SEGV_EXIT)
            s_race_cut++;
        else if (rc != TOUCHED_ALL)
            ok = false;
        // Where it stopped, and the page above: neither may still be mapped.
        uint32_t at = __atomic_load_n(&s_race_page, __ATOMIC_ACQUIRE);
        for (uint32_t p = at; p <= at + 1 && p < RACE_PAGES; p++) {
            int64_t probe = join_one(race_prober, block + (size_t)p * PAGE);
            s_race_probes++;
            if (probe != SEGV_EXIT) {
                os64_printf("maprace: page %u of an unmapped block %s (round %lu)\n", (unsigned)p,
                            probe == PAGE_READ ? "still reads: a fault installed it after unmap passed it"
                                               : "could not be probed",
                            (unsigned long)round);
                ok = false;
            }
        }
        s_race_rounds++;
    }
    os64_printf("maprace: unmap racing a fault, %ld s or %u rounds: %lu rounds, %lu cut short by the unmap, "
                "%lu pages probed after it\n", (long)seconds, (unsigned)RACE_ROUNDS, (unsigned long)s_race_rounds, (unsigned long)s_race_cut,
                (unsigned long)s_race_probes);
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
    if (!unmap_race(seconds))
        FAIL(4);
    return (int)0x3A9E0000u;
}
