// exit_by_return.c — ring-3 exit-trampoline fixture.
//
// The whole program is one statement: return a magic value from _start.
// That exercises the path syscall_smoke.c deliberately avoids: a compiled C
// `ret` at CPL 3 pops the return address task_setup_ring3_exit_path() seeded
// on the user stack, lands in the trampoline page at TASK_EXIT_TRAMPOLINE_VIRT
// (still ring 3), which moves RAX into RDI and invokes SYSCALL_EXIT.  The
// kernel-side test asserts task->retVal == EXIT_BY_RETURN_MAGIC — the value
// can only have traveled RAX -> trampoline -> exit syscall -> task->retVal.
//
// If this fixture faults instead of exiting, the seeded return address or the
// trampoline mapping is broken (see thread.c ring3 branch + task.c
// task_setup_ring3_exit_path).

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc;
    (void)argv;
    (void)env;

    return 0x2E7BEA57UL;   // "RET BEAST" — matched by test_ring3_exit_by_return
}
