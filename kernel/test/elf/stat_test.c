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
//   6. stat routing across the mount table, in two halves: "/sys" (a
//      synthetic claims its prefix on every boot ever, so the ROUTING is
//      provable unconditionally — and so is OS64_DE_MOUNT, against /bin
//      which must NOT carry it), then the prefix of every disk-backed
//      non-root mount /sys/mounts actually lists (a DISK driver's stat
//      receiving the fs-local tail). WHICH disk mounts exist stopped being
//      a constant when /etc/mounts.conf made boot mounts policy
//      (2026-08-31) — this step used to hardcode "/fat or /ext2" from the
//      sweep era and failed on the first boot whose policy said otherwise.
//      A boot that mounts no secondary disk answers SKIP, not failure:
//      that is a fact about the BOOT, not about stat (the synctest rule).
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
#define SKIP_NO_SECONDARY 0x57A70007UL   // no disk mount beyond root — testrun's skipcode

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

// /sys/mounts, slurped raw (this fixture is deliberately libos64-free: it
// proves the syscall floor with nothing but the syscalls). Sized for the
// PRODUCER's worst case, not for the boots we happen to run: a full mount
// table, every row carrying a mount prefix and a 36-character GPT name that
// escaping can quadruple (a name is arbitrary bytes off somebody's disk).
// The old 4096 fitted a typical boot and failed a legal one — and this
// fixture reports a short read as FAIL_MOUNT, so the suite would have called
// a supported mounts.conf a broken stat.
static char gMounts[16384];

static long read_mounts(void)
{
    uint64_t h = os64_syscall2(SYSCALL_OPEN, (uint64_t)"/sys/mounts", (uint64_t)"r");
    if (failed(h))
        return -1;
    long total = 0;
    for (;;)
    {
        uint64_t n = os64_syscall4(SYSCALL_READ, h,
                                   (uint64_t)(gMounts + total),
                                   (uint64_t)(sizeof(gMounts) - 1 - total),
                                   OS64_WAIT_FOREVER);
        if (failed(n))
            { total = -1; break; }
        if (n == 0)
            break;
        total += (long)n;
        if (total >= (long)sizeof(gMounts) - 1)
            { total = -1; break; }   // grew past the buffer — refuse, loudly
    }
    os64_syscall1(SYSCALL_CLOSE, h);
    if (total >= 0)
        gMounts[total] = '\0';
    return total;
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

    // 6a. Routing, unconditionally: a synthetic's prefix routes to its own
    //     filesystem's stat on every boot there has ever been — and stat
    //     says it is a MOUNT while /bin, an ordinary directory on the root
    //     filesystem, says it is not. Both polarities, because stat is the
    //     door where that bit is least obvious: the path resolves INTO the
    //     mounted filesystem and asks its root, which knows nothing about
    //     being mounted, so the answer is assembled by the resolver rather
    //     than reported by the driver (dirent.h's OS64_DE_MOUNT). A walker
    //     that stats before it descends has to get the same answer readdir
    //     would have given it.
    if (failed(stat_path("/sys", &e)) ||
        (e.flags & (OS64_DE_DIR | OS64_DE_MOUNT)) != (OS64_DE_DIR | OS64_DE_MOUNT))
        exit_with(FAIL_MOUNT);
    if (failed(stat_path("/bin", &e)) || (e.flags & OS64_DE_MOUNT))
        exit_with(FAIL_MOUNT);

    // 6b. The disk half: stat the prefix of every disk-backed non-root
    //     mount the table actually lists (device column != "-"). A mount
    //     the table names but stat cannot see IS a failure; a table with
    //     no secondary disk mounts is the boot's policy, and answers SKIP.
    if (read_mounts() < 0)
        exit_with(FAIL_MOUNT);
    int secondaries = 0;
    for (char *p = gMounts; *p != '\0'; )
    {
        char *line = p;
        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            *p++ = '\0';
        if (line[0] != '/')
            continue;   // the '#' header, or noise

        // Columns: prefix fstype device ... — split the first three.
        char *prefix = line;
        char *q = line;
        while (*q != '\0' && *q != ' ' && *q != '\t') q++;
        if (*q == '\0') continue;
        *q++ = '\0';
        while (*q == ' ' || *q == '\t') q++;             // fstype
        while (*q != '\0' && *q != ' ' && *q != '\t') q++;
        while (*q == ' ' || *q == '\t') q++;             // device
        char *device = q;
        while (*q != '\0' && *q != ' ' && *q != '\t') q++;
        *q = '\0';

        if (device[0] == '-' || device[0] == '\0' || prefix[1] == '\0')
            continue;   // synthetic, a short row, or the root itself

        secondaries++;
        // A DISK mount, so the MOUNT bit has to hold for a real filesystem
        // and not only for the synthetics 6a proved it on.
        if (failed(stat_path(prefix, &e)) ||
            (e.flags & (OS64_DE_DIR | OS64_DE_MOUNT)) != (OS64_DE_DIR | OS64_DE_MOUNT))
            exit_with(FAIL_MOUNT);
    }
    if (secondaries == 0)
        exit_with(SKIP_NO_SECONDARY);

    exit_with(STAT_OK);
    return 0;   // unreachable; keeps the non-void signature honest
}
