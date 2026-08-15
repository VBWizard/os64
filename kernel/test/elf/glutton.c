// glutton.c — ring-3 fixture for the burial reclaim (task_release_vmas).
//
// exit_by_return proved the trampoline with a one-page footprint; this
// fixture exists to be FAT. It deliberately faults in every kind of page a
// real program uses — multiple text pages, an initialized .data page, .rodata,
// several .bss pages, and a mapped heap region — and then exits WITHOUT
// unmapping any of it. Everything it touched is resident at burial, which
// makes it the honest workload for test_task_teardown_leak: the undertaker
// must give back a real program's whole resident set, not a toy's single
// text page. (Chris's requirement, 2026-08-15, the day the deferral was
// paid: "the test needs to exercise the fix better — a real program with
// real multiple text pages, bss, heap pages.")
//
// Expected resident set at exit (the test asserts a conservative floor):
//   text    ~4 pages  (_start's page + three 4KB-aligned functions below)
//   .data    1 page   (initialized array, written = faulted + CoW-free path)
//   .bss     4 pages  (anonymous VMA, verified zero then written)
//   heap     4 pages  (SYSCALL_MAP region, verified zero then written,
//                      deliberately NOT unmapped — burial's job now)
// The test's floor counts only the pages that stay per-task under any future
// design (data/bss/heap = 9); text pages may one day be shared via the page
// cache and stop being per-task, and the test must not break that day.
//
// 0x0FEA57ED = "FEASTED" — the glutton finished everything on the table.

#include <stdint.h>
#include "os64/syscall.h"

#define GLUTTON_MAGIC      0x0FEA57EDUL
#define FAIL_BSS_NOT_ZERO  0x0FEA0001UL
#define FAIL_MAP           0x0FEA0002UL
#define FAIL_HEAP_NOT_ZERO 0x0FEA0003UL
#define FAIL_DATA          0x0FEA0004UL
#define FAIL_READBACK      0x0FEA0005UL

#define PAGE 4096UL
#define BSS_PAGES  4
#define HEAP_PAGES 4

static inline int failed(uint64_t v) { return (int64_t)v < 0; }

static void __attribute__((noreturn)) exit_with(uint64_t code)
{
    os64_syscall1(SYSCALL_EXIT, code);
    __builtin_unreachable();
}

// One initialized page: lives in .data, faults in file-backed, and the write
// below exercises the private-copy path (this frame is the task's own, never
// the cache's).
static uint8_t data_page[PAGE] = { 0xD7, [PAGE - 1] = 0xD7 };

// .rodata, read below so its page faults in too (it shares the data VMA's
// treatment for reclaim purposes: file-backed, per-task today).
static const char rodata_tag[] = "glutton: eats one of every page kind";

// Four untouched-by-the-loader pages: the anonymous demand-zero VMA.
static uint8_t bss_pages[BSS_PAGES * PAGE];

// Three functions pinned to their own 4KB text pages. Calling each one
// forces a distinct text-page fault — a multi-page code footprint like a
// real utility's, from a source file small enough to read. noinline keeps
// the calls real; the alignment keeps the pages distinct.
static __attribute__((noinline, aligned(4096))) uint64_t text_page_one(uint64_t x)
{
    return x * 3 + 1;
}

static __attribute__((noinline, aligned(4096))) uint64_t text_page_two(uint64_t x)
{
    return (x << 2) ^ 0x5A;
}

static __attribute__((noinline, aligned(4096))) uint64_t text_page_three(uint64_t x)
{
    return x + (x >> 3) + 7;
}

unsigned long _start(unsigned long argc, char **argv, char **env)
{
    (void)argc; (void)argv; (void)env;

    // Text: execute all three aligned pages (plus this one — four total).
    uint64_t acc = text_page_three(text_page_two(text_page_one(17)));

    // .rodata: a real read from the tag string.
    for (const char *p = rodata_tag; *p; p++)
        acc += (uint8_t)*p;

    // .data: verify the loader delivered the initializer, then dirty the
    // whole page so it is unambiguously this task's modified copy.
    if (data_page[0] != 0xD7 || data_page[PAGE - 1] != 0xD7)
        exit_with(FAIL_DATA);
    volatile uint8_t *d = data_page;
    for (uint64_t off = 0; off < PAGE; off += 64)
        d[off] = (uint8_t)(0xC0 + (off >> 6));

    // .bss: every page must arrive zero (the demand-zero guarantee), then
    // gets written so all four are resident at exit.
    volatile uint8_t *b = bss_pages;
    for (uint64_t off = 0; off < BSS_PAGES * PAGE; off += 512)
        if (b[off] != 0)
            exit_with(FAIL_BSS_NOT_ZERO);
    for (uint64_t off = 0; off < BSS_PAGES * PAGE; off += 64)
        b[off] = (uint8_t)(off >> 8);

    // Heap: map four pages, verify zero, dirty them all — and deliberately
    // never unmap. This region riding into the grave is the whole point:
    // burial, not the program, gives it back.
    uint64_t r = os64_syscall1(SYSCALL_MAP, HEAP_PAGES * PAGE);
    if (failed(r) || (r & (PAGE - 1)) != 0)
        exit_with(FAIL_MAP);
    volatile uint8_t *h = (volatile uint8_t *)r;
    for (uint64_t off = 0; off < HEAP_PAGES * PAGE; off += 512)
        if (h[off] != 0)
            exit_with(FAIL_HEAP_NOT_ZERO);
    for (uint64_t off = 0; off < HEAP_PAGES * PAGE; off += 64)
        h[off] = (uint8_t)(0xE0 + (off >> 9));

    // Read a sample of everything back so no store was imaginary. Expected
    // values are nonzero on purpose — a zero expectation can't tell a real
    // readback from an untouched page.
    if (d[64] != 0xC1 || b[256] != 1 || h[512 + 64] != (uint8_t)0xE1)
        exit_with(FAIL_READBACK);

    (void)acc;
    exit_with(GLUTTON_MAGIC);
}
