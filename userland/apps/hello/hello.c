// hello.c — the first os64 userland application.
//
// It is deliberately tiny, because the POINT of hello is not hello — it is
// to exercise the entire userland pipeline end to end, ONCE, so the process
// is proven and repeatable for every app after it:
//
//   abi/ (the syscall contract) → launch (crt0-equiv, calls main, exits) →
//   libos64 (the friendly veneer) → this app, built to an ELF, dropped on
//   the disk image, task_create'd at CPL 3 by the kernel, run, and exited
//   cleanly with a code the kernel can check.
//
// If this prints and exits 0, the road to the shell is paved.

#include "os64/os64.h"

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    os64_puts("Hello from os64 userland! (app: hello, via libos64)\n");

    // Prove the startup handoff: the kernel passed argc/argv in RDI/RSI and
    // launch let them through. argv[0] is the path we were launched as.
    if (argc > 0 && argv && argv[0]) {
        os64_puts("hello: launched as ");
        os64_puts(argv[0]);
        os64_puts("\n");
    }

    // Prove the process/scheduler round trip too: yield, then come back.
    os64_yield();
    os64_puts("hello: back from yield — exiting 0\n");
    os64_debug_log("goodbye from hello :-)");

    return 0;   // launch converts this into exit(0); the kernel checks it
}
