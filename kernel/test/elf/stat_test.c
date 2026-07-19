// stat_test.c — ring-3 fixture for the stat syscall: "what is this one
// name?" answered in the SAME os64_dirent_t readdir speaks (stat is readdir
// for exactly one name — see dops->stat in vfs.h). 0x57A7xxxx codes name
// the failed step ("57A7" ~ STAT, the hex practically writes itself).
//
// Steps:
//   1. stat("/partition_info") -> a FILE with size > 0 (ships on every root,
//      FAT and ext2 alike, introducing its own filesystem)
//   2. stat("/bin")            -> a DIRECTORY
//   3. stat("/")               -> a DIRECTORY named "/" (both drivers must
//      synthesize the root's entry — it has no parent entry to be named by)
//   4. stat("/no/such/thing")  -> negative (absence answers in-band)
//   5. stat("bin")             -> RELATIVE path resolves against cwd ("/")
//   6. stat("/fat") or stat("/ext2") -> whichever secondary mount this boot
//      produced is a DIRECTORY — stat routing across the mount table
#include <stdint.h>
#include "os64/syscall.h"
#include "os64/dirent.h"

#define STAT_OK          0x57A7600DUL
#define FAIL_FILE        0x57A70001UL
#define FAIL_DIR         0x57A70002UL
#define FAIL_ROOT        0x57A70003UL
#define FAIL_ABSENT      0x57A70004UL
#define FAIL_RELATIVE    0x57A70005UL
#define FAIL_MOUNT       0x57A70006UL

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

static uint64_t stat_path(const char *path, os64_dirent_t *e)
{
    return os64_syscall2(SYSCALL_STAT, (uint64_t)path, (uint64_t)e);
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;
    os64_dirent_t e;

    // 1. A file, with its size.
    if (failed(stat_path("/partition_info", &e)) ||
        (e.flags & OS64_DE_DIR) || e.size == 0)
        exit_with(FAIL_FILE);

    // 2. A directory.
    if (failed(stat_path("/bin", &e)) || !(e.flags & OS64_DE_DIR))
        exit_with(FAIL_DIR);

    // 3. The root itself — the synthesized entry, named "/".
    if (failed(stat_path("/", &e)) || !(e.flags & OS64_DE_DIR) ||
        e.name[0] != '/' || e.name[1] != '\0')
        exit_with(FAIL_ROOT);

    // 4. Absence is an answer, not an accident.
    if (!failed(stat_path("/no/such/thing", &e)))
        exit_with(FAIL_ABSENT);

    // 5. Relative resolution against cwd (every task starts at "/").
    if (failed(stat_path("bin", &e)) || !(e.flags & OS64_DE_DIR))
        exit_with(FAIL_RELATIVE);

    // 6. Across the mount table: one of the secondary mounts must be there
    //    (FAT root grows /ext2; ext2 root grows /fat).
    if ((failed(stat_path("/fat", &e)) || !(e.flags & OS64_DE_DIR)) &&
        (failed(stat_path("/ext2", &e)) || !(e.flags & OS64_DE_DIR)))
        exit_with(FAIL_MOUNT);

    exit_with(STAT_OK);
    return 0;   // unreachable; keeps the non-void signature honest
}
