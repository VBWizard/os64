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
//      until burial (KEPT_PER_THREAD) and a FREE_MARGIN for the rest.
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
    return (int)0x57AC0000u;
}
