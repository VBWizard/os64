#ifndef OS64_MEM_H
#define OS64_MEM_H

// libos64 memory (LIBOS64.md layer). The kernel's contribution to a heap is
// exactly two calls — everything above this line is LIBRARY, and the library
// is where malloc/free/realloc live (they are Chris's to write; this header
// is the wall they build on).
//
// There is NO brk/sbrk in os64 and never will be (ratified — see DEBTS.md).
// brk's model is one heap with one movable end, which is why the classic
// malloc can only return memory to the OS from the very top. Regions are
// independent: a heap built on map/unmap can hand memory back from the
// MIDDLE, which makes brk's famous flaw structurally impossible here.

#include <stddef.h>
#include <stdint.h>

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

#endif // OS64_MEM_H
