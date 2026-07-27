#ifndef OS64_ABI_MEMORY_H
#define OS64_ABI_MEMORY_H

#include <stdint.h>

// Physical memory accounting, as SYSCALL_MEMORY hands it to ring 3.
//
// "Free memory" is the most-lied-about number in the history of Unix, and
// the lie was retroactive: when Linux grew a page cache, the meaning of
// "free" changed underneath every tool and human that had already learned
// it — cache counted as "used", generations of users diagnosed RAM their
// kernel had merely borrowed, a website (linuxatemyram.com) exists solely
// to apologize, and MemAvailable arrived in 2014 as a 22-year-late errata
// sheet. os64 diverges at birth: every field below has ONE meaning, forever,
// and the page cache's seat at the table is already set — empty, waiting —
// so the day a cache exists, numbers change but meanings never do.
//
// The everyday questions, pre-answered so nobody ever does Linux-style
// column arithmetic:
//
//     "how much RAM does this machine have?"       -> total
//     "how much could I allocate right now?"       -> available  (JUST this)
//     "used, the way top wants to show it"         -> used       (JUST this)
//     "is memory fragmented?"                      -> largest_free_extent
//     "do the kernel's books balance?"             -> free + used == usable
//
// That last line is not a courtesy — it is a LIVE AUDIT. free and used are
// counted in the SAME atomic walk of the allocator's extent ledger, and the
// ledger and `usable` are seeded from the same memory map, so the identity
// must hold EXACTLY, every call, forever. The day it doesn't, an extent has
// been dropped or double-counted by a merge/compaction/split bug — and any
// ring-3 program can catch the kernel's accounting red-handed with three
// loads and an add. (The boot test suite does, every boot.)
typedef struct {
	uint64_t total;        // installed physical RAM (the whole machine)
	uint64_t usable;       // governed by the allocator: total minus
	                       //   firmware/MMIO/reserved regions
	uint64_t free;         // strictly unused THIS INSTANT — never anything
	                       //   else, forever; will SHRINK when a page cache
	                       //   exists, and that shrink will be the truth
	uint64_t used;         // allocated THIS INSTANT, counted (not derived)
	                       //   from the same walk as free — see the audit
	                       //   identity above
	uint64_t reclaimable;  // droppable-on-demand (the future page cache's
	                       //   reserved seat; 0 today, honestly)
	uint64_t available;    // free + reclaimable, summed BY THE KERNEL —
	                       //   the one number "can I allocate?" wants
	uint64_t largest_free_extent;  // biggest contiguous free run: 1GB "free"
	                       //   in 2MB crumbs still can't hold a 4MB segment
	uint32_t page_size;    // the ACTIVE page size, reported live (4096
	                       //   today) — ticks() doctrine: never an ABI
	                       //   constant a binary could go stale on
} os64_memory_t;

#endif // OS64_ABI_MEMORY_H
