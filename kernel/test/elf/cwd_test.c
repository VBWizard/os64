// cwd_test.c — ring-3 fixture for kernel-owned cwd: getcwd/chdir, canonical
// ".." collapse, relative-path resolution in open AND spawn, inheritance.
// 0x0C3Dxxxx codes name the failed step ("C3D" ~ CWD, squint harder).
//
// The clever bit: the fixture SPAWNS ITSELF. Parent mode chdirs to /bin and
// launches "cwd_test" — by RELATIVE path, which itself proves spawn resolves
// against cwd — passing the expected directory as argv. Child mode getcwd's
// and compares: inheritance proven by a child born somewhere specific.
//
// Steps (parent mode):
//   1. getcwd            -> "/" (every task starts at the root)
//   2. chdir("/bin")     -> 0; getcwd -> "/bin"
//   3. open("hello","r") -> RELATIVE open resolves to /bin/hello: ELF magic
//   4. chdir("..")       -> getcwd "/" (canonicalizer collapsed it — the
//                           stored cwd is "/", not "/bin/..")
//   5. chdir("/nosuch")  -> fails; getcwd STILL "/" (failed chdir = no move)
//   6. chdir("/dir1//../bin/.") -> getcwd "/bin" (the full gauntlet... note
//                           /dir1 is on the ext2 partition, NOT the root fs —
//                           so this path only works via canonicalization
//                           happening BEFORE existence checking)
//   7. spawn "cwd_test" child with expected "/bin"; child exit 0 = inherited
#include <stdint.h>
#include "os64/syscall.h"

#define CWD_OK            0x0C3D600DUL
#define FAIL_START_ROOT   0x0C3D0001UL
#define FAIL_CHDIR_BIN    0x0C3D0002UL
#define FAIL_REL_OPEN     0x0C3D0003UL
#define FAIL_DOTDOT       0x0C3D0004UL
#define FAIL_BAD_CHDIR    0x0C3D0005UL
#define FAIL_GAUNTLET     0x0C3D0006UL
#define FAIL_INHERIT      0x0C3D0007UL

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

static int cwd_is(const char *want)
{
    char buf[128];
    if (failed(os64_syscall2(SYSCALL_GETCWD, (uint64_t)buf, sizeof(buf))))
        return 0;
    return str_eq(buf, want);
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)env;

    // CHILD MODE: argv[1] == "child", argv[2] = the cwd we must have been
    // born with. Exit 0 on match — the parent reads it via wait.
    if (argc >= 3 && str_eq(argv[1], "child"))
        exit_with(cwd_is(argv[2]) ? 0 : 1);

    // 1. Life begins at the root.
    if (!cwd_is("/"))
        exit_with(FAIL_START_ROOT);

    // 2. Move, and see the move.
    if (failed(os64_syscall1(SYSCALL_CHDIR, (uint64_t)"/bin")) || !cwd_is("/bin"))
        exit_with(FAIL_CHDIR_BIN);

    // 3. A RELATIVE open now means /bin/hello — the resolve-everywhere
    //    plumbing at work. ELF magic proves we opened the real file.
    char buf[4];
    uint64_t h = os64_syscall2(SYSCALL_OPEN, (uint64_t)"hello", (uint64_t)"r");
    // (syscall4 with trailing 0: read's arg3 = deadline, 0 = block forever)
    if (failed(h) || os64_syscall4(SYSCALL_READ, h, (uint64_t)buf, 4, 0) != 4 ||
        buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F')
        exit_with(FAIL_REL_OPEN);
    os64_syscall1(SYSCALL_CLOSE, h);

    // 4. ".." collapses to a clean parent, not an ever-growing suffix.
    if (failed(os64_syscall1(SYSCALL_CHDIR, (uint64_t)"..")) || !cwd_is("/"))
        exit_with(FAIL_DOTDOT);

    // 5. A failed chdir moves nothing.
    if (!failed(os64_syscall1(SYSCALL_CHDIR, (uint64_t)"/nosuchdir")) || !cwd_is("/"))
        exit_with(FAIL_BAD_CHDIR);

    // 6. The gauntlet: dots, doubled slashes, a detour through a directory
    //    that exists on a DIFFERENT filesystem — canonicalization is pure
    //    string logic and never touches the disk, so only the final "/bin"
    //    has to actually exist.
    if (failed(os64_syscall1(SYSCALL_CHDIR, (uint64_t)"/dir1//../bin/.")) || !cwd_is("/bin"))
        exit_with(FAIL_GAUNTLET);

    // 7. Spawn BY RELATIVE PATH from /bin (spawn resolves too), child
    //    verifies it woke up in /bin. Inheritance, witnessed.
    char *cargv[] = { "cwd_test", "child", "/bin", (char *)0 };
    uint64_t pid = os64_syscall6(SYSCALL_SPAWN, (uint64_t)"cwd_test",
                                 (uint64_t)cargv, (uint64_t)-1, (uint64_t)-1,
                                 (uint64_t)-1, 0);
    if (failed(pid))
        exit_with(FAIL_INHERIT);
    int code = -1;
    if (failed(os64_syscall2(SYSCALL_WAIT, pid, (uint64_t)&code)) || code != 0)
        exit_with(FAIL_INHERIT);

    exit_with(CWD_OK);
}
