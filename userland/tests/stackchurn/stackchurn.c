// stackchurn — a thread's stacks end with the thread (THREAD_STACKS.md).
// Thousands of threads are made and joined in one program, more than the
// address space once held: when every ended thread kept its stacks until
// the program exited, a program's stacks ran into its own image or the
// fixed windows after about 1,350 threads at most. Each proves its stack is
// its own:
//
//   1. threads one at a time: each fills a quarter megabyte of its stack
//      with a tag of its own, page by page, yields, and checks every tag;
//   2. the same from four makers at once, each thread also writing the
//      address of its fill into a table of live threads and checking it
//      against every other live one: no two live stacks overlap (two
//      threads handed one range would also overwrite each other's tags);
//   3. the program's fixed windows still read back: its argv strings and an
//      environment variable, compared with copies taken at the start, and a
//      caught SIGINT returning through the sigreturn window; the program's
//      own return goes through the exit window, which testrun's exit code
//      proves;
//   4. every thread made is still on the task's chain (/proc/self/status'
//      made count), and what the stacks took is given back: one live thread
//      once kworker has caught up, and /sys/memory/frames' free bytes
//      are back to where they began, less what each ended thread keeps
//      until burial (KEPT_PER_THREAD) and a FREE_MARGIN for the rest;
//   6. a released stack is gone from the program: a thread keeps writing
//      into a page of another thread's stack while that thread ends and is
//      reaped, and no write may land once the reap is seen done. See
//      stale_step for what it can and cannot show.
//
// `stackchurn` makes TURN_THREADS + MAKERS * EACH threads; `stackchurn long`
// four times as many.
//
// Exit codes: 0x57AC0000 success, 0x57AC00NN the step that failed, 0x57AC0005
// when /sys/memory/frames could not be read at the start.

#include "os64/os64.h"
#include "os64/fmt.h"
#include "os64/io.h"
#include "os64/proc.h"
#include "os64/signal.h"
#include "os64/str.h"
#include "os64/thread.h"

#define FAIL(step) return (int)(0x57AC0000u | (step))

#define TURN_THREADS 2000
#define MAKERS 4
#define EACH 500
#define FILL_PAGES 64                       // a quarter of the default stack
#define PAGE 4096u
#define LIVE_SLOTS (MAKERS)                 // one live thread per maker at a time
// What may stay taken: each ended thread's thread_t (a page) and its share
// of the page tables its never-reused stack addresses drew (THREAD_STACKS.md
// § Booked), until the program's burial, and what the rest of the machine
// takes meanwhile. An ended thread that kept its stacks would keep 1.1 MB.
#define KEPT_PER_THREAD (8ull * 1024)
#define FREE_MARGIN (32ull * 1024 * 1024)

static volatile uint64_t s_live_fill[LIVE_SLOTS];   // where each maker's current thread filled
static volatile int s_overlaps, s_bad_tags;

// A thread's body: `arg` is its tag in the high half, its live slot (or
// LIVE_SLOTS for none) in the low half.
static int64_t churn(void *arg)
{
    uint64_t tag = (uint64_t)(uintptr_t)arg & ~0xFFFFull;
    uint32_t slot = (uint32_t)((uintptr_t)arg & 0xFFFF);
    volatile uint64_t fill[FILL_PAGES * PAGE / sizeof(uint64_t)];
    const uint32_t per_page = PAGE / sizeof(uint64_t);
    for (uint32_t p = 0; p < FILL_PAGES; p++)
        fill[p * per_page] = tag | p;
    if (slot < LIVE_SLOTS) {
        uint64_t mine = (uint64_t)(uintptr_t)&fill[0];
        __atomic_store_n(&s_live_fill[slot], mine, __ATOMIC_RELEASE);
        for (uint32_t k = 0; k < LIVE_SLOTS; k++) {
            uint64_t other = __atomic_load_n(&s_live_fill[k], __ATOMIC_ACQUIRE);
            // Another live thread's fill within this one's: the same stack,
            // or overlapping ones (every stack is at least the fill's size).
            if (k != slot && other != 0 && other + sizeof(fill) > mine && mine + sizeof(fill) > other)
                __atomic_add_fetch(&s_overlaps, 1, __ATOMIC_RELAXED);
        }
    }
    os64_yield();
    for (uint32_t p = 0; p < FILL_PAGES; p++)
        if (fill[p * per_page] != (tag | p)) {
            __atomic_add_fetch(&s_bad_tags, 1, __ATOMIC_RELAXED);
            break;
        }
    if (slot < LIVE_SLOTS)
        __atomic_store_n(&s_live_fill[slot], 0, __ATOMIC_RELEASE);
    return (int64_t)(tag >> 16);
}

static bool make_and_join(uint64_t tag, uint32_t slot)
{
    int64_t h = os64_thread(churn, (void *)(uintptr_t)((tag << 16) | slot));
    if (h < 0)
        return false;
    int64_t rc = -1;
    bool ok = os64_thread_join((int32_t)h, &rc) >= 0 && rc == (int64_t)tag;
    os64_close((int32_t)h);
    return ok;
}

static volatile int s_maker_failures;
static uint32_t s_each;

static int64_t maker(void *arg)
{
    uint32_t m = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 0; i < s_each; i++)
        if (!make_and_join(((uint64_t)(m + 1) << 24) | i, m)) {
            __atomic_add_fetch(&s_maker_failures, 1, __ATOMIC_RELAXED);
            return -1;
        }
    return 0;
}

// The first number after `key` and a tab in a /proc or /sys text, or -1.
static char s_text[8192];
static int64_t read_text(const char *path)
{
    int64_t h = os64_open(path, "r");
    if (h < 0)
        return -1;
    int64_t have = 0, n;
    while (have < (int64_t)sizeof(s_text) - 1 && (n = os64_read((int32_t)h, s_text + have, sizeof(s_text) - 1 - have)) > 0)
        have += n;
    os64_close((int32_t)h);
    s_text[have] = 0;
    return have;
}

static const char *find(const char *text, const char *word)
{
    size_t k = os64_strlen(word);
    for (; *text; text++)
        if (os64_memcmp(text, word, k) == 0)
            return text;
    return NULL;
}

static const char *after(const char *text, const char *key)
{
    size_t k = os64_strlen(key);
    for (const char *line = text; line && *line;) {
        if (os64_memcmp(line, key, k) == 0)
            return line + k;
        line = os64_strchr(line, '\n');
        if (line)
            line++;
    }
    return NULL;
}

// "threads\t<made> made, <live> live": the live count, or -1, and the made
// count in *made.
static int64_t live_threads(int64_t *made)
{
    *made = -1;
    if (read_text("/proc/self/status") <= 0)
        return -1;
    const char *v = after(s_text, "threads\t");
    if (v == NULL)
        return -1;
    *made = (int64_t)os64_atou(v);
    const char *live = find(v, "made, ");
    return live ? (int64_t)os64_atou(live + 6) : -1;
}

// "books: usable U = free F + ...": F, or 0.
static uint64_t free_bytes(void)
{
    if (read_text("/sys/memory/frames") <= 0)
        return 0;
    const char *v = after(s_text, "books: usable ");
    const char *f = v ? find(v, "= free ") : NULL;
    return f ? os64_atou(f + 7) : 0;
}

// "reaps_pending\t<n>": n, or -1.
static int64_t reaps_pending(void)
{
    if (read_text("/proc/self/status") <= 0)
        return -1;
    const char *v = after(s_text, "reaps_pending\t");
    return v ? (int64_t)os64_atou(v) : -1;
}

// Step 6. The victim publishes a page of its own stack and waits to be let
// go; the toucher writes into that page until a write faults (its SIGSEGV
// handler ends it) or until it has made STALE_MOST writes after main said
// the victim's reap was done.
#define SEGV_EXIT 0x5E6F
#define STALE_EXIT 0x57A1E
#define STALE_MOST 1000
static volatile uint64_t *volatile s_victim_page;
static volatile int s_victim_go, s_victim_reaped;
static volatile uint64_t s_touches, s_stale;

static void on_segv(int signo)
{
    (void)signo;
    os64_thread_exit(SEGV_EXIT);
}

static int64_t victim(void *arg)
{
    (void)arg;
    volatile uint64_t room[2 * PAGE / sizeof(uint64_t)];   // holds one whole page
    room[0] = 0;
    s_victim_page = (volatile uint64_t *)(((uintptr_t)&room[0] + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    while (!__atomic_load_n(&s_victim_go, __ATOMIC_ACQUIRE))
        os64_yield();
    return 0;
}

static int64_t toucher(void *arg)
{
    (void)arg;
    volatile uint64_t *at = s_victim_page;
    for (uint64_t n = 1;; n++) {
        // Read the flag BEFORE the write: a write that lands after the
        // victim's reap was seen done is one no CPU should still allow.
        int reaped = __atomic_load_n(&s_victim_reaped, __ATOMIC_ACQUIRE);
        at[0] = n;
        __atomic_add_fetch(&s_touches, 1, __ATOMIC_RELAXED);
        if (reaped && __atomic_add_fetch(&s_stale, 1, __ATOMIC_RELAXED) >= STALE_MOST)
            return STALE_EXIT;
    }
}

// WHAT IT SHOWS: that a reaped stack is unmapped before the reap counts it
// done. A reap that freed the frames and left the stack mapped lets the
// toucher go on writing, into frames already someone else's, long after
// main sees the reap done: STALE (proven in QEMU by leaving the unmap out).
// WHAT IT DOES NOT SHOW: that the reap waits for every CPU to flush before
// it frees. Freeing a frame takes it out of the kernel's own map, which
// sends every CPU a flush without waiting for it, so the toucher's CPU drops
// its translation microseconds after the free whether the reap waited or
// not, and long before main's flag; with the wait taken out the step still
// passes, in QEMU and, for the same reason, on real hardware. The window
// the wait closes is that IPI's latency, which no timing here can reach.
static int stale_step(void)
{
    if (os64_signal_set_handler(OS64_SIGSEGV, on_segv) < 0)
        return -1;
    int64_t hv = os64_thread(victim, NULL);
    if (hv < 0)
        return -1;
    for (int i = 0; i < 1000 && s_victim_page == NULL; i++)
        os64_sleep(1);
    int64_t ht = s_victim_page ? os64_thread(toucher, NULL) : -1;
    if (ht < 0)
        return -1;
    for (int i = 0; i < 1000 && __atomic_load_n(&s_touches, __ATOMIC_RELAXED) < 100000; i++)
        os64_sleep(1);
    uint64_t before_go = __atomic_load_n(&s_touches, __ATOMIC_RELAXED);
    __atomic_store_n(&s_victim_go, 1, __ATOMIC_RELEASE);
    int64_t rc = -1;
    if (os64_thread_join((int32_t)hv, &rc) < 0 || rc != 0)
        return -1;
    os64_close((int32_t)hv);
    // The victim's reap is done when the task has no reap pending and only
    // main and the toucher are live (or only main, if the toucher already
    // faulted, which is the expected end).
    int64_t live = -1, made;
    bool seen = false;
    for (int i = 0; i < 500 && !seen; i++) {
        live = live_threads(&made);
        seen = reaps_pending() == 0 && live >= 1 && live <= 2;
        if (!seen)
            os64_sleep(10);
    }
    // Set even when the reap was not seen, so the toucher stops; the step
    // then fails for that reason, not the toucher's.
    __atomic_store_n(&s_victim_reaped, 1, __ATOMIC_RELEASE);
    rc = -1;
    if (os64_thread_join((int32_t)ht, &rc) < 0)
        return -1;
    os64_close((int32_t)ht);
    os64_signal_set_handler(OS64_SIGSEGV, OS64_SIG_DEFAULT);
    uint64_t after_go = __atomic_load_n(&s_touches, __ATOMIC_RELAXED) - before_go;
    if (!seen) {
        os64_printf("stackchurn: a dead thread's stack: its reap was not seen done in 5 s (%ld live)\n", (long)live);
        return -1;
    }
    os64_printf("stackchurn: a dead thread's stack: the toucher %s after %lu writes since the victim was let go, "
                "%lu after its reap was seen done\n", rc == SEGV_EXIT ? "faulted" : "did not fault",
                (unsigned long)after_go, (unsigned long)s_stale);
    return rc == SEGV_EXIT && s_stale == 0 ? 0 : -1;
}

static volatile int s_caught;
static void on_interrupt(int signo)
{
    s_caught = signo;
}

int main(int argc, char **argv)
{
    uint32_t scale = (argc > 1 && argv[1][0] == 'l') ? 4 : 1;
    s_each = EACH * scale;

    // Copies of what lives in the fixed windows, to read back at the end.
    static char argv0[128], path_env[256];
    os64_strcopy(argv0, sizeof(argv0), argv[0]);
    const char *path = os64_getenv("PATH");
    os64_strcopy(path_env, sizeof(path_env), path ? path : "");
    uint64_t free_before = free_bytes();
    if (free_before == 0)
        FAIL(5);

    for (uint32_t i = 0; i < TURN_THREADS * scale; i++)
        if (!make_and_join(i, LIVE_SLOTS)) {
            os64_printf("stackchurn: thread %u of the ones made in turn failed\n", i);
            FAIL(1);
        }
    if (s_bad_tags != 0) {
        os64_printf("stackchurn: %d threads made in turn lost a tag\n", s_bad_tags);
        FAIL(1);
    }

    int64_t makers[MAKERS];
    for (uintptr_t m = 0; m < MAKERS; m++)
        if ((makers[m] = os64_thread(maker, (void *)m)) < 0)
            FAIL(2);
    for (int m = 0; m < MAKERS; m++) {
        int64_t rc = -1;
        if (os64_thread_join((int32_t)makers[m], &rc) < 0 || rc != 0)
            s_maker_failures++;
        os64_close((int32_t)makers[m]);
    }
    if (s_maker_failures != 0 || s_bad_tags != 0 || s_overlaps != 0) {
        os64_printf("stackchurn: four makers at once: %d failed, %d lost a tag, %d overlapping live stacks\n",
                    s_maker_failures, s_bad_tags, s_overlaps);
        FAIL(2);
    }

    path = os64_getenv("PATH");
    if (!os64_streq(argv[0], argv0) || path == NULL || !os64_streq(path, path_env)) {
        os64_printf("stackchurn: argv[0] or PATH changed under the churn\n");
        FAIL(3);
    }
    if (os64_signal_set_handler(OS64_SIGINT, on_interrupt) < 0)
        FAIL(3);
    int64_t ctl = os64_open("/proc/self/ctl", "w");
    if (ctl < 0)
        FAIL(3);
    (void)os64_write((int32_t)ctl, "interrupt", 9);
    os64_close((int32_t)ctl);
    for (int i = 0; i < 100 && s_caught == 0; i++)
        os64_sleep(10);
    os64_signal_set_handler(OS64_SIGINT, OS64_SIG_DEFAULT);
    if (s_caught != OS64_SIGINT) {
        os64_printf("stackchurn: the SIGINT handler did not run and return\n");
        FAIL(3);
    }

    // kworker releases ended threads' stacks soon after they end, not at once.
    // Every thread made is on the task's chain until burial, main included: a
    // count short of that is a thread lost from the chain.
    int64_t live = -1, listed = -1;
    for (int i = 0; i < 500 && (live = live_threads(&listed)) != 1; i++)
        os64_sleep(10);
    uint64_t free_after = free_bytes();
    uint64_t made = TURN_THREADS * scale + MAKERS * s_each + MAKERS;
    os64_printf("stackchurn: %lu threads made and joined; /proc says %ld made, %ld live after; "
                "free bytes %lu before, %lu after\n", (unsigned long)made, (long)listed, (long)live,
                (unsigned long)free_before, (unsigned long)free_after);
    if (live != 1 || listed != (int64_t)made + 1)
        FAIL(4);
    if (free_after + FREE_MARGIN + made * KEPT_PER_THREAD < free_before) {
        os64_printf("stackchurn: %lu bytes did not come back\n", (unsigned long)(free_before - free_after));
        FAIL(4);
    }
    if (stale_step() != 0)
        FAIL(6);
    return (int)0x57AC0000u;
}
