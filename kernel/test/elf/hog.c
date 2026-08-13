// hog.c — the tickless-starvation fixture: burn CPU forever, syscall never.
//
// This is the EXACT shape that motivated the preemption backstop (the
// 2026-08-09 starvation debt): a ring-3 loop that never enters the kernel.
// Before the backstop, dispatching this on a tickless AP was a life sentence
// for the core — no syscall, no interrupt, no scheduler pass, ever again.
// Anything pinned there (kworker on core 1) starved; Ctrl+C's forced-syscall
// redirect had no pass to run in; kill was a bit nobody would ever read.
//
// test_backstop_preemption (test_main.c) pins one of these to an AP, counts
// that core's lease expiries (kSchedBackstopFires) for half a second, then
// kills it with SIGKILL — a signal only a scheduler pass can deliver to a
// thread that never syscalls. The fixture passing = the whole chain works:
// lease armed at dispatch, expiry preempts, pass delivers the kill.
//
// The loop variable is volatile so even a smarter compiler can't reduce the
// hog to an idle husk — the fixture's one job is to genuinely burn the core.

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc;
    (void)argv;
    (void)env;

    volatile unsigned long spin = 0;
    for (;;)
        spin++;

    return 0;   // unreachable — this program only dies by signal
}
