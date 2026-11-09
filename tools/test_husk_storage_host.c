// Check history eviction and nested scratch lifetimes using husk's real code.
#define main husk_main
#include "../userland/apps/husk/husk.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool refuse_heap;
void *os64_malloc(size_t bytes) { return refuse_heap ? NULL : malloc(bytes); }
void os64_free(void *p) { free(p); }
int64_t os64_write(int32_t h, const void *p, size_t n)
{
    (void)h; (void)p;
    return (int64_t)n;
}

static void history_test(void)
{
    char *reference[HISTORY_BYTES / 2];
    size_t count = 0, used = 0;
    char line[LINE_MAX];
    for (unsigned step = 0; step < 4000; step++)
    {
        size_t len = step < 100 ? 15 : 1 + (step * 97u) % (LINE_MAX - 1);
        memset(line, 'a' + step % 26, len);
        line[len] = 0;
        while (used + len + 1 > HISTORY_BYTES)
        {
            used -= strlen(reference[0]) + 1;
            free(reference[0]);
            memmove(reference, reference + 1, --count * sizeof(reference[0]));
        }
        reference[count] = malloc(len + 1);
        assert(reference[count]);
        memcpy(reference[count++], line, len + 1);
        used += len + 1;
        history_store(line);
        history_store(line); // Consecutive duplicate consumes no storage.
        assert(s_hist_count == (int)count && s_hist_used == used);
        for (size_t i = 0; i < count; i++)
            assert(strcmp(history_get((int)i + 1), reference[count - 1 - i]) == 0);
        assert(!history_get(0) && !history_get((int)count + 1));
        if (step == 99) assert(count == 100);
    }
    for (size_t i = 0; i < count; i++) free(reference[i]);
    s_hist_count = 0;
    s_hist_head = s_hist_next = s_hist_used = 0;
    memset(line, 'x', LINE_MAX - 1); line[LINE_MAX - 1] = 0;
    for (int i = 0; i < 4; i++) { line[0] = 'a' + i; history_store(line); }
    assert(s_hist_count == 4 && s_hist_used == HISTORY_BYTES);
    line[0] = 'e'; history_store(line);
    assert(s_hist_count == 4 && history_get(4)[0] == 'b');
}

static void scratch_test(void)
{
    for (int depth = 0; depth <= SUBST_DEPTH_MAX; depth++)
    {
        s_expdepth = depth;
        assert(expansion_begin(&s_expctx[depth]));
        memset(s_expctx[depth].text, 'a' + depth, EXPANDED_MAX);
        char *word = command_scratch(4000);
        assert(word);
        memset(word, 'z', 4000);
        for (int outer = 0; outer < depth; outer++)
            for (size_t j = 0; j < EXPANDED_MAX; j++)
                assert(s_expctx[outer].text[j] == 'a' + outer);
    }
    s_expdepth = 0;
    size_t reserved = os64_arena_stats(s_expctx[0].arena).reserved_bytes;
    for (int line = 0; line < 1000; line++)
    {
        assert(expansion_begin(&s_expctx[0]));
        assert(command_scratch(4000));
        assert(os64_arena_stats(s_expctx[0].arena).reserved_bytes == reserved);
    }
    assert(!command_scratch(256 * 1024));
    assert(s_expand_complained);
    for (int depth = 0; depth <= SUBST_DEPTH_MAX; depth++)
    {
        os64_arena_destroy(s_expctx[depth].arena);
        memset(&s_expctx[depth], 0, sizeof(s_expctx[depth]));
    }
    refuse_heap = true;
    assert(!expansion_begin(&s_expctx[0]));
    refuse_heap = false;
    assert(expansion_begin(&s_expctx[0]));
    os64_arena_destroy(s_expctx[0].arena);
}

int main(void)
{
    history_test();
    scratch_test();
    puts("husk storage: history wrap/eviction, nested lifetime, reuse and failure PASS");
    return 0;
}
