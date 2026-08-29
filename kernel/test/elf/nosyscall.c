// nosyscall.c — the tickless-starvation fixture: burn CPU forever, syscall
// never. THE NAME IS THE SPECIFICATION: what makes this fixture useful is not
// that it is greedy but that it is SILENT, and it was called `hog` until
// 2026-08-29, when /bin/hog turned out to be Chris's measuring instrument —
// a hog that reads the clock as it spins, i.e. one that syscalls. Two
// programs, one name, one destination directory — and the userland one is
// what reached the image, so this test had been spawning it. A name that
// states the property cannot be quietly satisfied by something lacking it.
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
// The loop variable is volatile so even a smarter compiler can't reduce this
// to an idle husk — the fixture's one job is to genuinely burn the core.

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
