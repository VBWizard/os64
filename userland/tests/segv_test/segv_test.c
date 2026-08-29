#include "os64/os64.h"

// segv_test — deliberately dereference a wild pointer from ring 3.
//
// Test fixture for the 2026-07-22 fault-isolation work: a user-mode page
// fault the demand pager can't resolve must SEGFAULT THE TASK (exit code
// 139 = 128 + SIGSEGV, the old Unix convention) and never panic the OS.
// Run it from husk; the pass condition is the kernel printing
// "Segmentation fault: task N ..." and husk getting its prompt back.
//
// 0x40000000000 (4 TiB) is canonical lower-half, far above anything the
// loader or heap maps, so vma_lookup finds nothing and the no-VMA user
// path is the one exercised.
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    os64_printf("segv_test: dereferencing 0x40000000000 — the OS should survive, I should not.\n");

    volatile uint64_t *wild = (volatile uint64_t *)0x40000000000UL;
    uint64_t value = *wild;

    // Reaching here means the fault never fired (or worse, resolved) — loud failure.
    os64_printf("segv_test: FAIL — read 0x%lx and lived\n", value);
    return 1;
}
