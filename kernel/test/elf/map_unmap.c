// map_unmap.c — ring-3 fixture for the heap primitive: map(len)/unmap(base).
// The wall malloc will build on, proven before a single heaprec exists.
// 0x3A9xxxxx codes name the failed step ("MA9" was taken by no one).
//
// Steps:
//   1. map(3 pages)      -> page-aligned base in the heap range
//   2. read it           -> every sampled byte is ZERO (the guarantee), and
//                           the reads themselves demand-page all 3 pages
//   3. write a pattern across all 3 pages, read it back
//   4. map again         -> a DIFFERENT region, no overlap (guard gap ahead
//                           of it); writing it leaves region 1's pattern
//                           untouched (independence)
//   5. unmap(region 1)   -> 0; region 2 still intact afterwards
//   6. unmap(bogus)      -> in-band error for: never-mapped address, the
//                           middle of region 2 (whole regions only!), and
//                           region 1 AGAIN (already gone — not a nop)
//   7. map a third time  -> still works after an unmap; release it; exit

#include <stdint.h>
#include "os64/syscall.h"

#define MAP_OK             0x03A9600DUL
#define FAIL_MAP1          0x3A900001UL
#define FAIL_NOT_ZEROED    0x3A900002UL
#define FAIL_PATTERN       0x3A900003UL
#define FAIL_MAP2          0x3A900004UL
#define FAIL_INDEPENDENCE  0x3A900005UL
#define FAIL_UNMAP1        0x3A900006UL
#define FAIL_SURVIVOR      0x3A900007UL
#define FAIL_BOGUS_UNMAP   0x3A900008UL
#define FAIL_MAP3          0x3A900009UL

#define PAGE 4096UL
#define LEN  (3 * PAGE)

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;

    // 1. A fresh region: aligned, in the heap range, three pages long.
    uint64_t r1 = os64_syscall1(SYSCALL_MAP, LEN);
    if (failed(r1) || (r1 & (PAGE - 1)) != 0 || r1 < 0x70000000UL)
        exit_with(FAIL_MAP1);
    volatile uint8_t *a = (volatile uint8_t *)r1;

    // 2. Sample every page — first touches fault, the demand pager delivers,
    //    and what it delivers must be zero.
    for (uint64_t off = 0; off < LEN; off += 512)
        if (a[off] != 0)
            exit_with(FAIL_NOT_ZEROED);

    // 3. A pattern the region must hold across all three pages.
    for (uint64_t off = 0; off < LEN; off += 64)
        a[off] = (uint8_t)(off >> 6);
    for (uint64_t off = 0; off < LEN; off += 64)
        if (a[off] != (uint8_t)(off >> 6))
            exit_with(FAIL_PATTERN);

    // 4. A second region: distinct, non-overlapping (the guard page sits
    //    between them), and writing it must not disturb region 1.
    uint64_t r2 = os64_syscall1(SYSCALL_MAP, LEN);
    if (failed(r2) || r2 < r1 + LEN + PAGE)
        exit_with(FAIL_MAP2);
    volatile uint8_t *b = (volatile uint8_t *)r2;
    for (uint64_t off = 0; off < LEN; off += 64)
        b[off] = 0xEE;
    for (uint64_t off = 0; off < LEN; off += 64)
        if (a[off] != (uint8_t)(off >> 6))
            exit_with(FAIL_INDEPENDENCE);

    // 5. Release region 1; region 2 must not notice.
    if (failed(os64_syscall1(SYSCALL_UNMAP, r1)))
        exit_with(FAIL_UNMAP1);
    for (uint64_t off = 0; off < LEN; off += 64)
        if (b[off] != 0xEE)
            exit_with(FAIL_SURVIVOR);

    // 6. unmap is strict: exact live bases only. Not a random address, not
    //    the middle of a region, not a region that's already gone.
    if (!failed(os64_syscall1(SYSCALL_UNMAP, 0x60000000UL)) ||
        !failed(os64_syscall1(SYSCALL_UNMAP, r2 + PAGE)) ||
        !failed(os64_syscall1(SYSCALL_UNMAP, r1)))
        exit_with(FAIL_BOGUS_UNMAP);

    // 7. The allocator keeps working after a release.
    uint64_t r3 = os64_syscall1(SYSCALL_MAP, PAGE);
    if (failed(r3))
        exit_with(FAIL_MAP3);
    ((volatile uint8_t *)r3)[0] = 1;   // and it's real memory
    os64_syscall1(SYSCALL_UNMAP, r3);
    os64_syscall1(SYSCALL_UNMAP, r2);

    exit_with(MAP_OK);
}
