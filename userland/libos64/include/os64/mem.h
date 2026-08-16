#ifndef OS64_MEM_H
#define OS64_MEM_H

// libos64 memory (LIBOS64.md layer). The kernel's contribution to a heap is
// exactly two calls — everything above this line is LIBRARY, and the library
// is where malloc/free/realloc live (see the heap section at the bottom of
// this header; the design record is MALLOC.md).
//
// There is NO brk/sbrk in os64 and never will be (ratified — see DEBTS.md).
// brk's model is one heap with one movable end, which is why the classic
// malloc can only return memory to the OS from the very top. Regions are
// independent: a heap built on map/unmap can hand memory back from the
// MIDDLE, which makes brk's famous flaw structurally impossible here.

#include <stddef.h>
#include <stdint.h>
#include "os64/memory.h"   // os64_memory_t — the abi contract for os64_memory()
#include "os64/heap.h"     // os64_heap_report_t — the abi contract for /proc/<pid>/heap

// Allocate a fresh anonymous memory region of at least `len` bytes (rounded
// up to whole 4KB pages). Returns the region base, or NULL. The region is:
//   - DEMAND-PAGED: no physical memory moves until you actually touch a
//     page; untouched pages cost nothing.
//   - ZEROED: every page arrives all-zero, guaranteed (calloc's first
//     argument-half is free).
//   - GUARDED: an unmapped guard page follows every region — run off the
//     end and you fault immediately instead of corrupting a neighbor.
// Region base addresses are never reused after unmap (v1): a stale pointer
// into a released region always faults, never aliases new data.
void *os64_map(size_t len);

// Release an ENTIRE region previously returned by os64_map — whole regions
// only, by design (a heap gives back the regions it took; partial returns
// are bookkeeping nobody needs yet). Returns 0, or negative for an address
// that isn't a live region base. Touching the region afterwards faults —
// that's the use-after-free tripwire working, not a bug.
int64_t os64_unmap(void *base);

// Fill *out with the physical memory picture — one atomic kernel snapshot
// (see os64/memory.h for every field's meaning, fixed forever, and the
// free + used == usable audit identity). Returns 0, or negative on a bad
// pointer. The number a program deciding whether to allocate wants is
// out->available — JUST that field, no arithmetic; that's the whole point.
int64_t os64_memory(os64_memory_t *out);

// ── The heap ────────────────────────────────────────────────────────────────
//
// malloc/free over the regions above. The engine is boundary-tagged first-fit
// (Knuth 1968, refined by Doug Lea's in-use bit) with a region ledger that
// hands an emptied region straight back to the kernel — which is the thing
// brk-based allocators cannot do, since they can only shrink from the top.
// The whole design conversation, including the rulings and their dates, is
// MALLOC.md. What a CALLER needs to know is only this:
//
//   - Allocations are 16-byte aligned, always.
//   - malloc() returns memory with NO promise about its contents: a fresh
//     block from a fresh region happens to be zero (the kernel guarantees
//     that), a recycled block holds 0xA5 poison from its last free. Do not
//     rely on either — call calloc() when you mean zero, which skips the
//     memset when it can prove the memory is already zero.
//   - free(NULL) is a no-op, as it has been since V7.
//   - free() of anything that is NOT a live block from this heap KILLS THE
//     PROGRAM (Chris's ruling, 2026-08-15). So does a block whose canary a
//     neighbour has stomped. A heap that has been corrupted has already lost;
//     routing around the crime scene hides the criminal (os32's malloc
//     silently skipped bad markers — "not even a debug print... for shame").
//     The exit codes spell the crime: 0xF12EEBAD ("FREE BAD") for a bad or
//     double free, 0xCA9A12ED ("CANARIED") for a stomped block.
//   - The heap is LOCKED: threads in one task share an address space and
//     therefore share this heap. malloc is the first consumer of shared
//     mutable state os64 has had (DEBTS' thread rows called it), so it
//     carries its own lock rather than waiting for a general mutex.

// Allocate at least `size` bytes, 16-byte aligned. NULL if the kernel is out
// of memory or `size` is absurd. size 0 returns a real, freeable, minimum
// block (never NULL) — so `free(malloc(0))` is honest rather than a special
// case every caller has to remember.
void *os64_malloc(size_t size);

// Release a block from os64_malloc/calloc/realloc. NULL is a no-op; anything
// else that isn't a live block ends the program (see above).
void os64_free(void *ptr);

// count * size bytes, zeroed. Skips the memset when the memory is provably
// still virgin kernel-zeroed pages — the one place os64's map() guarantee
// pays a direct dividend. Returns NULL on overflow of count * size.
void *os64_calloc(size_t count, size_t size);

// Resize. Grows in place when the following block is free and big enough
// (the boundary tags make that check one load), shrinks in place always,
// otherwise allocates-copies-frees. realloc(NULL, n) == malloc(n);
// realloc(p, 0) frees p and returns NULL.
void *os64_realloc(void *ptr, size_t size);

// Called ONCE by libos64's own init, before main. Zeroes the heap state and
// registers the report struct with the kernel so /proc/<pid>/heap answers
// even for a program that never allocates. Idempotent.
void os64_heap_init(void);

// Walk every region, every block, and every canary, right now. Returns the
// number of problems found (0 = the heap is intact); each one is reported on
// stderr and the serial wire. This is MALLOC_CHECK done honestly: a bug-hunt
// tool a program can call between suspicious operations, not a mode that
// quietly changes the allocator's behaviour.
uint64_t os64_heap_verify(void);

// Tell the kernel where this program's heap report lives, so procfs can
// render /proc/<pid>/heap. LIBRARY PLUMBING — os64_heap_init calls it once
// and no application ever should. It lives beside os64_map rather than in
// heap.c so that the heap engine itself makes no syscalls directly and can be
// unit-tested on the HOST (tools/test_heap_host.c) with these three functions
// stubbed out. Pass NULL to withdraw the registration.
int64_t os64_heap_publish(const os64_heap_report_t *report);

#endif // OS64_MEM_H
