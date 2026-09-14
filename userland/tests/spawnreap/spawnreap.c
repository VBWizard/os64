// A sibling collects short-lived children while spawn is still returning.
#include "os64/os64.h"

#define COUNT 128
#define PASS 0x5A9E0000u
static int64_t collected[COUNT];
static volatile unsigned count, stop, bad;

static int64_t reaper(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE))
    {
        int32_t status = -1;
        int64_t pid = os64_reap(&status);
        if (pid > 0)
        {
            unsigned n = count;
            if (n >= COUNT || status != 0)
                __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
            else
            {
                collected[n] = pid;
                __atomic_store_n(&count, n + 1, __ATOMIC_RELEASE);
            }
        }
        else
            os64_sleep(1);
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1)
        return 0;
    int64_t returned[COUNT];
    int64_t thread = os64_thread(reaper, NULL);
    if (thread < 0)
        return PASS | 1;
    for (unsigned i = 0; i < COUNT; i++)
    {
        returned[i] = os64_spawn("/tests/spawnreap",
                                (char *[]){"/tests/spawnreap", "child", NULL});
        if (returned[i] <= 0)
            __atomic_store_n(&bad, 1, __ATOMIC_RELAXED);
    }
    for (unsigned wait = 0; wait < 3000 && __atomic_load_n(&count, __ATOMIC_ACQUIRE) < COUNT; wait++)
        os64_sleep(10);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    int64_t result;
    os64_thread_join((int32_t)thread, &result);
    os64_close((int32_t)thread);
    if (__atomic_load_n(&bad, __ATOMIC_RELAXED) || count != COUNT)
        return PASS | 2;
    for (unsigned i = 0; i < COUNT; i++)
    {
        unsigned matches = 0;
        for (unsigned j = 0; j < COUNT; j++)
            matches += returned[i] == collected[j];
        if (matches != 1)
            return PASS | 3;
    }
    os64_printf("spawnreap: PASS 128 child PIDs match concurrent sibling reap results\n");
    return PASS;
}
