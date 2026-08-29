// arg_echo.c — task startup-state test fixture (arguments, environment, and
// segment initialization).
//
// The kernel launches this as /tests/arg_echo with argc=3 and
//   argv = { "/tests/arg_echo", "hello", "world" }
// and argv[0] is CHECKED BY CONTENT below, so the path is part of the
// contract: move the fixture and both spawners (test_task_args and testrun's
// table) must move with it, or the fixture reports FAIL_ARGV0 and says so.
// and (always, since env_inherit never returns NULL) an inherited environment.
//
// os64's task ABI hands the entry point argc/argv/env in RDI/RSI/RDX. Because
// _start is a normal C function, the System V calling convention delivers those
// three registers as its first three parameters for free — no asm shim needed.
// A plain `return` from _start lands on task_exit_with_retval (seeded onto the
// ring0 stack by createThread), which captures RAX as the task's retVal. The
// kernel-side test (test_task_args) asserts that retVal == ARG_ECHO_OK.
//
// This fixture is the regression test for three fixes:
//   * task_setup_entry() must run AFTER argc/argv/env are populated (otherwise
//     all three registers latch as 0 and the program sees no arguments).
//   * task_create()'s argc>0 argv construction must copy the strings into the
//     task's own blob and expose TASK_ARGV_VIRT-relative pointers (the old code
//     handed out dangling/garbage pointers and corrupted the caller).
//   * the ELF loader must zero-fill the BSS tail of a page whose file data ends
//     mid-page — verified end-to-end here via g_data/g_bss (see below), which
//     the vma-level unit test cannot cover because it stubs elf_map_segment.
//
// Every check returns a distinct 0xE00000xx code on failure so a regression
// tells us exactly which invariant broke; success returns ARG_ECHO_OK.

#include <stdint.h>

// Must match kernel/include/task.h. TASK_ENV_VIRT moved 0x6f006000 ->
// 0x6f100000 on 2026-08-13 when the fixed-VA block was re-laid to give argv a
// 1MB window (shell globbing raised the argument ceiling to 512). ARGV did NOT
// move — it is the address a program can actually observe, so its neighbours
// were the ones that gave ground. This fixture is precisely what catches a
// mismatch between these two files, which is why it now runs on every boot
// (test_task_args) and not only from the ring-3 suite.
#define TASK_ARGV_VIRT 0x6f000000UL
#define TASK_ENV_VIRT  0x6f100000UL

// Success sentinel and per-check failure codes.
#define ARG_ECHO_OK        0x00A11600DUL   // "ALL GOOD"
#define FAIL_ARGC          0xE0000001UL
#define FAIL_ARGV_PTR      0xE0000002UL
#define FAIL_ARGV_NULLS    0xE0000003UL
#define FAIL_ARGV_TERM     0xE0000004UL
#define FAIL_ARGV0         0xE0000005UL
#define FAIL_ARGV1         0xE0000006UL
#define FAIL_ARGV2         0xE0000007UL
#define FAIL_ENV_PTR       0xE0000008UL
#define FAIL_ENV_EMPTY     0xE0000009UL
#define FAIL_DATA_INIT     0xE000000AUL
#define FAIL_BSS_ZERO      0xE000000BUL

// These two globals force a second (RW) PT_LOAD whose file data (.data) ends
// partway into its page, immediately followed by BSS (.bss) in that SAME page.
// That is exactly the layout the ELF loader's partial-page zero-fill has to get
// right: g_data must survive the file-backed read, and the BSS bytes sharing the
// page must read back as zero rather than as whatever followed .data in the file.
// (An all-zero global would land in .bss with p_filesz==0 and take the plain
// anonymous path instead, missing the partial-page case — hence a *non-zero*
// initializer here.)
volatile unsigned int  g_data = 0x5A5A5A5AU;   // initialized  -> .data
volatile unsigned char g_bss[256];             // uninitialized -> .bss

// Freestanding string compare — no libc is linked.
static int streq(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    // --- argc / argv pointer arrived via RDI/RSI (proves task_setup_entry ran
    //     after the arguments were built, not before) ---
    if (argc != 3)
        return FAIL_ARGC;
    if ((uintptr_t)argv != TASK_ARGV_VIRT)
        return FAIL_ARGV_PTR;

    // --- argv array is well-formed: three non-NULL entries, NULL-terminated ---
    if (argv[0] == 0 || argv[1] == 0 || argv[2] == 0)
        return FAIL_ARGV_NULLS;
    if (argv[3] != 0)
        return FAIL_ARGV_TERM;

    // --- string contents survived the copy and the TASK-space pointers are
    //     dereferenceable inside our own address space ---
    if (!streq(argv[0], "/tests/arg_echo"))
        return FAIL_ARGV0;
    if (!streq(argv[1], "hello"))
        return FAIL_ARGV1;
    if (!streq(argv[2], "world"))
        return FAIL_ARGV2;

    // --- env arrived via RDX and is mapped where we expect; the kernel test
    //     seeds one variable, so count must be non-zero (layout per env.h:
    //     [ uint32_t page_count | uint32_t count | uint32_t data_end | data ]) ---
    if ((uintptr_t)env != TASK_ENV_VIRT)
        return FAIL_ENV_PTR;
    if (((const uint32_t *)env)[1] == 0)   // count field
        return FAIL_ENV_EMPTY;

    // --- segment initialization: the initialized .data global must survive the
    //     file-backed load, and the .bss sharing its partial page must be zero ---
    if (g_data != 0x5A5A5A5AU)
        return FAIL_DATA_INIT;
    for (unsigned i = 0; i < sizeof(g_bss); i++) {
        if (g_bss[i] != 0)
            return FAIL_BSS_ZERO;
    }

    return ARG_ECHO_OK;
}
