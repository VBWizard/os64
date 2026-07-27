// memory_test.c — ring-3 fixture for memory(out): the physical memory picture
// and, more importantly, the AUDIT — free + used must equal usable exactly,
// because both sides of the identity are seeded from the same memory map and
// counted in one atomic ledger walk. A kernel whose books don't balance has a
// merge/compaction/split bug, and this fixture exists so that bug cannot hide.
// 0xF3EExxxx codes name the failed step; 0xF3EE600D = "FREE GOOD".
//
// Steps:
//   1. memory(NULL)      -> refused (negative), like every out-struct syscall
//   2. memory(&m)        -> returns 0; fields are sane:
//                           total >= usable > 0, free > 0, used > 0 (the
//                           kernel itself is resident!), page_size a nonzero
//                           power of two, largest_free_extent in (0, free]
//   3. THE BOOKS BALANCE -> free + used == usable, EXACTLY (no tolerance:
//                           the identity is exact by construction, so any
//                           slack here would only hide the bug it hunts)
//   4. the contract      -> available == free + reclaimable (summed by the
//                           kernel; this fixture just verifies the promise)
//   5. the needle moves  -> map a region, TOUCH every page (demand paging:
//                           untouched pages cost nothing, so touching is the
//                           allocation), re-query: free fell, used rose, and
//                           the books STILL balance mid-flight
//   6. and moves back    -> unmap; free recovers past its touched low-water
//                           mark, books balance a third time
//
// Step 5/6 direction checks are safe on the quiet post-boot suite: for the
// deltas to be invisible, something else would have to free >=256KB in the
// microseconds between two syscalls. The balance check needs no such luck —
// it holds at every instant or the kernel is wrong.

#include <stdint.h>
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"
#include "os64/memory.h"

#define MEM_OK              0xF3EE600DUL
#define FAIL_NULL_ACCEPTED  0xF3EE0001UL   // memory(NULL) didn't refuse
#define FAIL_CALL           0xF3EE0002UL   // memory(&m) returned an error
#define FAIL_FIELDS_INSANE  0xF3EE0003UL   // ordering/zero sanity violated
#define FAIL_PAGE_SIZE      0xF3EE0004UL   // page_size zero or not a power of 2
#define FAIL_EXTENT_INSANE  0xF3EE0005UL   // largest_free_extent outside (0, free]
#define FAIL_BOOKS_1        0xF3EE0006UL   // free + used != usable (at rest)
#define FAIL_CONTRACT       0xF3EE0007UL   // available != free + reclaimable
#define FAIL_MAP            0xF3EE0008UL   // couldn't map the probe region
#define FAIL_NEEDLE_STUCK   0xF3EE0009UL   // touched 256KB, numbers didn't move
#define FAIL_BOOKS_2        0xF3EE000AUL   // books unbalanced mid-allocation
#define FAIL_UNMAP          0xF3EE000BUL   // unmap of the probe region failed
#define FAIL_NO_RECOVERY    0xF3EE000CUL   // free never recovered post-unmap
#define FAIL_BOOKS_3        0xF3EE000DUL   // books unbalanced after unmap

#define PROBE_PAGES         64UL           // 256KB at 4KB pages — big enough
                                           // to dwarf background churn, small
                                           // enough to be instant

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

static uint64_t read_memory(os64_memory_t *m)
{
    return os64_syscall1(SYSCALL_MEMORY, (uint64_t)m);
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;

    // 1. NULL is refused, not dereferenced.
    if (!failed(os64_syscall1(SYSCALL_MEMORY, 0)))
        exit_with(FAIL_NULL_ACCEPTED);

    // 2. The picture arrives and makes sense.
    os64_memory_t m1;
    if (failed(read_memory(&m1)))
        exit_with(FAIL_CALL);
    if (m1.total < m1.usable || m1.usable == 0 || m1.free == 0 || m1.used == 0
        || m1.free > m1.usable || m1.used > m1.usable)
        exit_with(FAIL_FIELDS_INSANE);
    if (m1.page_size == 0 || (m1.page_size & (m1.page_size - 1)) != 0)
        exit_with(FAIL_PAGE_SIZE);
    if (m1.largest_free_extent == 0 || m1.largest_free_extent > m1.free)
        exit_with(FAIL_EXTENT_INSANE);

    // 3. The audit. Exact, or the ledger is lying to someone.
    if (m1.free + m1.used != m1.usable)
        exit_with(FAIL_BOOKS_1);

    // 4. The kernel did the one addition userland must never redo.
    if (m1.available != m1.free + m1.reclaimable)
        exit_with(FAIL_CONTRACT);

    // 5. Consume real pages and watch the needle move the right way.
    uint64_t probe_bytes = PROBE_PAGES * m1.page_size;
    uint64_t base = os64_syscall1(SYSCALL_MAP, probe_bytes);
    if (failed(base) || base == 0)
        exit_with(FAIL_MAP);
    volatile uint8_t *probe = (volatile uint8_t *)base;
    for (uint64_t pg = 0; pg < PROBE_PAGES; pg++)
        probe[pg * m1.page_size] = 0x64;   // one touch per page = one fault = one real page

    os64_memory_t m2;
    if (failed(read_memory(&m2)))
        exit_with(FAIL_CALL);
    if (m2.free >= m1.free || m2.used <= m1.used)
        exit_with(FAIL_NEEDLE_STUCK);
    if (m2.free + m2.used != m2.usable)
        exit_with(FAIL_BOOKS_2);

    // 6. Give it back; the ledger reabsorbs it (and still balances).
    if (failed(os64_syscall1(SYSCALL_UNMAP, base)))
        exit_with(FAIL_UNMAP);

    os64_memory_t m3;
    if (failed(read_memory(&m3)))
        exit_with(FAIL_CALL);
    if (m3.free <= m2.free)
        exit_with(FAIL_NO_RECOVERY);
    if (m3.free + m3.used != m3.usable)
        exit_with(FAIL_BOOKS_3);

    exit_with(MEM_OK);
}
