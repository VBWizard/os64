// dir_list.c — ring-3 fixture for readdir: the syscall your ls will stand on.
// Uses the abi contract directly (<os64/syscall.h> + <os64/dirent.h>), exits
// with a distinct 0x0D12xxxx code per failed step.
//
// Steps:
//   1. open("/bin", "d")          -> directory handle
//   2. readdir loop               -> must find "hello" (file, size > 0) and
//                                    "husk"; every /bin entry is a non-dir
//   3. loop ended with 0          -> and STAYS 0 on the next call (end of
//                                    directory is a state, not an event)
//   4. readdir on a FILE handle   -> error (type safety: a file is not a dir)
//   5. open("/nosuchdir", "d")    -> fails in-band
//   6. close(dir)                 -> 0; readdir afterwards -> error
//   7. open("/", "d")             -> root lists "bin" WITH the DIR flag
//                                    (the flag that tells ls to color it 😉)

#include <stdint.h>
#include "os64/syscall.h"
#include "os64/dirent.h"

#define DIRLIST_OK        0x0D12600DUL   // all checks passed
#define FAIL_OPENDIR      0x0D120001UL   // step 1
#define FAIL_MISSING      0x0D120002UL   // step 2: hello/husk not found
#define FAIL_SUBDIR_FLAG  0x0D120003UL   // step 2: /bin entry claimed to be a dir
#define FAIL_END_STICKY   0x0D120004UL   // step 3
#define FAIL_TYPE_SAFETY  0x0D120005UL   // step 4
#define FAIL_BOGUS_PATH   0x0D120006UL   // step 5
#define FAIL_CLOSE        0x0D120007UL   // step 6
#define FAIL_ROOT_BIN     0x0D120008UL   // step 7

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;

    os64_dirent_t e;

    // 1-2. Walk /bin: the fixtures and apps land there at image build, so
    // hello and husk are guaranteed inventory.
    uint64_t d = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/bin", (uint64_t)"d");
    if (failed(d))
        exit_with(FAIL_OPENDIR);

    int saw_hello = 0, saw_husk = 0;
    long r;
    while ((r = (long)os64_syscall2(SYSCALL_READDIR, d, (uint64_t)&e)) == 1)
    {
        if (e.flags & OS64_DE_DIR)
            exit_with(FAIL_SUBDIR_FLAG);   // /bin holds programs, not dirs
        if (str_eq(e.name, "hello") && e.size > 0)
            saw_hello = 1;
        if (str_eq(e.name, "husk"))
            saw_husk = 1;
    }
    if (r != 0 || !saw_hello || !saw_husk)
        exit_with(FAIL_MISSING);

    // 3. End of directory is sticky — a second look agrees.
    if (os64_syscall2(SYSCALL_READDIR, d, (uint64_t)&e) != 0)
        exit_with(FAIL_END_STICKY);

    // 4. readdir wants a DIRECTORY handle; hand it a file and it must refuse.
    uint64_t f = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/bin/hello", (uint64_t)"r");
    if (failed(f) || !failed(os64_syscall2(SYSCALL_READDIR, f, (uint64_t)&e)))
        exit_with(FAIL_TYPE_SAFETY);
    os64_syscall1(SYSCALL_CLOSE, f);

    // 5. A directory that isn't there fails in-band, like everything else.
    if (!failed(os64_syscall2(SYSCALL_OPEN, (uint64_t)"/nosuchdir", (uint64_t)"d")))
        exit_with(FAIL_BOGUS_PATH);

    // 6. Close releases the handle — and a dead handle stops readdir-ing.
    if (failed(os64_syscall1(SYSCALL_CLOSE, d)) ||
        !failed(os64_syscall2(SYSCALL_READDIR, d, (uint64_t)&e)))
        exit_with(FAIL_CLOSE);

    // 7. The root knows /bin is a directory — the flag ls will color by.
    d = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/", (uint64_t)"d");
    if (failed(d))
        exit_with(FAIL_ROOT_BIN);
    int saw_bin_dir = 0;
    while (os64_syscall2(SYSCALL_READDIR, d, (uint64_t)&e) == 1)
        if (str_eq(e.name, "bin") && (e.flags & OS64_DE_DIR))
            saw_bin_dir = 1;
    os64_syscall1(SYSCALL_CLOSE, d);
    if (!saw_bin_dir)
        exit_with(FAIL_ROOT_BIN);

    exit_with(DIRLIST_OK);
}
