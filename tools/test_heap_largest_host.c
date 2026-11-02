/* Check the published maximum against a physical pool walk, independently
 * of the free-list cache. Include the allocator to use its block layout;
 * reuse the existing host's map/unmap stubs without running its death tests. */
#define main heap_existing_tests_main
#define gReport fixture_report
#include "test_heap_host.c"
#undef gReport
#undef main
#include "../userland/libos64/heap.c"

static void check_largest(void)
{
    uint64_t largest = 0, count = 0, bytes = 0;
    for (int i = 0; i < gFakeCount; i++) {
        heap_region_t *r = gFake[i].base;
        if (!(r->flags & HEAP_REGION_POOL)) continue;
        heap_pool_t *pool = (heap_pool_t *)r;
        for (uint64_t at = sizeof(*pool); at < pool->frontier;) {
            heap_block_t *b = (heap_block_t *)((char *)pool + at);
            uint64_t size = b->size_flags & ~(uint64_t)HEAP_FLAG_MASK;
            if (!(b->size_flags & HEAP_IN_USE)) {
                uint64_t payload = size - sizeof(*b);
                if (payload > largest) largest = payload;
                count++; bytes += payload;
            }
            at += size;
        }
    }
    CHECK(!(gReport.generation & 1), "report left unpublished");
    CHECK(gReport.largest_free == largest, "maximum %lu, pool walk %lu",
          (unsigned long)gReport.largest_free, (unsigned long)largest);
    CHECK(gReport.blocks_free == count && gReport.bytes_free == bytes,
          "free totals disagree with the pool walk");
    CHECK(os64_heap_verify() == 0, "runtime verifier rejected a valid heap");
}

static void expect_cache_problem(const char *message)
{
    gDiagnosticLength = 0;
    gDiagnostic[0] = '\0';
    CHECK(os64_heap_verify() != 0, "runtime verifier missed corrupt cache");
    CHECK(strstr(gDiagnostic, message) != NULL, "wrong diagnostic: %s", gDiagnostic);
}

static void check_verifier(void *live)
{
    heap_free_t *saved = gLargestFree;
    uint64_t maximum = gReport.largest_free, bound = gOtherFreeUpper;
    CHECK(saved != NULL && gReport.blocks_free >= 2, "missing free-block fixture");
    gReport.largest_free++;
    expect_cache_problem("cached largest free block");
    gReport.largest_free = maximum;
    gLargestFree = NULL;
    expect_cache_problem("cached largest free block");
    gLargestFree = (heap_free_t *)(uintptr_t)1;
    expect_cache_problem("cached largest free block");
    gLargestFree = (heap_free_t *)((char *)live - sizeof(heap_block_t));
    expect_cache_problem("cached largest free block");
    heap_free_t *smaller = gFreeList;
    while (smaller && block_size(&smaller->hdr) - sizeof(heap_block_t) >= maximum)
        smaller = smaller->next;
    CHECK(smaller != NULL, "missing smaller free block");
    gLargestFree = smaller;
    expect_cache_problem("cached largest free block");
    gLargestFree = saved;
    gOtherFreeUpper = 0;
    expect_cache_problem("other-free bound");
    gOtherFreeUpper = UINT64_MAX;
    CHECK(os64_heap_verify() == 0, "conservative bound was rejected");
    gOtherFreeUpper = bound;
    gReport.blocks_free++;
    expect_cache_problem("free-block count");
    gReport.blocks_free--;
    CHECK(os64_heap_verify() == 0, "restored cache was rejected");
    gDiagnosticLength = 0;
    gDiagnostic[0] = '\0';
}

int main(void)
{
    os64_heap_init();
    CHECK(os64_heap_verify() == 0, "empty heap was rejected");
    gReport.blocks_free = 1;
    expect_cache_problem("free-block count");
    gReport.blocks_free = 0;
    gLargestFree = (heap_free_t *)(uintptr_t)1;
    expect_cache_problem("cached largest free block");
    gLargestFree = NULL;
    /* Guarded equal maxima, a split below the runner-up, then coalescing. */
    void *a = os64_malloc(8192), *g1 = os64_malloc(64);
    void *b = os64_malloc(8192), *g2 = os64_malloc(64);
    os64_free(a); check_largest();
    os64_free(b); check_largest();
    b = os64_malloc(6144); check_largest();
    check_verifier(g2);
    gCheckAlways = true;
    a = os64_malloc(4096); check_largest();
    a = os64_realloc(a, 1024); check_largest();
    a = os64_realloc(a, 6144); check_largest();
    os64_free(b); check_largest();
    os64_free(g1); check_largest();
    os64_free(a); check_largest();
    os64_free(g2); check_largest();

    /* Vary the order and size of splits, coalesces, realloc growth/shrink,
     * dedicated regions and a pool return at teardown; audit each public call. */
    void *slots[128] = {0};
    uint32_t rng = 0x5ca9a12;
    for (unsigned turn = 0; turn < 10000; turn++) {
        rng = rng * 1664525u + 1013904223u;
        unsigned i = (rng >> 8) % 128;
        size_t size = (rng % 97 == 0) ? 150 * 1024 : 16 + ((rng >> 4) % 12000);
        if (!slots[i]) slots[i] = os64_malloc(size);
        else if (rng & 1) { os64_free(slots[i]); slots[i] = NULL; }
        else {
            void *next = os64_realloc(slots[i], size);
            CHECK(next != NULL, "realloc failed");
            if (next) slots[i] = next;
        }
        check_largest();
    }
    for (unsigned i = 0; i < 128; i++) { os64_free(slots[i]); check_largest(); }
    CHECK(os64_heap_verify() == 0, "heap verification failed");
    CHECK(gReport.blocks_live == 0, "allocations remain");
    CHECK(gDiagnosticLength == 0, "HEAPCHECK reported a valid heap: %s", gDiagnostic);
    /* Release the retained primordial pool for the host leak check. */
    while (gFakeCount) os64_unmap(gFake[gFakeCount - 1].base);
    printf("heap maximum: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures != 0;
}
