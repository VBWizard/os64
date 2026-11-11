// A program that ends with other threads still alive reports the code it
// ended with, and one that is really killed still reports the kill.
//
// Teardown kills a dying task's remaining threads, and each of them dies
// through the same door a real SIGKILL uses. This checks that their deaths
// do not rewrite what the parent collects: a child whose main returns 7, and
// one that calls exit(9), each with one sibling parked in a sleep and one
// spinning with no system calls. A third child is killed from outside and
// must report 137.
#include "os64/os64.h"

#define PASS 0xE817C000u
#define ROUNDS 16

static volatile uint32_t s_started;
static volatile uint32_t s_stop;   // never set: the siblings end only by being killed

static int64_t sleeper(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&s_started, 1, __ATOMIC_RELEASE);
    while (!s_stop)
        os64_sleep(1000);
    return 0;
}

static int64_t spinner(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&s_started, 1, __ATOMIC_RELEASE);
    while (!s_stop)
        __asm__ volatile("pause");
    return 0;
}

static int child(const char *how)
{
    if (os64_thread(sleeper, NULL) < 0 || os64_thread(spinner, NULL) < 0)
        return 1;
    while (__atomic_load_n(&s_started, __ATOMIC_ACQUIRE) < 2)
        os64_yield();
    os64_sleep(20);   // long enough for the sleeper to be parked
    if (os64_streq(how, "exit"))
        os64_exit(9);
    if (os64_streq(how, "wait"))
        while (!s_stop)
            os64_sleep(1000);
    return 7;
}

static int32_t run(const char *how, bool kill)
{
    int64_t pid = os64_spawn("/tests/exitcode", (char *[]){"/tests/exitcode", (char *)how, NULL});
    if (pid <= 0)
        return -1;
    if (kill) {
        char path[48];
        os64_snprintf(path, sizeof(path), "/proc/%ld/ctl", (long)pid);
        os64_sleep(100);   // let it start its threads
        int64_t ctl = os64_open(path, "w");
        if (ctl < 0 || os64_write((int32_t)ctl, "kill", 4) != 4)
            return -2;
        os64_close((int32_t)ctl);
    }
    int32_t status = -1;
    if (os64_wait(pid, &status) != pid)
        return -3;
    return status;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return child(argv[1]);
    for (unsigned i = 0; i < ROUNDS; i++) {
        int32_t returned = run("return", false), exited = run("exit", false);
        if (returned != 7 || exited != 9) {
            os64_printf("exitcode: FAIL round %u: main returned 7 and the parent saw %d; exit(9) and it saw %d\n", i,
                        returned, exited);
            return PASS | 1;
        }
    }
    int32_t killed = run("wait", true);
    if (killed != 137) {
        os64_printf("exitcode: FAIL: a killed program reported %d, not 137\n", killed);
        return PASS | 2;
    }
    os64_printf("exitcode: PASS %u returns of 7 and exits of 9 with live threads, and a kill reported 137\n", ROUNDS);
    return PASS;
}
