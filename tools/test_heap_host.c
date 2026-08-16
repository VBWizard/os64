// test_heap_host.c — HOST-side unit test for libos64's heap (heap.c).
//
// The allocator is pure computation over memory it gets from exactly three
// functions (os64_map, os64_unmap, os64_heap_publish) plus a handful of
// reporting calls — so it can be compiled with the host's gcc, given host
// memory to carve, and exercised in milliseconds. That matters more here than
// anywhere else in the tree: a boundary-tag allocator's interesting cases are
// SPLIT, MERGE-FORWARD, MERGE-BACKWARD, MERGE-BOTH and give-back, and finding
// out which one broke by rebooting a kernel is how weeks disappear.
//
// The deaths are testable too: the os64_exit stub longjmps back into the
// harness with the exit code, so "a double free kills the program with
// 0xF12EEBAD" is an assertion here rather than a hope.
//
// Build & run:
//   gcc -g -I userland/libos64/include -I abi/include
//       userland/libos64/heap.c userland/libos64/fmt.c userland/libos64/str.c
//       tools/test_heap_host.c -o /tmp/os64_heap_test    (one line)
//   /tmp/os64_heap_test
//
// (The real thing then gets proven in the OS by /bin/malloctest, which runs
// the same shapes at ring 3 on real regions from the real kernel.)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>

#include "os64/heap.h"
#include "os64/mem.h"

// ── The stubs: os64's world, faked just enough ──────────────────────────────

#define FAKE_REGION_MAX 64
static struct { void *base; size_t len; } gFake[FAKE_REGION_MAX];
static int gFakeCount;
static int gMapCalls, gUnmapCalls;

void *os64_map(size_t len)
{
    // The kernel's contract, honoured: page-rounded and ZEROED.
    len = (len + 4095) & ~(size_t)4095;
    void *p = aligned_alloc(4096, len);
    if (p == NULL)
        return NULL;
    memset(p, 0, len);

    gFake[gFakeCount].base = p;
    gFake[gFakeCount].len = len;
    gFakeCount++;
    gMapCalls++;
    return p;
}

int64_t os64_unmap(void *base)
{
    for (int i = 0; i < gFakeCount; i++)
        if (gFake[i].base == base)
        {
            // Poison before release: anything still pointing in here should
            // read obvious garbage, exactly as the never-reused region VAs do
            // in the real OS.
            memset(base, 0xDD, gFake[i].len);
            free(base);
            gFake[i] = gFake[--gFakeCount];
            gUnmapCalls++;
            return 0;
        }
    fprintf(stderr, "STUB: unmap of an address that was never mapped: %p\n", base);
    exit(2);
}

static const os64_heap_report_t *gReport;
int64_t os64_heap_publish(const os64_heap_report_t *report)
{
    gReport = report;
    return 0;
}

long os64_write(int handle, const void *buf, size_t len)
{
    (void)handle;
    fwrite(buf, 1, len, stdout);
    return (long)len;
}

void os64_serial_log(const char *s) { (void)s; }
void os64_yield(void) { }
// Straight through to the host's environment, so `HEAPCHECK=1 ./os64_heap_test`
// exercises the verify-every-call mode exactly as `export HEAPCHECK=1` does
// inside the OS.
const char *os64_getenv(const char *key) { return getenv(key); }

// The death hook: heap_die calls os64_exit, and the harness catches it.
static jmp_buf gDeathLanding;
static int gExpectingDeath;
static int32_t gDeathCode;

void os64_exit(int32_t code)
{
    if (gExpectingDeath)
    {
        gDeathCode = code;
        longjmp(gDeathLanding, 1);
    }
    fprintf(stderr, "UNEXPECTED os64_exit(0x%X)\n", (unsigned)code);
    exit(3);
}

// ── Test scaffolding ────────────────────────────────────────────────────────

static int gFailures, gChecks;

#define CHECK(cond, ...) do {                                   \
        gChecks++;                                              \
        if (!(cond)) {                                          \
            gFailures++;                                        \
            printf("  FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// Run `body` expecting the heap to kill the program with `code`.
#define EXPECT_DEATH(code, body) do {                                   \
        gChecks++;                                                      \
        gExpectingDeath = 1;                                            \
        gDeathCode = 0;                                                 \
        if (setjmp(gDeathLanding) == 0) {                               \
            body;                                                       \
            gFailures++;                                                \
            printf("  FAIL %s:%d: expected death 0x%X, survived\n",     \
                   __func__, __LINE__, (unsigned)(code));               \
        } else if ((uint32_t)gDeathCode != (uint32_t)(code)) {          \
            gFailures++;                                                \
            printf("  FAIL %s:%d: died 0x%X, expected 0x%X\n",          \
                   __func__, __LINE__, (unsigned)gDeathCode,            \
                   (unsigned)(code));                                   \
        }                                                               \
        gExpectingDeath = 0;                                            \
    } while (0)

// THE FRONTIER TAG. Runs first, on a virgin heap, so both blocks below are
// carved straight off a pool's frontier — the path that used to guess its
// PREV_FREE bit by reading the 8 bytes underneath it. In an IN-USE block
// those bytes are the program's data, so a program whose last word spells a
// plausible block size could hand itself a boundary tag that lies. Here the
// data spells 64 on purpose. (Found on the real OS by malloctest's churn
// mode, minutes after the 20,000-round soak below had passed clean.)
static void t_frontier_tag(void)
{
    uint64_t *first = os64_malloc(64);
    CHECK(first != NULL, "malloc returned NULL");
    for (int i = 0; i < 8; i++)
        first[i] = 64;                      // every word a plausible footer

    uint8_t *second = os64_malloc(64);
    CHECK(second != NULL, "malloc returned NULL");
    CHECK((uint8_t *)first + 80 == second,
          "expected the second block carved directly above the first");

    CHECK(os64_heap_verify() == 0, "a lying tag survived the carve");

    // The free that used to walk backwards into live data.
    os64_free(second);
    for (int i = 0; i < 8; i++)
        CHECK(first[i] == 64, "word %d of the live block was eaten by a merge", i);

    CHECK(os64_heap_verify() == 0, "heap does not verify after the free");
    os64_free(first);
}

static void t_basics(void)
{
    os64_heap_init();
    CHECK(gReport != NULL, "the report was never published");
    CHECK(gReport->magic == OS64_HEAP_REPORT_MAGIC, "report magic wrong");
    CHECK(gReport->version == OS64_HEAP_REPORT_VERSION, "report version wrong");
    CHECK((gReport->generation & 1) == 0, "generation left odd (torn)");

    void *a = os64_malloc(1);
    void *b = os64_malloc(100);
    void *c = os64_malloc(1000);
    CHECK(a && b && c, "malloc returned NULL for a small request");
    CHECK(((uintptr_t)a % 16) == 0 && ((uintptr_t)b % 16) == 0
          && ((uintptr_t)c % 16) == 0, "payload not 16-byte aligned");
    CHECK(a != b && b != c, "malloc handed out the same block twice");

    memset(a, 0x11, 1);
    memset(b, 0x22, 100);
    memset(c, 0x33, 1000);

    CHECK(gReport->blocks_live == 3, "blocks_live = %lu, want 3",
          (unsigned long)gReport->blocks_live);
    CHECK(os64_heap_verify() == 0, "heap does not verify after three mallocs");

    os64_free(a); os64_free(b); os64_free(c);
    CHECK(gReport->blocks_live == 0, "blocks_live = %lu after freeing all",
          (unsigned long)gReport->blocks_live);
    CHECK(gReport->bytes_live == 0, "bytes_live = %lu after freeing all",
          (unsigned long)gReport->bytes_live);
    CHECK(os64_heap_verify() == 0, "heap does not verify after the frees");

    // malloc(0) is a real block, and freeing it is not a special case.
    void *z = os64_malloc(0);
    CHECK(z != NULL, "malloc(0) returned NULL");
    os64_free(z);
    os64_free(NULL);                 // must be a no-op, not a crime
}

static void t_recycle_and_coalesce(void)
{
    // Three neighbours; free the outer two, then the middle one, and the
    // whole span must come back as ONE block — the merge-both case, which is
    // the one os32's allocator never had.
    void *a = os64_malloc(200);
    void *b = os64_malloc(200);
    void *c = os64_malloc(200);
    void *guard = os64_malloc(200);   // keeps the trio away from the frontier
    CHECK(a && b && c && guard, "setup allocation failed");

    uint64_t free_before = gReport->bytes_free;
    os64_free(a);
    os64_free(c);
    CHECK(gReport->blocks_free >= 2, "two frees did not produce two free blocks");

    os64_free(b);
    CHECK(os64_heap_verify() == 0, "heap does not verify after merge-both");

    // The merged block must be big enough to serve all three at once, which
    // proves they actually joined rather than merely being adjacent.
    void *big = os64_malloc(600);
    CHECK(big == a, "the merged block was not reused for the combined request");
    os64_free(big);
    os64_free(guard);
    (void)free_before;

    // Exact recycling: free then re-malloc the same size gets the same block.
    void *p1 = os64_malloc(128);
    os64_free(p1);
    void *p2 = os64_malloc(128);
    CHECK(p1 == p2, "a freed block was not reused for an identical request");
    os64_free(p2);
}

static void t_calloc(void)
{
    // Fresh from the frontier: the pages are kernel-zeroed and nobody has
    // written to them, so calloc must return zeroes without a memset.
    uint8_t *p = os64_calloc(64, 4);
    CHECK(p != NULL, "calloc returned NULL");
    int nonzero = 0;
    for (int i = 0; i < 64 * 4; i++)
        if (p[i] != 0)
            nonzero++;
    CHECK(nonzero == 0, "calloc of fresh memory returned %d non-zero bytes", nonzero);

    memset(p, 0xEE, 64 * 4);
    os64_free(p);

    // Recycled: the block now holds poison, so calloc has to zero it.
    uint8_t *q = os64_calloc(64, 4);
    CHECK(q == p, "expected the recycled block");
    nonzero = 0;
    for (int i = 0; i < 64 * 4; i++)
        if (q[i] != 0)
            nonzero++;
    CHECK(nonzero == 0, "calloc of recycled memory returned %d non-zero bytes", nonzero);
    os64_free(q);

    CHECK(os64_calloc((size_t)-1 / 2, 4) == NULL, "calloc overflow not refused");
}

// CALLOC MUST NEVER HAND BACK A PREDECESSOR'S BYTES — the regression for the
// bug Chris's own malloc test found, hours after the heap shipped.
//
// The recipe needs a CLEAN block (carved from virgin frontier, never freed)
// that grows in place over a freed neighbour and then splits: the leftover
// used to inherit the parent's "clean" flag while its memory came from the
// poisoned neighbour, and the next calloc trusted the flag. The free list is
// drained first so the two blocks below really are virgin carvings wherever
// this test runs in the suite.
static void t_calloc_never_leaks_predecessor(void)
{
    void *drain[64];
    int drained = 0;

    while (gReport->blocks_free > 0 && drained < 64)
    {
        size_t take = (size_t)gReport->largest_free;
        drain[drained] = os64_malloc(take);
        if (drain[drained] == NULL)
            break;
        drained++;
    }
    CHECK(gReport->blocks_free == 0, "could not drain the free list (%lu left)",
          (unsigned long)gReport->blocks_free);

    char *a = os64_malloc(100);      // virgin carve: CLEAN, never freed
    char *b = os64_malloc(1000);     // virgin carve: CLEAN
    CHECK(a && b, "setup allocation failed");
    memset(a, 0x11, 100);
    memset(b, 0x22, 1000);
    os64_free(b);                    // now free, poisoned 0xA5

    a = os64_realloc(a, 300);        // absorbs b, splits — who owns the tail?
    CHECK(a != NULL, "realloc failed");

    uint8_t *c = os64_calloc(1, 400);   // lands in that leftover
    CHECK(c != NULL, "calloc failed");
    int nonzero = 0;
    for (int i = 0; i < 400; i++)
        if (c[i] != 0)
            nonzero++;
    CHECK(nonzero == 0, "calloc returned %d bytes of somebody else's data", nonzero);

    os64_free(c);
    os64_free(a);
    while (drained > 0)
        os64_free(drain[--drained]);
}

static void t_poison(void)
{
    uint8_t *p = os64_malloc(256);
    memset(p, 0x77, 256);
    os64_free(p);

    // The body — past the two list links the allocator reuses — must now
    // read as poison. This is the tripwire that turns a use-after-free from
    // "worked by luck" into "obviously wrong data".
    int poisoned = 0;
    for (int i = 16; i < 200; i++)
        if (p[i] == 0xA5)
            poisoned++;
    CHECK(poisoned == 200 - 16, "freed body not poisoned (%d of %d bytes)",
          poisoned, 200 - 16);
}

static void t_realloc(void)
{
    char *p = os64_malloc(100);
    memset(p, 'A', 100);

    // Grow in place: the block above is free (nothing else has been allocated
    // since), so the tags let it absorb without a copy.
    char *g = os64_realloc(p, 200);
    CHECK(g == p, "realloc did not grow in place when it could");
    int intact = 1;
    for (int i = 0; i < 100; i++)
        if (g[i] != 'A')
            intact = 0;
    CHECK(intact, "realloc lost the original bytes while growing");

    // Shrink always happens in place.
    char *s = os64_realloc(g, 48);
    CHECK(s == g, "realloc moved the block while shrinking");

    // Forced copy: block the successor first, then ask for far more.
    char *blocker = os64_malloc(64);
    char *moved = os64_realloc(s, 4096);
    CHECK(moved != NULL, "realloc copy path returned NULL");
    intact = 1;
    for (int i = 0; i < 48; i++)
        if (moved[i] != 'A')
            intact = 0;
    CHECK(intact, "realloc copy path lost the original bytes");

    void *from_null = os64_realloc(NULL, 32);
    CHECK(from_null != NULL, "realloc(NULL, n) should malloc");
    CHECK(os64_realloc(moved, 0) == NULL, "realloc(p, 0) should free and return NULL");
    os64_free(from_null);
    os64_free(blocker);
    CHECK(os64_heap_verify() == 0, "heap does not verify after the realloc battery");
}

// SHRINK MUST COALESCE ITS TAIL — the regression for the bug mallochavoc's
// give-back complaints exposed (2026-08-16, ~30 "region empty but its free
// space did not merge" lines per run). realloc's shrink is the ONE maker of
// free blocks whose successor's state is not structurally guaranteed: the
// shrinking block was LIVE, so the block above it may be free, and the first
// version inserted the tail right against it. Two adjacent free blocks pass
// every tag check — honest sizes, honest canaries, consistent bits — which is
// also why the verifier grew an adjacent-free tripwire in the same fix. This
// test fails against the old code twice over: the verifier now names the
// crime, and the combined-request malloc lands elsewhere.
static void t_shrink_coalesce(void)
{
    // A live block with a FREE successor: a, b, guard — then free b.
    char *a = os64_malloc(4096);
    char *b = os64_malloc(256);
    char *guard = os64_malloc(64);       // keeps b away from the frontier
    CHECK(a && b && guard, "setup allocation failed");
    os64_free(b);

    uint64_t free_blocks_before = gReport->blocks_free;

    // Shrink a in place: the tail lands directly against free b and must
    // absorb it — one free block afterwards, not two.
    char *s = os64_realloc(a, 64);
    CHECK(s == a, "realloc moved the block while shrinking");
    CHECK(gReport->blocks_free == free_blocks_before,
          "shrink left %lu free blocks, want %lu (tail did not absorb its free neighbour)",
          (unsigned long)gReport->blocks_free, (unsigned long)free_blocks_before);
    CHECK(os64_heap_verify() == 0, "adjacent free blocks after a shrink");

    // The abandoned bytes read as poison, exactly as free() would have left
    // them — a use-after-shrink must not find its old data intact.
    int poisoned = 1;
    for (int i = 96; i < 200; i++)          // past the tail's header and links
        if ((uint8_t)a[i] != 0xA5)
            poisoned = 0;
    CHECK(poisoned, "the shrink's tail was not poisoned");

    // And the merged block serves a request neither piece could alone:
    // a's old span (4112) minus its kept 80 plus b's 272 = 4304 bytes of
    // block. The payload lands just past the kept block's header+payload.
    char *big = os64_malloc(4200);
    CHECK(big == a + 80, "the merged tail was not reused for the combined request");

    os64_free(big);
    os64_free(guard);
    os64_free(a);
    CHECK(os64_heap_verify() == 0, "heap does not verify after the cleanup");
}

static void t_big_and_giveback(void)
{
    uint64_t regions_before = gReport->regions;
    uint64_t unmaps_before  = gReport->calls_unmap;

    // At or above the threshold: its own region, and its free is one unmap.
    void *big = os64_malloc(256 * 1024);
    CHECK(big != NULL, "big malloc returned NULL");
    CHECK(gReport->region_dedicated == 1, "big allocation did not get its own region");
    CHECK(gReport->regions == regions_before + 1, "region count did not grow");
    memset(big, 0x5A, 256 * 1024);

    os64_free(big);
    CHECK(gReport->region_dedicated == 0, "dedicated region survived its free");
    CHECK(gReport->calls_unmap == unmaps_before + 1,
          "freeing a big block did not give the region back");
    CHECK(gReport->regions == regions_before, "region count did not return");

    // Force a SECOND pool by exhausting the first, then empty it: the extra
    // pool must go home to the kernel. (The primordial pool is kept on
    // purpose — see HEAP_REGION_KEEP.)
    enum { N = 40 };
    void *chunk[N];
    for (int i = 0; i < N; i++)
    {
        chunk[i] = os64_malloc(64 * 1024);   // just under the big threshold
        CHECK(chunk[i] != NULL, "pool-filling allocation %d failed", i);
    }
    CHECK(gReport->region_pools >= 2, "40 x 64KB did not force a second pool");

    uint64_t pools_peak = gReport->region_pools;
    for (int i = 0; i < N; i++)
        os64_free(chunk[i]);

    CHECK(gReport->region_pools < pools_peak,
          "no pool was given back after everything in it was freed");
    CHECK(gReport->region_pools >= 1, "the primordial pool was given away");
    CHECK(os64_heap_verify() == 0, "heap does not verify after the give-back");
}

static void t_crimes(void)
{
    // Double free: named as such, and fatal.
    void *p = os64_malloc(64);
    os64_free(p);
    EXPECT_DEATH(0xF12EEBAD, os64_free(p));

    // A pointer this heap never handed out.
    int stack_object = 0;
    EXPECT_DEATH(0xF12EEBAD, os64_free(&stack_object));

    // Misaligned: inside a region, but not on a payload boundary.
    void *q = os64_malloc(64);
    EXPECT_DEATH(0xF12EEBAD, os64_free((char *)q + 1));
    os64_free(q);

    // The stomp: run off the end of one block into the next block's header,
    // then free the overflower. The canary is address- and size-tied, so the
    // clobbered header cannot possibly validate.
    char *a = os64_malloc(64);
    char *b = os64_malloc(64);
    CHECK(b > a, "expected b above a");
    memset(a, 0xCC, 64 + 32);            // 32 bytes past the end: into b's header
    EXPECT_DEATH(0xCA9A12ED, os64_free(a));
    (void)b;

    // NOTE for whoever reads this output: the complaints printed by this
    // function are the POINT, and so is the "books do not balance" line from
    // the verify below. Each EXPECT_DEATH longjmps out of the middle of a
    // free(), skipping the accounting that would have finished it — in a real
    // process the program is simply gone at that instant, so nobody is left
    // to disagree with the numbers. In here, the auditor is.
    //
    // The heap is now genuinely corrupt — which makes this the only moment
    // the VERIFIER can be tested against the disease it exists to diagnose.
    // It must REPORT, not crash: a clobbered header's size field is a wild
    // number, and stepping by it walks off the region. (It did, the first
    // time HEAPCHECK=1 ran over this very test.)
    CHECK(os64_heap_verify() > 0, "the verifier found nothing wrong with a stomped heap");
}

// The test that actually finds allocator bugs: thousands of allocations and
// frees in an unpredictable order, every block stamped with a pattern derived
// from its own identity and checked on the way out. A split that hands back
// overlapping memory, a merge that swallows a live neighbour, or a footer
// written one word wide all show up here as a stamp that doesn't match —
// long before they would show up in the OS as a mystery.
static void t_soak(void)
{
    enum { SLOTS = 256, ROUNDS = 20000 };
    static struct { uint8_t *p; size_t len; uint8_t stamp; } slot[SLOTS];
    uint32_t rng = 0x05CA9A12;          // fixed seed: a failure is reproducible

    for (int round = 0; round < ROUNDS; round++)
    {
        rng = rng * 1664525u + 1013904223u;         // Knuth's LCG, fittingly
        int i = (int)((rng >> 8) % SLOTS);

        if (slot[i].p != NULL)
        {
            // Verify the stamp survived its neighbours, then release.
            int bad = 0;
            for (size_t k = 0; k < slot[i].len; k++)
                if (slot[i].p[k] != slot[i].stamp)
                    bad++;
            if (bad)
            {
                CHECK(0, "round %d: block %d (%zu bytes) had %d stomped bytes",
                      round, i, slot[i].len, bad);
                return;
            }
            os64_free(slot[i].p);
            slot[i].p = NULL;
            continue;
        }

        rng = rng * 1664525u + 1013904223u;
        // Mostly small, occasionally over the big-region threshold — so the
        // dedicated-region path gets soaked alongside the carving path.
        size_t len = ((rng >> 8) % 100 == 0)
            ? (size_t)(150 * 1024 + (rng % 4096))
            : (size_t)(1 + (rng >> 4) % 900);

        uint8_t *p = os64_malloc(len);
        if (p == NULL)
        {
            CHECK(0, "round %d: malloc(%zu) returned NULL", round, len);
            return;
        }
        slot[i].p = p;
        slot[i].len = len;
        slot[i].stamp = (uint8_t)(i ^ round);
        memset(p, slot[i].stamp, len);

        if ((round % 2000) == 0)
            CHECK(os64_heap_verify() == 0, "round %d: heap does not verify", round);
    }

    for (int i = 0; i < SLOTS; i++)
        if (slot[i].p != NULL)
            os64_free(slot[i].p);

    CHECK(os64_heap_verify() == 0, "heap does not verify after the soak");
    CHECK(gReport->blocks_live == 0, "soak left %lu blocks live",
          (unsigned long)gReport->blocks_live);
    CHECK(gReport->bytes_live == 0, "soak left %lu bytes live",
          (unsigned long)gReport->bytes_live);
    // Everything is free, so all but the primordial pool should have gone
    // home — the give-back working under churn rather than in a set piece.
    CHECK(gReport->regions == 1, "soak left %lu regions mapped, want 1",
          (unsigned long)gReport->regions);
    printf("  soak: %lu mallocs, %lu frees, %lu regions taken, %lu given back\n",
           (unsigned long)gReport->calls_malloc, (unsigned long)gReport->calls_free,
           (unsigned long)gReport->calls_map, (unsigned long)gReport->calls_unmap);
}

int main(void)
{
    printf("libos64 heap — host tests\n");
    t_frontier_tag();   // first: it needs a virgin heap to carve at the frontier
    t_basics();
    t_recycle_and_coalesce();
    t_calloc();
    t_calloc_never_leaks_predecessor();
    t_poison();
    t_realloc();
    t_shrink_coalesce();
    t_big_and_giveback();
    t_soak();
    t_crimes();     // last: it leaves the heap deliberately corrupted

    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures != 0;
}
