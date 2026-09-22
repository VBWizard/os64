#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "os64/mem.h"

#define ARENA_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "arena host: FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#include "../userland/tests/arenatest/cases.h"

static size_t live_allocations;
static size_t heap_calls;
static size_t failures_left;
static size_t refuse_above = SIZE_MAX;
static size_t previous_request, last_request;

void *os64_malloc(size_t size)
{
    heap_calls++;
    previous_request = last_request;
    last_request = size;
    if (failures_left) {
        failures_left--;
        return NULL;
    }
    if (size > refuse_above)
        return NULL;
    void *out = malloc(size);
    if (out) {
        ARENA_CHECK((uintptr_t)out % 16 == 0);
        live_allocations++;
        // Do not let calloc tests accidentally pass on fresh zero pages.
        os64_memset(out, 0xa5, size);
    }
    return out;
}

void os64_free(void *pointer)
{
    if (pointer) {
        ARENA_CHECK(live_allocations > 0);
        live_allocations--;
        free(pointer);
    }
}

static void test_heap_failure(void)
{
    failures_left = 1;
    ARENA_CHECK(os64_arena_create(0, 0) == NULL);
    ARENA_CHECK(live_allocations == 0);
    os64_arena_t *arena = os64_arena_create(64, 0);
    ARENA_CHECK(arena != NULL);
    unsigned char *blocks[8];
    for (size_t round = 0; round < 8; round++) {
        os64_arena_stats_t before = os64_arena_stats(arena);
        size_t live_before = live_allocations;
        size_t request = 1024u << round;
        // These requests exceed the growth target, so there is no smaller
        // capacity to retry. The preferred-size case is tested separately.
        failures_left = 1;
        ARENA_CHECK(os64_arena_alloc(arena, request) == NULL);
        ARENA_CHECK(!failures_left);
        os64_arena_stats_t after = os64_arena_stats(arena);
        ARENA_CHECK(before.used_bytes == after.used_bytes);
        ARENA_CHECK(before.reserved_bytes == after.reserved_bytes);
        ARENA_CHECK(before.chunk_count == after.chunk_count);
        ARENA_CHECK(live_allocations == live_before);
        unsigned char *p = os64_arena_alloc(arena, request);
        ARENA_CHECK(p != NULL);
        os64_memset(p, 0x7d, request);
        blocks[round] = p;
        for (size_t i = 0; i <= round; i++)
            for (size_t j = 0; j < (1024u << i); j++)
                ARENA_CHECK(blocks[i][j] == 0x7d);
    }
    size_t calls = heap_calls;
    os64_arena_reset(arena);
    for (size_t round = 0; round < 8; round++)
        ARENA_CHECK(os64_arena_alloc(arena, 1024u << round) != NULL);
    ARENA_CHECK(heap_calls == calls);
    os64_arena_reset(arena);
    for (size_t round = 8; round > 0; round--)
        ARENA_CHECK(os64_arena_alloc(arena, 1024u << (round - 1)) != NULL);
    ARENA_CHECK(heap_calls == calls);
    os64_arena_destroy(arena);
    ARENA_CHECK(live_allocations == 0);
}

static void test_preferred_fallback(void)
{
    os64_arena_t *arena = os64_arena_create(8192, 0);
    ARENA_CHECK(arena != NULL);
    unsigned char *first = os64_arena_alloc(arena, 8192);
    ARENA_CHECK(first != NULL);
    os64_memset(first, 0x5a, 8192);
    os64_arena_stats_t before = os64_arena_stats(arena);
    size_t calls = heap_calls;
    failures_left = 2;
    ARENA_CHECK(os64_arena_alloc_aligned(arena, 37, 64) == NULL);
    ARENA_CHECK(!failures_left && heap_calls == calls + 2);
    os64_arena_stats_t after = os64_arena_stats(arena);
    ARENA_CHECK(after.used_bytes == before.used_bytes);
    ARENA_CHECK(after.reserved_bytes == before.reserved_bytes);
    ARENA_CHECK(after.chunk_count == before.chunk_count);
    ARENA_CHECK(live_allocations == 2);

    failures_left = 1;
    unsigned char *p = os64_arena_alloc_aligned(arena, 37, 64);
    ARENA_CHECK(p && (uintptr_t)p % 64 == 0);
    ARENA_CHECK(!failures_left && heap_calls == calls + 4);
    ARENA_CHECK(last_request < previous_request);
    ARENA_CHECK(os64_arena_stats(arena).reserved_bytes == before.reserved_bytes + last_request);
    size_t preferred = previous_request;
    os64_memset(p, 0xc3, 37);
    ARENA_CHECK(os64_arena_alloc(arena, 8192) != NULL);
    ARENA_CHECK(last_request == preferred); // Retry leaves the growth target intact.
    for (size_t i = 0; i < 8192; i++)
        ARENA_CHECK(first[i] == 0x5a);
    for (size_t i = 0; i < 37; i++)
        ARENA_CHECK(p[i] == 0xc3);
    os64_arena_destroy(arena);

    // Small heap blocks remain available even when large requests fail.
    refuse_above = 64 * 1024;
    arena = os64_arena_create(1024 * 1024, 128 * 1024);
    ARENA_CHECK(arena != NULL);
    for (size_t i = 0; i < 1000; i++) {
        ARENA_CHECK(os64_arena_alloc(arena, 16) != NULL);
        ARENA_CHECK(os64_arena_stats(arena).reserved_bytes <= 128 * 1024);
    }
    os64_arena_destroy(arena);
    arena = os64_arena_create(0, 0);
    ARENA_CHECK(arena != NULL);
    for (size_t i = 0; i < 128; i++)
        ARENA_CHECK(os64_arena_alloc(arena, 1024) != NULL);
    os64_arena_destroy(arena);
    refuse_above = SIZE_MAX;
    ARENA_CHECK(live_allocations == 0);
}

int main(void)
{
    arena_test_cases();
    ARENA_CHECK(live_allocations == 0);
    test_heap_failure();
    test_preferred_fallback();
    puts("arena host: PASS (shared guest cases, failed create/growth, preferred retry, bounded heap, reuse, no leaks)");
    return 0;
}
