# MALLOC.md — the design conversation, recorded before the writing

*2026-07-19. Chris designs and writes this malloc — that was called long ago
and it holds. This document records the design conversation we had so the
thinking survives until the writing happens, whoever is or isn't in the
other chair that day. Decisions marked RATIFIED were made by Chris in that
conversation; decisions marked OPEN are his to make at the keyboard.*

## Prior art: os32's malloc, named with today's vocabulary

os32's allocator was an **in-band, header-per-block** design and it worked
for years:

```c
typedef struct sheap    //20
{
    uint32_t marker;    //4   <- a HEAP CANARY, independently invented
    uint32_t len;       //4
    bool inUse;         //1
    uint16_t uses;      //2   <- generation counting, also independently invented
    uint8_t filler;     //1
    struct sheap* prev, *next; //8
} heaprec_t;
// payload followed the header: retVal = heapPtr + sizeof(heaprec_t)
```

The `marker` protocol — set in mallocI(), verified in freeI() (bad marker =
silent return: double-free and wild-free protection) and verified again in
mallocFindAvailableMemory() before reusing a chunk — is exactly the canary
idea the industry named in 1998. os32 had it without the name.

What os32 never got: **coalescing**. Adjacent free chunks stayed split
forever. That is the one structural upgrade round two makes.

## The wall this is built on: the map/unmap contract

(Authoritative text lives in `userland/libos64/include/os64/mem.h` — read
it before writing a line. Summary:)

- `os64_map(len)` returns a fresh **region**: page-rounded, demand-paged,
  **guaranteed zeroed**, with an unmapped **guard page** immediately after
  it. The kernel chooses the address; there is no address parameter.
- `os64_unmap(base)` releases an **entire region by its exact base** —
  the same pointer map() returned, nothing else. No partial returns, no
  middle-of-region trims. The kernel validates the base strictly and
  rejects anything that isn't a live region.
- **There is no brk/sbrk and never will be** (ratified long since — see
  DEBTS.md). Regions are independent; that independence is the whole gift.

### The address-space question, asked and answered

Chris asked: when a region is unmapped, its addresses become a dead hole —
should malloc re-request that space to avoid "wasting" it?

**No, and the no is load-bearing.** Ratified v1 kernel behavior: region
addresses are **never reused** after unmap. The hole is dead *on purpose* —
a stale pointer into a freed region faults forever instead of silently
aliasing whatever got allocated there next. It is the userland twin of the
kernel's HHDM unmap-on-free tripwire, and tripwires are why #PFs stopped
being a daily hunt and started being arrest warrants.

The cost is virtual addresses only — the physical memory went home the
moment unmap returned. User space is 2^47 bytes ≈ 128 TB of addresses; a
malloc that mapped and unmapped a 64KB region every second would take
**68 years** to burn through it. The hole is made of nothing. Let it be.

(If a future need ever demands address hints — MAP_FIXED-style — that is a
syscall ABI change AND a deliberate disarming of the tripwire. Decide it
then, out loud. v1 declines.)

## RATIFIED design decisions (Chris, 2026-07-19)

1. **In-band metadata** — header glued to each block, os32 style.
   "Apparently I really did like that design." The stomping worry is met
   by (2) and (4), not by moving the metadata out-of-band.
2. **Boundary tags** (Knuth, 1968): the block size recorded at BOTH ends
   of every block. Freeing peeks one word below (predecessor's footer) and
   one word past (successor's header); both merges are O(1), at free time,
   no re-jiggering pass ever. This is the coalesce os32 never got. What
   Chris liked most: *it happens when the memory is freed* — no scheduling
   question exists.
3. **One free list, first-fit.** Size-class bins were presented and
   ruled OVERHEAD for this OS (verdict delivered from the floor, laughing).
   Knuth's own baseline ran the world's timesharing systems on exactly
   this. The code will be right there if regret ever arrives.
4. **Canary/marker, round two** — the os32 marker protocol returns. One
   change urged from experience: the os32 silent-skip on a bad marker at
   reuse time ("not even a debug print... for shame" — Chris, about his own
   code) should be LOUD. A failed canary means someone already stomped the
   heap; routing around the crime scene hides the criminal.
5. **The region ledger + empty-region give-back.** malloc keeps one entry
   per region the kernel granted ("base X, N bytes"). Boundary tags make
   the payoff a one-comparison side effect of free: if a just-merged free
   block spans its entire region, unmap the region — memory genuinely
   returned to the kernel, from anywhere in the heap. Every brk-based
   malloc in history could only shrink from the top; this one hands back
   any region the moment it empties. The toy outdoes the classic,
   structurally.

## BUILT — 2026-08-15 (Opus, at Chris's direction)

The heap exists: `userland/libos64/heap.c`, `<os64/mem.h>` for its face,
`tools/test_heap_host.c` for its unit tests, `/bin/malloctest` for its
in-OS proof, `/proc/<pid>/heap` for its self-portrait. Every RATIFIED
decision above was built as written; the OPEN list below was ruled on the
day and each ruling is recorded there. What the engine actually is:

- **Boundary tags with Doug Lea's refinement.** Knuth's footer is carried
  only by FREE blocks; an in-use block pays a 16-byte header and nothing
  else, because a `PREV_FREE` bit in the successor's header says whether
  looking below is even worth it. Merge forward, merge backward, merge
  both — all O(1), all at free time.
- **A virgin frontier per pool.** New blocks are carved off untouched
  region space before the free list is consulted for growth, which is what
  makes `calloc` free for first-touch allocations: those pages are the
  kernel's own zeros and nobody has written to them since.
- **The give-back, as promised.** A pool whose last live block is freed is
  handed back with `unmap` — from the middle of the heap, which brk cannot
  do. The PRIMORDIAL pool is exempt: a malloc/free loop would otherwise
  map and unmap a megabyte per iteration, and region addresses are never
  reused, so that churn spends address space for nothing.
- **The audit identity** (abi/os64/heap.h): mapped == live + free +
  overhead + virgin, checked by procfs on every read, printed as `audit
  ok` / `audit BROKEN`. The allocator's books can catch their own author.

### Threads and the heap (asked and answered, 2026-08-15)

**Threads share the task's address space, so they share ONE heap.** The
heap's state is ordinary process globals — every thread of a task sees the
same region ledger, the same free list, the same pools. Nothing is
inherited or copied at thread creation, and a block allocated by one thread
may be freed by any other. (A SPAWNED PROCESS is the opposite: its own
address space, its own libos64 data, its own heap, sharing nothing.)

That makes malloc the first genuine consumer of shared mutable state in
os64 userland, which is why the heap carries its own lock. `malloctest
threads N` is the proof: N threads hammering the shared pools, verifying
their own stamps, and deliberately freeing each other's blocks through an
atomic handoff table.

Writing that test immediately crashed the OS — and **the heap was
innocent.** See MEMORY.md's fingerprint: the kernel's demand pager did
`seek` then `read` on a `vfs_file_t` whose position all threads share, so
two threads faulting on different code pages each got the other's file
offset and executed a page of valid machine code from the wrong part of the
binary. malloc was simply the first workload in this OS's life that made
several threads of one task execute enough distinct code to fault pages in
simultaneously. Fixed in the kernel (`pos_lock`); the heap test now runs
clean with 8 threads on cold text.

### The bug this design caught on its first day

The frontier carver needed to know whether the block below it was free, and
the first version asked the obvious question the wrong way: it read the 8
bytes underneath as the predecessor's footer. An IN-USE block has no
footer — those bytes are the program's data. A program whose last word
happened to spell a plausible block size would hand itself a boundary tag
that lied, and the next `free()` would merge backwards into live memory.

The 20,000-round host soak never saw it (its stamp bytes never spelled a
plausible size). `malloctest churn` on the real OS hit it in under a
minute — and the canary caught it, killed the program, and named the crime
("the block below this one is not the free block its tag claims") instead
of letting it corrupt anything. The fix is that the frontier's header
carries `PREV_FREE` like every other block's, maintained by the free path;
the regression test is `t_frontier_tag`, which fails against the old code
with the identical message. **Tripwires over silence, paid off inside one
afternoon.**

## OPEN decisions — RULED 2026-08-15

- **What is a region?** RULED **(b)**: pools for small allocations, one
  dedicated region per allocation of 128KB or more. The threshold and the
  1MB pool size are named constants (`HEAP_BIG_BYTES`, `HEAP_POOL_BYTES`).
  The argument that decided it: big blocks are exactly where a first-fit
  list fragments worst, and exactly where the kernel already has the
  perfect answer — an independent region that `free` hands straight back.
- **free(garbage) religion.** RULED: **kill the process.** ("Yes, killing
  tasks is absolutely the right way to go on free(garbage)!") A wild free,
  a double free, a misaligned pointer, or a stomped canary prints the crime
  to stderr AND the serial wire, then exits with a badge that spells it:
  `0xF12EEBAD` ("FREE BAD") or `0xCA9A12ED` ("CANARIED"). A corrupted heap
  has already lost; continuing only moves the crash somewhere less
  informative.
- **Alignment guarantee.** RULED: **16 bytes**, always. The header is
  16 bytes, so a 16-aligned block yields a 16-aligned payload with no
  padding math at all. SSE is still #UD at ring 3, but a malloc ABI is
  forever and the FPU-state slice will land some day.
- **calloc trap.** RULED as designed: fresh pages arrive zeroed and are
  handed over WITHOUT a memset; a recycled block carries a `HEAP_DIRTY`
  bit and gets the memset it deserves. Blocks carved off a pool's virgin
  frontier count as fresh, which is most of what a starting program does —
  so os64's zeroed-region guarantee shows up as calloc being free.
- **realloc**: exists, and grows in place when the successor is free
  (boundary tags make it one load), shrinks in place always, and falls back
  to allocate-copy-free otherwise. `realloc(NULL, n)` is `malloc`;
  `realloc(p, 0)` frees and returns NULL.
- **Locks: NEEDED NOW — this line was overtaken by events.** os64 grew
  real ring-3 threads on 2026-08-02, and threads share one address space
  and therefore one heap. malloc is the first genuine consumer of shared
  mutable state in os64 userland (DEBTS' thread rows predicted it would
  be), so the heap carries its own lock: test-and-set, a short `pause`
  spin, then `os64_yield`. Deliberately NOT a general mutex — that gets
  designed when a PROGRAM needs one, not retrofitted from a library's
  dozens-of-instructions critical section.

## House rules that apply

- stdint names throughout (`uint32_t marker` was already right in 2016).
- The type→format table lives in fmt.h when the debug prints start.
- Guard pages catch runs off a region's END for free; canaries are for
  neighbor-to-neighbor stomps INSIDE a region.
- Test the way everything else here is tested: a fixture that allocates,
  frees, coalesces, exhausts, double-frees, and stomps ON PURPOSE — the
  suite should prove the tripwires fire, not just that the happy path runs.

*The first one took a few days plus a tiny bit of debugging. This one has
its design conversation written down before line one, which the first one
never got. — recorded by Fable 5, who will be reading the commit log even
if not sitting in the chair. 🍩*
