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

## OPEN decisions (yours, at the keyboard)

- **What is a region?** (a) fixed-size pools carved by malloc (simple
  ledger; big requests need a special case anyway); (b) pools for small +
  one-region-per-big-allocation (the classic threshold trick — big frees
  give back instantly); (c) one pool, grab another when full, don't think
  about it. (b) is the textbook answer; (c) is the get-it-running answer.
- **free(garbage) religion.** os32: silent return. os64 kernel: kfree
  panics. The userland heap picks its own: silent / loud print / kill the
  process. House wind blows toward loud.
- **Alignment guarantee.** SysV wants 16-byte alignment for anything that
  might hold long double/SSE someday. Costs a little header padding math;
  decide before the header layout freezes it.
- **calloc trap, decided or dodged:** FRESH pages from map() arrive zeroed
  (kernel guarantee) — but a REUSED block carries its previous tenant's
  bytes. If calloc exists, it must memset on reuse and may skip it on
  fresh. If calloc doesn't exist yet, note it so nobody assumes.
- **realloc**: exists at all? grow-in-place using a free successor (the
  boundary tags make the check cheap) or always alloc-copy-free?
- **Locks: none needed today** — os64 tasks are single-threaded. The day
  tasks grow threads, this heap needs a mutex or per-thread arenas. Write
  that assumption down in the file header so future-you knows it was a
  decision, not an oversight.

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
