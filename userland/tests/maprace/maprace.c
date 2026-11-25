// maprace — threads of one task mapping at once each get a range of their
// own (task_va_reserve, the task's one door for its heap range). Run by
// testrun.
//
//   1. four threads each map 2000 single pages, writing their own tag into
//      every page they were given;
//   2. no base was handed out twice, across all of them, and every page
//      still holds the tag its thread wrote (two threads given one range
//      would overwrite each other's).
//
// Exit codes: 0x3A9E0000 success, 0x3A9E00NN the step that failed.

#include "os64/os64.h"
#include "os64/mem.h"
#include "os64/thread.h"

#define THREADS 4
#define MAPS 2000
#define FAIL(step) return (int)(0x3A9E0000u | (step))

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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
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
    return (int)0x3A9E0000u;
}
