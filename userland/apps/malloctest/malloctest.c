// malloctest — the heap, proven at ring 3 on real regions from the real
// kernel (2026-08-15). The engine's fine-grained cases are pinned down on the
// host by tools/test_heap_host.c, in milliseconds; what this fixture proves is
// everything the host cannot fake: that map/unmap really back it, that the
// guard pages and demand paging behave, that /proc/<pid>/heap renders what
// malloc published, and that a heap crime really does kill a task.
//
// Three personalities, chosen by argv so the roster can run all of them:
//
//   malloctest              the battery -> 0x0A110C00 ("ALLOC 00")
//   malloctest doublefree   frees one block twice -> must die 0xF12EEBAD
//   malloctest stomp        overruns a block -> must die 0xCA9A12ED
//   malloctest threads [n]  n threads (default 4) hammering ONE shared heap,
//                           including freeing each other's blocks -> 0x0A110C10
//   malloctest churn [secs] allocates and frees forever, so that
//                           `watch -n 1 "cat /proc/<pid>/heap"` is a live
//                           heap profiler built from tools that already exist
//
// The two crime modes PASS BY DYING. A fixture whose success is a corpse is
// unusual enough to say out loud: testrun compares the exit code, so "the
// tripwire fired, with the right badge" is a green line like any other, and a
// crime that DIDN'T kill the program shows up as a wrong code rather than as
// silence.

#include "os64/os64.h"
#include "os64/mem.h"

#define MALLOC_OK              0x0A110C00UL   // "ALLOC 00"
#define FAIL_NULL              0x0A110C01UL   // malloc returned NULL
#define FAIL_ALIGN             0x0A110C02UL   // payload not 16-byte aligned
#define FAIL_OVERLAP           0x0A110C03UL   // two live blocks share memory
#define FAIL_COALESCE          0x0A110C04UL   // neighbours did not merge
#define FAIL_RECYCLE           0x0A110C05UL   // a freed block was not reused
#define FAIL_CALLOC            0x0A110C06UL   // calloc returned non-zero bytes
#define FAIL_REALLOC           0x0A110C07UL   // realloc lost or moved data
#define FAIL_GIVEBACK          0x0A110C08UL   // an emptied region was not returned
#define FAIL_VERIFY            0x0A110C09UL   // the heap walk found problems
#define FAIL_PROC              0x0A110C0AUL   // /proc/<pid>/heap missing or wrong
#define FAIL_BIG               0x0A110C0BUL   // the dedicated-region path
#define FAIL_POISON            0x0A110C0CUL   // a freed body was not poisoned

// ── the battery ─────────────────────────────────────────────────────────────

static uint32_t test_basics(void)
{
    void *a = os64_malloc(1);
    void *b = os64_malloc(300);
    void *c = os64_malloc(5000);

    if (a == NULL || b == NULL || c == NULL)
        return FAIL_NULL;
    if (((uintptr_t)a | (uintptr_t)b | (uintptr_t)c) & 15)
        return FAIL_ALIGN;

    // Write every byte of all three, then read them back: any overlap between
    // blocks (a split gone wrong) shows up as a byte with the wrong stamp.
    os64_memset(a, 0xA1, 1);
    os64_memset(b, 0xB2, 300);
    os64_memset(c, 0xC3, 5000);

    if (*(uint8_t *)a != 0xA1)
        return FAIL_OVERLAP;
    for (int i = 0; i < 300; i++)
        if (((uint8_t *)b)[i] != 0xB2)
            return FAIL_OVERLAP;
    for (int i = 0; i < 5000; i++)
        if (((uint8_t *)c)[i] != 0xC3)
            return FAIL_OVERLAP;

    os64_free(b);
    os64_free(a);
    os64_free(c);
    os64_free(NULL);                 // a no-op since V7, and still

    void *z = os64_malloc(0);        // a real, freeable block — never NULL
    if (z == NULL)
        return FAIL_NULL;
    os64_free(z);

    return 0;
}

static uint32_t test_coalesce_and_recycle(void)
{
    // Three neighbours plus a guard; free all three and the span must come
    // back as ONE block big enough for the lot. This is the merge os32's
    // allocator never had, and the whole reason for the boundary tags.
    void *a = os64_malloc(400);
    void *b = os64_malloc(400);
    void *c = os64_malloc(400);
    void *guard = os64_malloc(400);

    if (a == NULL || b == NULL || c == NULL || guard == NULL)
        return FAIL_NULL;

    os64_free(a);
    os64_free(c);
    os64_free(b);                    // merge-both: the interesting case

    void *big = os64_malloc(1200);
    if (big != a)
        return FAIL_COALESCE;

    os64_free(big);
    os64_free(guard);

    void *p1 = os64_malloc(256);
    os64_free(p1);
    void *p2 = os64_malloc(256);
    if (p1 != p2)
        return FAIL_RECYCLE;

    // The poison: the freed body must read 0xA5, past the two words the
    // allocator reuses for its own list links.
    uint8_t *body = (uint8_t *)p2;
    os64_memset(body, 0x5C, 256);
    os64_free(p2);
    for (int i = 16; i < 200; i++)
        if (body[i] != 0xA5)
            return FAIL_POISON;

    return 0;
}

// Runs FIRST, while this process's heap is still virgin, because that is what
// the recipe needs: a CLEAN block (carved off the frontier, never freed) that
// grows in place over a freed neighbour and then splits. The leftover used to
// inherit the parent's "clean" flag while its memory came from the poisoned
// neighbour — and the next calloc trusted the flag and handed over 0xA5.
// Found by CHRIS's own malloc test on 2026-08-15, hours after the heap
// shipped; the cure is that a block joining the free list is marked dirty at
// the one choke point where the links get written, so no split, merge, or
// shrink can forget it.
static uint32_t test_calloc_never_leaks_predecessor(void)
{
    char *a = os64_malloc(100);          // virgin carve: CLEAN
    char *b = os64_malloc(1000);         // virgin carve: CLEAN
    if (a == NULL || b == NULL)
        return FAIL_NULL;

    os64_memset(a, 0x11, 100);
    os64_memset(b, 0x22, 1000);
    os64_free(b);                        // now free, poisoned 0xA5

    a = os64_realloc(a, 300);            // absorbs b, then splits
    if (a == NULL)
        return FAIL_REALLOC;

    uint8_t *c = os64_calloc(1, 400);    // lands in that leftover
    if (c == NULL)
        return FAIL_NULL;
    for (int i = 0; i < 400; i++)
        if (c[i] != 0)
            return FAIL_CALLOC;          // somebody else's bytes

    os64_free(c);
    os64_free(a);
    return 0;
}

static uint32_t test_calloc(void)
{
    // Fresh from a region's virgin frontier: the kernel already zeroed these
    // pages, so calloc gets to skip the memset entirely. Prove it is zero.
    uint8_t *p = os64_calloc(200, 4);
    if (p == NULL)
        return FAIL_NULL;
    for (int i = 0; i < 200 * 4; i++)
        if (p[i] != 0)
            return FAIL_CALLOC;

    os64_memset(p, 0xFF, 200 * 4);
    os64_free(p);

    // Recycled: now it holds poison, and calloc must clean it.
    uint8_t *q = os64_calloc(200, 4);
    if (q == NULL)
        return FAIL_NULL;
    for (int i = 0; i < 200 * 4; i++)
        if (q[i] != 0)
            return FAIL_CALLOC;
    os64_free(q);

    if (os64_calloc((size_t)-1 / 2, 4) != NULL)
        return FAIL_CALLOC;          // the multiply would wrap: must refuse

    return 0;
}

static uint32_t test_realloc(void)
{
    char *p = os64_malloc(100);
    if (p == NULL)
        return FAIL_NULL;
    os64_memset(p, 'R', 100);

    char *grown = os64_realloc(p, 300);
    if (grown == NULL)
        return FAIL_NULL;
    for (int i = 0; i < 100; i++)
        if (grown[i] != 'R')
            return FAIL_REALLOC;

    char *shrunk = os64_realloc(grown, 64);
    if (shrunk != grown)
        return FAIL_REALLOC;         // shrinking always happens in place

    // Force the copy path: block the successor, then ask for far more.
    char *blocker = os64_malloc(64);
    char *moved = os64_realloc(shrunk, 8192);
    if (moved == NULL)
        return FAIL_NULL;
    for (int i = 0; i < 64; i++)
        if (moved[i] != 'R')
            return FAIL_REALLOC;

    if (os64_realloc(moved, 0) != NULL)
        return FAIL_REALLOC;
    os64_free(blocker);
    return 0;
}

static uint32_t test_big_and_giveback(void)
{
    // At or above the threshold: a region of its own, and freeing it hands
    // that region straight back to the kernel.
    uint8_t *big = os64_malloc(512 * 1024);
    if (big == NULL)
        return FAIL_NULL;

    // Touch every page: demand paging means these are being faulted in for
    // real, one at a time, by the kernel that just promised them.
    for (int i = 0; i < 512 * 1024; i += 4096)
        big[i] = (uint8_t)i;
    for (int i = 0; i < 512 * 1024; i += 4096)
        if (big[i] != (uint8_t)i)
            return FAIL_BIG;
    os64_free(big);

    // Fill past one pool so a second one exists, then empty it: the extra
    // region must go home. (The primordial pool is kept on purpose.)
    enum { N = 32 };
    void *chunk[N];
    for (int i = 0; i < N; i++)
    {
        chunk[i] = os64_malloc(64 * 1024);
        if (chunk[i] == NULL)
            return FAIL_NULL;
        os64_memset(chunk[i], (uint8_t)i, 64 * 1024);
    }
    for (int i = 0; i < N; i++)
        os64_free(chunk[i]);

    return 0;
}

// ── /proc/<pid>/heap ────────────────────────────────────────────────────────

// Find "key\tvalue" in the report and return the value, or ~0 if absent.
static uint64_t heap_field(const char *text, const char *key)
{
    size_t klen = os64_strlen(key);

    for (const char *p = text; *p != '\0'; p++)
    {
        bool at_line_start = (p == text) || (p[-1] == '\n');
        if (!at_line_start)
            continue;

        size_t i = 0;
        while (i < klen && p[i] == key[i])
            i++;
        if (i == klen && p[i] == '\t')
            return os64_atou(p + i + 1);
    }
    return ~0ULL;
}

static uint32_t test_proc_heap(void)
{
    // Hold a known amount of memory, then ask the KERNEL what our heap looks
    // like. If the two agree, the whole chain works: malloc published the
    // report, the kernel walked our page tables to read it, and procfs
    // rendered it as text.
    void *held[4];
    for (int i = 0; i < 4; i++)
    {
        held[i] = os64_malloc(1000);
        if (held[i] == NULL)
            return FAIL_NULL;
    }

    char path[64];
    os64_snprintf(path, sizeof(path), "/proc/%lu/heap", os64_taskid());

    int32_t h = (int32_t)os64_open(path, "r");
    if (h < 0)
        return FAIL_PROC;

    char text[2048];
    int64_t n = os64_read(h, text, sizeof(text) - 1);
    os64_close(h);
    if (n <= 0)
        return FAIL_PROC;
    text[n] = '\0';

    // The kernel checks malloc's books for us and writes the verdict into the
    // file: every mapped byte must be live, free, overhead, or never-carved.
    // Anything but "ok" here is the allocator's own accounting caught lying.
    const char *audit = text;
    bool audit_ok = false;
    for (const char *p = text; *p != '\0'; p++)
        if ((p == text || p[-1] == '\n') && p[0] == 'a' && p[1] == 'u'
            && p[2] == 'd' && p[3] == 'i' && p[4] == 't' && p[5] == '\t')
        {
            audit = p + 6;
            audit_ok = (audit[0] == 'o' && audit[1] == 'k');
            break;
        }
    if (!audit_ok)
        return FAIL_PROC;

    uint64_t live = heap_field(text, "blocks_live");
    uint64_t regions = heap_field(text, "regions");
    uint64_t taken = heap_field(text, "regions_taken");
    uint64_t returned = heap_field(text, "regions_returned");

    if (live == ~0ULL || regions == ~0ULL || taken == ~0ULL || returned == ~0ULL)
        return FAIL_PROC;            // a key the file is supposed to carry
    if (live < 4)
        return FAIL_PROC;            // we are holding four blocks right now
    if (regions < 1)
        return FAIL_PROC;
    if (returned == 0)
        return FAIL_GIVEBACK;        // the earlier tests gave regions back
    if (taken < returned)
        return FAIL_PROC;            // more returned than taken is nonsense

    os64_printf("malloctest: /proc heap says %lu live blocks, %lu regions "
                "(%lu taken, %lu returned)\n", live, regions, taken, returned);

    for (int i = 0; i < 4; i++)
        os64_free(held[i]);
    return 0;
}

// ── the crimes ──────────────────────────────────────────────────────────────

static void crime_double_free(void)
{
    void *p = os64_malloc(128);
    os64_free(p);
    os64_free(p);       // must not return: 0xF12EEBAD
    os64_printf("malloctest: a double free was SURVIVED — the tripwire is dead\n");
    os64_exit(1);
}

static void crime_stomp(void)
{
    char *a = os64_malloc(64);
    char *b = os64_malloc(64);
    (void)b;

    // Run 32 bytes off the end of `a`, straight into `b`'s header. The canary
    // is tied to the block's address AND size, so the clobbered header cannot
    // accidentally validate.
    os64_memset(a, 0xCC, 64 + 32);
    os64_free(a);       // must not return: 0xCA9A12ED
    os64_printf("malloctest: a stomped canary was SURVIVED — the tripwire is dead\n");
    os64_exit(1);
}

// ── threads: one address space, one heap, many hands ────────────────────────
//
// The question this answers (Chris, 2026-08-15): threads share the task's
// address space, so they share ONE heap — libos64's heap state is ordinary
// process globals, and every thread of a task sees the same `gRegions`, the
// same free list, the same pools. There is no per-thread heap and no
// inheritance step; a thread does not get a copy of anything.
//
// Which means malloc must be genuinely thread-safe, and this is the fixture
// that proves it rather than assuming it:
//
//   - Every thread hammers the shared pools with its own private allocations,
//     stamping and verifying each one, so a lock that lets two threads carve
//     the same bytes shows up as a stamp that isn't ours.
//   - Threads also HAND BLOCKS TO EACH OTHER through a shared slot table and
//     free each other's memory — the case a per-thread-cache allocator has to
//     think hard about, and which this one gets for free by having exactly
//     one heap behind one lock.
//
// The handoff table needs its own synchronization, and userland has no mutex
// yet (deliberately — DEBTS), so the fixture uses an atomic pointer exchange:
// publishing and claiming are the same instruction, and exactly one thread
// can win a slot. That is the whole protocol.

#define THREADS_OK        0x0A110C10UL
#define FAIL_THREAD_START 0x0A110C11UL
#define FAIL_THREAD_JOIN  0x0A110C12UL
#define FAIL_THREAD_DATA  0x0A110C13UL

#define HANDOFF_SLOTS 16
#define HANDOFF_STAMP 0xC5

static void *volatile gHandoff[HANDOFF_SLOTS];
static volatile int64_t gThreadFailures;

// A handed-over block: its length in the first 8 bytes, then HANDOFF_STAMP
// all the way to the end — self-describing, so the thread that claims it can
// check every byte without knowing who allocated it or how big it was.
static void *handoff_make(size_t len)
{
    uint8_t *p = os64_malloc(len);
    if (p == NULL)
        return NULL;
    *(uint64_t *)p = (uint64_t)len;
    for (size_t i = 8; i < len; i++)
        p[i] = HANDOFF_STAMP;
    return p;
}

static bool handoff_check_and_free(void *v)
{
    uint8_t *p = (uint8_t *)v;
    uint64_t len = *(uint64_t *)p;

    if (len < 64 || len > 8192)
    {
        os64_printf("malloctest: handoff length implausible: %lu\n", len);
        return false;
    }
    for (uint64_t i = 8; i < len; i++)
        if (p[i] != HANDOFF_STAMP)
        {
            os64_printf("malloctest: handoff byte %lu of %lu is 0x%02x, not 0x%02x\n",
                        i, len, p[i], HANDOFF_STAMP);
            return false;
        }
    os64_free(p);
    return true;
}

static int64_t heap_worker_body(void *arg, int rounds)
{
    uint32_t rng = 0x0A110C00u + (uint32_t)(uint64_t)arg * 2654435761u;

    for (int round = 0; round < rounds; round++)
    {
        rng = rng * 1664525u + 1013904223u;
        size_t n = 24 + (rng >> 7) % 1200;
        uint8_t stamp = (uint8_t)((uint64_t)arg + round);

        // Private: allocate, own it, verify nobody else wrote in it, release.
        uint8_t *p = ((rng & 7) == 0) ? os64_calloc(1, n) : os64_malloc(n);
        if (p == NULL)
        {
            gThreadFailures++;
            return FAIL_NULL;
        }
        if ((rng & 7) == 0)
            for (size_t i = 0; i < n; i++)
                if (p[i] != 0)                      // calloc, under contention
                {
                    gThreadFailures++;
                    os64_printf("malloctest: threaded calloc returned non-zero\n");
                    return FAIL_CALLOC;
                }

        for (size_t i = 0; i < n; i++)
            p[i] = stamp;
        for (size_t i = 0; i < n; i++)
            if (p[i] != stamp)
            {
                gThreadFailures++;
                os64_printf("malloctest: thread %lu had a block stomped at +%lu\n",
                            (uint64_t)arg, i);
                return FAIL_THREAD_DATA;
            }
        os64_free(p);

        // Cross-thread: publish one block into a slot and claim whatever was
        // there. One atomic exchange does both — whoever gets the old pointer
        // owns it, and frees memory another thread allocated.
        rng = rng * 1664525u + 1013904223u;
        int slot = (int)((rng >> 9) % HANDOFF_SLOTS);
        void *mine = handoff_make(64 + (rng >> 3) % 2000);
        if (mine == NULL)
        {
            gThreadFailures++;
            return FAIL_NULL;
        }
        void *theirs = __atomic_exchange_n(&gHandoff[slot], mine, __ATOMIC_ACQ_REL);
        if (theirs != NULL && !handoff_check_and_free(theirs))
        {
            gThreadFailures++;
            return FAIL_THREAD_DATA;
        }
    }
    return 0;
}

// THE BISECTION FIXTURE (2026-08-15). The threaded heap test faulted inside
// heap_lock's contention path — specifically at the instruction after its
// os64_yield syscall — which accuses either the heap's lock or the kernel's
// yield-under-threads path, and those are very different bugs. This worker
// touches NO heap at all: it just yields in a loop. If it faults the same
// way, malloc is innocent.
static int64_t yield_worker(void *arg)
{
    for (int i = 0; i < 200000; i++)
        os64_yield();
    return (int64_t)(uint64_t)arg * 0;
}

static uint32_t test_yieldstorm(uint64_t count)
{
    int64_t handle[8];

    if (count < 2) count = 2;
    if (count > 8) count = 8;

    for (uint64_t i = 0; i < count; i++)
    {
        handle[i] = os64_thread(yield_worker, (void *)i);
        if (handle[i] < 0)
            return FAIL_THREAD_START;
    }
    for (uint64_t i = 0; i < count; i++)
    {
        int64_t answer = -1;
        if (os64_thread_join((int32_t)handle[i], &answer) != 0)
            return FAIL_THREAD_JOIN;
    }
    os64_printf("malloctest: %lu threads survived a yield storm\n", count);
    return 0;
}

static int64_t heap_worker(void *arg)
{
    return heap_worker_body(arg, 3000);
}

// WARMUP=1 in the environment runs one worker iteration on the MAIN thread
// before any thread starts, so every code page this test executes is already
// resident. It exists as a DIAGNOSTIC: if a threaded failure disappears when
// the text is pre-faulted, the bug is in demand paging under concurrent
// faults, not in whatever the threads were doing. (2026-08-15 — that is
// exactly what it proved.)
static uint32_t test_threads(uint64_t count)
{
    int64_t handle[8];

    if (count < 2) count = 2;
    if (count > 8) count = 8;

    const char *warm = os64_getenv("WARMUP");
    if (warm != NULL && warm[0] == '1')
    {
        heap_worker_body((void *)0, 3);
        os64_printf("malloctest: text pre-faulted on the main thread\n");
    }

    for (uint64_t i = 0; i < count; i++)
    {
        handle[i] = os64_thread(heap_worker, (void *)i);
        if (handle[i] < 0)
            return FAIL_THREAD_START;
    }

    for (uint64_t i = 0; i < count; i++)
    {
        int64_t answer = -1;
        if (os64_thread_join((int32_t)handle[i], &answer) != 0)
            return FAIL_THREAD_JOIN;
        if (answer != 0)
            return (uint32_t)answer;
    }

    // Collect whatever is still sitting in the handoff table.
    for (int i = 0; i < HANDOFF_SLOTS; i++)
    {
        void *left = __atomic_exchange_n(&gHandoff[i], (void *)0, __ATOMIC_ACQ_REL);
        if (left != NULL && !handoff_check_and_free(left))
            return FAIL_THREAD_DATA;
    }

    if (gThreadFailures != 0)
        return FAIL_THREAD_DATA;
    if (os64_heap_verify() != 0)
        return FAIL_VERIFY;

    os64_printf("malloctest: %lu threads shared one heap cleanly\n", count);
    return 0;
}

// ── churn: something to watch ───────────────────────────────────────────────

// Hold a rotating population of blocks of wildly different sizes, so every
// number in /proc/<pid>/heap moves: live bytes breathe, the histogram fills
// across its buckets, fragmentation rises and falls, and regions are taken
// and given back. Runs until killed (`echo kill > /proc/<pid>/ctl`, or Ctrl+C
// if it is in the foreground), or for `seconds` if one is given.
static void churn(uint64_t seconds)
{
    enum { SLOTS = 64 };
    void *slot[SLOTS];
    size_t len[SLOTS];
    uint32_t rng = 0x0A110C00;

    // The stopwatch, never the calendar (os64/ticks.h): ticks and their rate
    // arrive together in one call, so a deadline is honest arithmetic rather
    // than a hardcoded 100.
    os64_ticks_t now;
    os64_ticks(&now);
    uint64_t deadline = seconds ? now.ticks + seconds * now.per_second : 0;

    for (int i = 0; i < SLOTS; i++)
    {
        slot[i] = NULL;
        len[i] = 0;
    }

    for (;;)
    {
        rng = rng * 1664525u + 1013904223u;
        int i = (int)((rng >> 8) % SLOTS);

        if (slot[i] != NULL)
        {
            // Check our own stamp on the way out: if a neighbour has scribbled
            // on us, this loop is where it shows, live, on screen.
            uint8_t want = (uint8_t)i;
            for (size_t k = 0; k < len[i]; k += 64)
                if (((uint8_t *)slot[i])[k] != want)
                {
                    os64_printf("malloctest: block %d was stomped at +%lu\n", i, k);
                    os64_exit((int)FAIL_OVERLAP);
                }
            os64_free(slot[i]);
            slot[i] = NULL;
        }
        else
        {
            rng = rng * 1664525u + 1013904223u;
            // One in thirty is over the big threshold, so the dedicated-region
            // path (and the give-back) shows up in the watched numbers too.
            size_t n = ((rng >> 8) % 30 == 0)
                ? (size_t)(160 * 1024 + (rng % 65536))
                : (size_t)(16 + (rng >> 6) % 3000);

            slot[i] = os64_malloc(n);
            if (slot[i] == NULL)
            {
                os64_printf("malloctest: out of memory at %lu bytes\n", (uint64_t)n);
                os64_exit((int)FAIL_NULL);
            }
            os64_memset(slot[i], (uint8_t)i, n);
            len[i] = n;
        }

        os64_sleep(20);   // slow enough for a human and a `watch` to follow

        if (deadline)
        {
            os64_ticks(&now);
            if (now.ticks >= deadline)
                break;
        }
    }

    for (int i = 0; i < SLOTS; i++)
        os64_free(slot[i]);
    os64_printf("malloctest: churn done\n");
}

// ── main ────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    if (argc > 1 && os64_streq(argv[1], "doublefree"))
        crime_double_free();
    if (argc > 1 && os64_streq(argv[1], "stomp"))
        crime_stomp();
    if (argc > 1 && os64_streq(argv[1], "yieldstorm"))
    {
        uint32_t r = test_yieldstorm(argc > 2 ? os64_atou(argv[2]) : 4);
        return r != 0 ? (int)r : (int)THREADS_OK;
    }
    if (argc > 1 && os64_streq(argv[1], "threads"))
    {
        uint32_t r = test_threads(argc > 2 ? os64_atou(argv[2]) : 4);
        if (r != 0)
            return (int)r;
        return (int)THREADS_OK;
    }
    if (argc > 1 && os64_streq(argv[1], "churn"))
    {
        churn(argc > 2 ? (uint64_t)os64_atou(argv[2]) : 0);
        return 0;
    }

    uint32_t r;
    // FIRST, before anything dirties the heap — it needs virgin carvings.
    if ((r = test_calloc_never_leaks_predecessor()) != 0) return (int)r;
    if ((r = test_basics()) != 0)               return (int)r;
    if ((r = test_coalesce_and_recycle()) != 0) return (int)r;
    if ((r = test_calloc()) != 0)               return (int)r;
    if ((r = test_realloc()) != 0)              return (int)r;
    if ((r = test_big_and_giveback()) != 0)     return (int)r;

    if (os64_heap_verify() != 0)
        return (int)FAIL_VERIFY;

    if ((r = test_proc_heap()) != 0)            return (int)r;

    if (os64_heap_verify() != 0)
        return (int)FAIL_VERIFY;

    os64_printf("malloctest: heap OK\n");
    return (int)MALLOC_OK;
}
