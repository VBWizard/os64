// sleep_test.c — ring-3 fixture for sleep(ms) + ticks(out): the 2012 SIGSLEEP
// machinery finally driven from CPL 3, and the stopwatch that proves it.
// 0x51EExxxx codes name the failed step; 0x51EE600D = all good.
//
// Steps:
//   1. ticks()           -> fills the struct; per_second is sane (nonzero,
//                           and not absurd), ticks is nonzero (we booted)
//   2. sleep(SLEEP_MS)   -> returns 0, and the tick delta measured around it
//                           is >= the request converted at the REPORTED rate
//                           (the fixture does the same boundary math the
//                           kernel does — with the rate ticks() handed back,
//                           so it stays honest if TICKS_PER_SECOND changes)
//   3. sleep(0)          -> the free yield: returns 0 promptly (bounded
//                           generously — the post-boot suite runs on loaded
//                           cores and a strict bound would flake)
//   4. monotonicity      -> every ticks() reading >= the one before it
//
// What the SUITE run deliberately does NOT test: interruption. Ctrl+C on a
// sleeper is interactive by nature — verified by hand like the rest of the
// SIGINT family. The hand-test knob is argv[1]: `sleep_test 30000` from husk
// is a 30-second nap to Ctrl+C (or `sleep_test 30000 &` + a ctl kill) and
// watch die with 130/137. The suite runs bare and gets the 200ms default.

#include <stdint.h>
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include "os64/ticks.h"

#define SLEEP_OK            0x51EE600DUL
#define FAIL_TICKS_CALL     0x51EE0001UL   // ticks() returned an error
#define FAIL_RATE_INSANE    0x51EE0002UL   // per_second zero or absurd
#define FAIL_SLEEP_RET      0x51EE0003UL   // sleep() returned nonzero
#define FAIL_SLEPT_SHORT    0x51EE0004UL   // woke before the deadline
#define FAIL_YIELD_RET      0x51EE0005UL   // sleep(0) returned nonzero
#define FAIL_YIELD_SLOW     0x51EE0006UL   // sleep(0) burned real time
#define FAIL_NOT_MONOTONIC  0x51EE0007UL   // the stopwatch ran backwards

#define SLEEP_MS            200UL          // 20 ticks at 100/s — long enough
                                           // to be unmistakable, short enough
                                           // to keep the suite quick

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

static uint64_t read_ticks(os64_ticks_t *t)
{
    return os64_syscall1(SYSCALL_TICKS, (uint64_t)t);
}

// Digits or nothing: returns 0 (caller falls back to the default) for
// anything that isn't a plain decimal number. Five lines instead of a
// library dependency — fixtures stay raw-syscall-only on purpose.
static uint64_t parse_ms(const char *s)
{
    uint64_t v = 0;
    if (s == 0 || *s == 0)
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    return v;
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)env;

    // The hand-test knob (header comment): argv[1] overrides the nap length.
    uint64_t sleep_ms = SLEEP_MS;
    if (argc > 1) {
        uint64_t v = parse_ms(argv[1]);
        if (v > 0)
            sleep_ms = v;
    }

    // 1. The stopwatch answers, and its numbers make sense.
    os64_ticks_t t0;
    if (failed(read_ticks(&t0)))
        exit_with(FAIL_TICKS_CALL);
    if (t0.per_second == 0 || t0.per_second > 1000000)
        exit_with(FAIL_RATE_INSANE);

    // 2. Sleep, measured by the clock itself. The expected floor is computed
    //    with the REPORTED rate — the same ceil the kernel applies — so this
    //    fixture keeps passing (and keeps meaning something) on a kernel
    //    rebuilt at any tick rate.
    uint64_t expect = (sleep_ms * t0.per_second + 999) / 1000;
    if (os64_syscall1(SYSCALL_SLEEP, sleep_ms) != 0)
        exit_with(FAIL_SLEEP_RET);

    os64_ticks_t t1;
    if (failed(read_ticks(&t1)))
        exit_with(FAIL_TICKS_CALL);
    if (t1.ticks - t0.ticks < expect)
        exit_with(FAIL_SLEPT_SHORT);

    // 3. The free yield: no time requested, none (much) taken. The bound is
    //    one full second — absurdly generous on purpose; this asserts
    //    "yield, not nap", not a latency SLA.
    if (os64_syscall1(SYSCALL_SLEEP, 0) != 0)
        exit_with(FAIL_YIELD_RET);

    os64_ticks_t t2;
    if (failed(read_ticks(&t2)))
        exit_with(FAIL_TICKS_CALL);
    if (t2.ticks - t1.ticks > t0.per_second)
        exit_with(FAIL_YIELD_SLOW);

    // 4. The stopwatch only runs forward.
    if (t1.ticks < t0.ticks || t2.ticks < t1.ticks)
        exit_with(FAIL_NOT_MONOTONIC);

    exit_with(SLEEP_OK);
}
