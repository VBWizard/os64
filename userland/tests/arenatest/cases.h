#ifndef ARENATEST_CASES_H
#define ARENATEST_CASES_H

#include "os64/arena.h"
#include "os64/str.h"
#include <stdint.h>

// The host and guest supply ARENA_CHECK so both exercise the public API.
static void arena_test_cases(void)
{
    os64_arena_reset(NULL);
    os64_arena_destroy(NULL);
    ARENA_CHECK(os64_arena_stats(NULL).reserved_bytes == 0);
    ARENA_CHECK(os64_arena_alloc(NULL, 1) == NULL);
    ARENA_CHECK(os64_arena_strdup(NULL, "x") == NULL);
    ARENA_CHECK(os64_arena_create(0, 1) == NULL);

    os64_arena_t *defaults = os64_arena_create(0, 0);
    ARENA_CHECK(defaults != NULL);
    size_t handle_bytes = os64_arena_stats(defaults).reserved_bytes;
    ARENA_CHECK(os64_arena_alloc(defaults, 4096) != NULL);
    ARENA_CHECK(os64_arena_stats(defaults).chunk_count == 1);
    os64_arena_destroy(defaults);
    defaults = os64_arena_create(0, handle_bytes);
    ARENA_CHECK(defaults != NULL);
    ARENA_CHECK(os64_arena_alloc(defaults, 1) == NULL);
    ARENA_CHECK(os64_arena_stats(defaults).reserved_bytes == handle_bytes);
    ARENA_CHECK(os64_arena_stats(defaults).chunk_count == 0);
    os64_arena_destroy(defaults);

    os64_arena_t *arena = os64_arena_create(64, 0);
    ARENA_CHECK(arena != NULL);
    ARENA_CHECK(os64_arena_stats(arena).chunk_count == 0);
    os64_arena_reset(arena);
    unsigned char *first = os64_arena_alloc(arena, 31);
    ARENA_CHECK(first && (uintptr_t)first % 16 == 0);
    os64_memset(first, 0x5a, 31);
    for (size_t alignment = 1; alignment <= 65536; alignment *= 2) {
        unsigned char *p = os64_arena_alloc_aligned(arena, 37, alignment);
        ARENA_CHECK(p && (uintptr_t)p % alignment == 0);
        os64_memset(p, 0xc3, 37);
    }
    for (size_t i = 0; i < 31; i++)
        ARENA_CHECK(first[i] == 0x5a);
    ARENA_CHECK(os64_arena_stats(arena).chunk_count > 1);
    char *copy = os64_arena_strdup(arena, "frame decoration");
    ARENA_CHECK(copy && os64_streq(copy, "frame decoration"));
    char *empty = os64_arena_strdup(arena, "");
    ARENA_CHECK(empty && *empty == '\0');

    os64_arena_stats_t before = os64_arena_stats(arena);
    ARENA_CHECK(os64_arena_alloc(arena, 0) == NULL);
    ARENA_CHECK(os64_arena_alloc(arena, SIZE_MAX) == NULL);
    ARENA_CHECK(os64_arena_alloc_aligned(arena, 1, 0) == NULL);
    ARENA_CHECK(os64_arena_alloc_aligned(arena, 1, 3) == NULL);
    ARENA_CHECK(os64_arena_alloc_aligned(arena, SIZE_MAX, 16) == NULL);
    ARENA_CHECK(os64_arena_alloc_aligned(arena, SIZE_MAX / 2 + 2,
                                        SIZE_MAX / 2 + 1) == NULL);
    ARENA_CHECK(os64_arena_calloc(arena, SIZE_MAX, 2) == NULL);
    ARENA_CHECK(os64_arena_calloc(arena, 0, 1) == NULL);
    ARENA_CHECK(os64_arena_calloc(arena, 1, 0) == NULL);
    ARENA_CHECK(os64_arena_strdup(arena, NULL) == NULL);
    os64_arena_stats_t after = os64_arena_stats(arena);
    ARENA_CHECK(before.used_bytes == after.used_bytes);
    ARENA_CHECK(before.reserved_bytes == after.reserved_bytes);
    ARENA_CHECK(before.chunk_count == after.chunk_count);
    ARENA_CHECK(first[0] == 0x5a && first[30] == 0x5a);
    os64_arena_reset(arena);
    after = os64_arena_stats(arena);
    ARENA_CHECK(after.used_bytes == 0 && after.reserved_bytes == before.reserved_bytes);
    ARENA_CHECK(after.chunk_count == before.chunk_count);
    unsigned char *zero = os64_arena_calloc(arena, 31, 1);
    ARENA_CHECK(zero != NULL);
    for (size_t i = 0; i < 31; i++)
        ARENA_CHECK(zero[i] == 0);
    os64_arena_destroy(arena);

    // A small budget forces geometric growth to clip, then fail atomically.
    arena = os64_arena_create(4096, 1024);
    ARENA_CHECK(arena != NULL);
    first = os64_arena_alloc(arena, 512);
    ARENA_CHECK(first != NULL);
    os64_memset(first, 0x6b, 512);
    before = os64_arena_stats(arena);
    ARENA_CHECK(before.reserved_bytes <= 1024);
    ARENA_CHECK(os64_arena_alloc(arena, 1024) == NULL);
    ARENA_CHECK(os64_arena_stats(arena).used_bytes == before.used_bytes);
    ARENA_CHECK(os64_arena_stats(arena).reserved_bytes == before.reserved_bytes);
    for (size_t i = 0; i < 512; i++)
        ARENA_CHECK(first[i] == 0x6b);
    ARENA_CHECK(os64_arena_alloc(arena, 1) != NULL);
    os64_arena_reset(arena);
    ARENA_CHECK(os64_arena_alloc(arena, 512) != NULL);
    ARENA_CHECK(os64_arena_stats(arena).reserved_bytes == before.reserved_bytes);
    os64_arena_destroy(arena);

    // Repeated frames must retain valid, nonoverlapping payloads and settle
    // at the first frame's reservation, including an oversized allocation.
    arena = os64_arena_create(64, 2 * 1024 * 1024);
    ARENA_CHECK(arena != NULL);
    size_t reserved = 0;
    for (size_t frame = 0; frame < 50; frame++) {
        unsigned char *blocks[128];
        size_t lengths[128];
        for (size_t i = 0; i < 128; i++) {
            lengths[i] = i == 64 ? 300 * 1024 : 1 + (i * 137) % 2048;
            blocks[i] = os64_arena_alloc(arena, lengths[i]);
            ARENA_CHECK(blocks[i] && (uintptr_t)blocks[i] % 16 == 0);
            os64_memset(blocks[i], (int)(i + frame), lengths[i]);
        }
        for (size_t i = 0; i < 128; i++)
            for (size_t j = 0; j < lengths[i]; j++)
                ARENA_CHECK(blocks[i][j] == (unsigned char)(i + frame));
        after = os64_arena_stats(arena);
        if (!frame)
            reserved = after.reserved_bytes;
        ARENA_CHECK(after.reserved_bytes == reserved);
        ARENA_CHECK(after.used_bytes <= after.reserved_bytes);
        os64_arena_reset(arena);
        ARENA_CHECK(os64_arena_stats(arena).used_bytes == 0);
    }
    os64_arena_destroy(arena);
}

#endif
