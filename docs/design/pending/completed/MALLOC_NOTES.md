# MALLOC_NOTES.md — Fable's input for the malloc arc (2026-08-13)

Input, not a spec: Chris and Opus own the design conversation. These are the
things I'd want said at the table before the first line is written. Nothing
here is a ruling — the rulings are Chris's.

## The shape I'd recommend (the "stock engine," done the os64 way)

A **size-class segregated allocator over mmap arenas** — the modern
convergent design (dlmalloc grew into it; jemalloc/tcmalloc/mimalloc all
landed there):

- **Small allocations** (≤ ~2KB): carved from size-class bins inside arena
  chunks. Free = push onto the class's free list. O(1) both ways.
- **Large allocations** (above a threshold, classically 128KB): straight
  mmap, munmap on free. No fragmentation story at all for the big stuff.
- **Arenas**: big virtual mmap regions grown lazily. os64's demand paging
  makes this free — map generously, fault in only what's touched. Do NOT
  pre-fault or pre-commit; the VMA machinery is the whole point.
- **v1 concurrency**: one lock per task heap is FINE. But leave the arena
  struct plural from day one (arena pointer in the header or per-thread
  arena index) so per-thread arenas can arrive later without a rewrite.
  The paging-arena work just walked this exact road in the kernel.

## The four decisions to make BEFORE coding (each is a Chris ruling)

1. **The zeroing contract.** The kernel zeroes every fresh page (conscious
   design — see the allocator choke point). But RECYCLED user blocks are
   not re-zeroed by anyone. So: does free() poison? does malloc() re-zero
   recycled blocks, or only calloc()? My lean: malloc returns dirty
   recycled memory like everyone since V7 (document it!), calloc zeroes,
   and free POISONS with a pattern (see tripwires below). Never add a
   redundant memset for fresh-from-kernel pages — the house rule.
2. **Alignment.** 16 bytes minimum. Userland is SSE2 since the FPU slice
   (2026-08-27), and an aligned vector move through a heap pointer faults on
   anything less.
3. **The mmap threshold and arena chunk size.** Boring numbers, but pick
   them out loud (128KB / 1-4MB are the classics) and name them in
   CONFIG-style constants, not magic literals.
4. **Scope fence: this arc is USERLAND malloc over the EXISTING map
   syscalls.** If the design conversation drifts into "kmalloc needs a
   freelist too" — it does, someday, but that touches kMemoryStatusLock
   and the page-fault-path allocation rules, which is dragon country.
   Book it as its own vegetable; do not ride it into this arc.

## The "something cool" Chris wished for — it exists, and it's very os64

Nobody's stock malloc engine is interesting. What IS interesting is what
this house uniquely does: **legibility and tripwires**.

- **`/proc/<pid>/heap`** — malloc renders its own bins: size classes,
  blocks live/free per class, arena count, bytes mapped vs touched,
  fragmentation percent, high-water mark. Every process's heap becomes a
  cat-able file, same key<TAB>value grammar as the rest of /proc, parsed
  by the same libos64 reader. `watch -n 1 "cat /proc/53/heap"` while a
  program runs = a live heap profiler for free, using only tools that
  already exist. No production allocator gives you this without LD_PRELOAD
  gymnastics. Here it's ~40 lines in the synth-text style. THIS is the
  cool thing.
- **Tripwires, the house culture, now at ring 3:**
  - Header canary per block (the os32 malloc canary lineage continues — a
    third generation of the same idea).
  - free() poisons the body (0xA5 or similar). Use-after-free reads
    garbage LOUDLY instead of working by luck. This is the userland
    sibling of the kernel's lazy-HHDM unmap tripwire — same philosophy,
    one privilege level up.
  - Double-free detection: canary state flips on free; a second free
    panics the program with the block address (exit code + stderr, not
    silence).
- **A heap-verify walk** (debug knob or env var): validate every canary in
  every arena on each malloc/free. Slow mode for bug hunts — MALLOC_CHECK
  done honestly.

## Practical notes for the implementer

- House conventions: stdint names, Intel asm if any, generous comments
  explaining intent, -mno-red-zone in any fixture CFLAGS, no retrofit
  sweeps of existing call sites.
- Tests: a ring-3 fixture (malloc_test) riding the existing
  fixture/KERNEL_FIXTURES machinery — exercise split/coalesce/recycle,
  canary trip on deliberate overflow (expect the kill), double-free trip,
  large-mmap path, and calloc zeroing. Plus /proc/<pid>/heap parse test
  if the file ships in this arc.
- The existing userland malloc (libos64) is the os32 descendant — read it
  first for the canary heritage and to inventory every current caller's
  assumptions before swapping engines. No caller should notice anything
  but speed.
- free(NULL) is a no-op. realloc grows in place when the neighbor is free;
  the naive copy fallback is fine for v1.

— Fable, with 30% of a tank and full confidence in the relief crew 💚
