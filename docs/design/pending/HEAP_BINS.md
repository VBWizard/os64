# HEAP_BINS.md — where a page's layout time went, and the two fixes

*Fable 5.1, 2026-10-28. A design, not code: Chris asked for the changes to
be designed and not written. The rows are in DEBTS.md (§ Heap "First-fit is
O(free blocks)", § Browser layout "A declaration block's parse starts with
no scratch"); this note carries the argument and the shape of each fix.*

## 1. What was measured

The project's own pull-request page, 644 KB of HTML, 3,907 elements, 87
deep, 22,415 CSS rules, took 22 s to show on the P5 and 11.7 s of that was
the status line's "laid out in". Two per-second logs of the window's task,
`/proc/<task>/status` and `/proc/<task>/heap`, split it:

| the 11 s stretch, each second | |
|---|---|
| user time | ~1000 ms |
| syscall time | 0 ms |
| page-fault time | 6 to 9 ms |
| mallocs | 20 to 50 |
| reallocs | ~2,700 |
| frees | ~2,700 |
| free blocks on the heap's list | 11,740, unchanging |
| fragmentation | 99% |

Every CPU on the machine measured the same with `coreprobe`, so the thread
was at full speed, alone, in user mode. The same layout under the host's
glibc takes about 2 s and its profile is the cascade's selector matching.

The heap is one doubly-linked free list, first-fit (MALLOC.md rule 3).
`heap_alloc` walks it, canary check per node, before it carves from a pool's
virgin frontier, and a realloc that cannot grow in place is a `heap_alloc`
plus a copy plus a free. Eleven thousand small leftovers sit at the front
of that list, which is where first-fit keeps them, and none can hold a
4 KiB request, so every such request visits all of them and then carves.
2,700 visits of 11,740 nodes a second through scattered memory is the
thousand milliseconds.

Who makes 2,700 reallocs a second? A gdb tally on the host replay, same
code, same page: 30,849 reallocs per layout, 22,176 of them libgarb's
parser growing a block's scratch from nothing, one per CSS rule, because a
cascade reads every rule's declaration block through a fresh parse
(DEBTS § Browser layout "Every cascade reads its sheets again") and each
parse starts with no scratch and frees it when the block ends. The rest are
the layout's own growing lists: table and flex scratch (2,974), background
layers (1,470), text segments, line records.

After the page is up the window's thread holds 7% of a core: 245 mallocs a
second for the animation ticks (the page's 35 loading spinners rotate
forever, their custom elements never having run), each walking the same
list.

Nothing in the page's own code asks for any of this. It is three choices of
ours stacking: the walk multiplies, the cascade's re-reading supplies
22,000 requests to multiply, and the parser's empty scratch turns each read
into an allocation. Fix the first and the other two go cheap; fix the second
and the first is still hit by everything else, the idle loop included.

## 2. Part A — the parse's scratch is kept by its owner (libgarb, size S)

*Built 2026-10-30 as designed here, so "Where it is" describes the code
before it. The proof is `test_garb_host.sh --scratch` and
`tools/test_garb_mutants.py`; the P5 measurement is still to take.*

**Where it is.** `parse.c`: a `Parse` owns `scratch`/`scratch_cap`/`top`,
the stack every list is built on and copied off at close (one stack serves
the nested lists). `run` ends by freeing the scratch. `reparse` — behind
`garb_items_of` and `garb_rules_of` — makes a fresh `Parse` over an owner's
arena for every block the cascade reads, so a cascade of N rules allocates
and frees N scratches.

**The change.** A `garb_parsed_t` gains a kept scratch: `scratch`,
`scratch_cap`. `reparse` borrows it into its `Parse` (`top` 0), and `run`'s
ending hands it back grown — `owner->scratch = p.scratch`,
`owner->scratch_cap = p.scratch_cap` — instead of freeing. `garb_free`
frees it with the arena. `parse_text` (a sheet's own parse, on a worker)
keeps today's behaviour: its result's scratch is freed at the end, since a
sheet is parsed once and would otherwise carry a dead buffer for its life.

**What this makes true.** A cascade allocates scratch once, however many
rules it reads: the first block grows it to 4 KiB, a larger block doubles it
a few times, and it stays. 22,176 malloc/free pairs become about two. The
starting size (Chris's instinct: measure it on the corpus) becomes a
refinement, because the doubling settles within the first handful of blocks
whatever it starts at; the fix is the keeping, not the sizing.

**Invariants to state in the code, because the kept buffer is shared state
where there was none:**

- A `garb_parsed_t`'s kept scratch is used by one `reparse` at a time. True
  today: `run` never calls `reparse` (no re-entry), and the cascade's reads
  all go to the cascade's own owner, `c->reads` when its arena could be
  made and `c->self` when not, on the window's thread. `garbdump` is
  single-threaded. The only sheet-owned reads would be a cascade whose
  read arena failed AND whose `into` fell back to the sheet — it does not,
  `into` is always the cascade's — so no sheet is ever reparsed from two
  threads. Write the rule down at the field.
- A kept scratch is bounded by the largest block the owner ever read, which
  `GARB_SHEET_MAX` bounds. No cap is needed; a comment says why.
- `p_spent` paths that today free the scratch on a short read must hand it
  back the same way; a lost scratch is a leak the host test would see.

**Proof.** The libgarb host suite as it stands (`tools/test_garb_host.sh`),
plus one case: a cascade over a sheet of N rules under a counting
allocator makes O(1) scratch allocations, not O(N). libgarb has no mutant
harness today; this slice adds one on `mutants_parallel.py` with two
mutants, a `run` that frees per block and a `reparse` that forgets to hand
the grown buffer back, each of which must fail that case. On the P5: the
PR page's "laid out in" and `calls_realloc` across a reload before and
after.

**Expected on the P5:** this page's 11.7 s falls by the 8 s the parse
scratch walks cost; the ~3 s of other walks and the ~1 to 2 s of cascade
matching remain, as does the idle 7%.

## 3. Part B — size-class bins in the heap (libos64 `heap.c`, size M)

**What stays exactly as it is.** The block header (16 bytes, size|flags and
canary), the free block's links and footer in its payload, `HEAP_MIN_BLOCK`
48, `HEAP_DIRTY`, the poison, the canaries and every death, boundary-tag
coalescing at free time, pools and dedicated regions, the give-back of an
empty region, the primordial pool's `KEEP`, realloc's shrink and in-place
growth, `HEAPCHECK`. Bins replace one thing: the single `gFreeList` and the
walk that searches it.

**The classes.** Block sizes are multiples of 16 from 48 up; a pool is 1 MiB
so a fully merged free block can reach about that; anything the program
asks for at 128 KiB or more is dedicated and never on a list.

- Exact bins for 48 .. 1,024 in steps of 16: 62 bins, every block in a bin
  the same size, so the head always fits.
- Logarithmic bins from 1 KiB to 1 MiB, four per doubling (the TLSF
  spacing the kernel's frame table already uses): 10 doublings, 40 bins.
  Blocks in one bin vary within a quarter-octave.
- One top bin for anything larger. About 103 heads in all, a static array,
  plus a 2-word bitmap of the non-empty bins.

`bin_of(size)` is a few shifts and a `clz`; it is the only place the class
boundaries live.

**The operations.**

- *Insert* (`free_list_insert`): push at the head of `bin_of(size)`, set
  the bit. LIFO is fine; nothing today relies on list order.
- *Remove* (`free_list_remove`): unlink through the block's own `prev`/
  `next`; when `prev` is NULL the head is `bins[bin_of(size)]`, which the
  block's size recovers, so the coalesce paths and realloc's in-place growth
  need no change; clear the bit when the bin empties.
- *Find* (`free_list_first_fit`): b = `bin_of(need)`. If bin b is an exact
  class and non-empty, take its head. Otherwise scan bin b first-fit (a
  quarter-octave of sizes, short), and if nothing there fits, take the head
  of the next non-empty bin above it — found by the bitmap and `ctz`, and
  guaranteed to fit because every block there is at least its class's
  floor, which is more than `need`. NULL only when no bin above is set,
  which is the carve-from-frontier case as today. A request never meets a
  block smaller than itself. The walk the P5 measured becomes a bitmap
  word and at most one short bin.
- *Largest free*: the highest set bit names the bin; scan that bin alone.
  `free_list_rescan_largest` keeps its name and loses its cost.

**The report** (`abi/include/os64/heap.h`, version 1 → 2, appended fields,
the kernel validates the version before reading them): `walk_steps`, free
blocks examined by allocations since start, which is the number this whole
hunt lacked and would have answered it from one `cat`; and `bins_used`,
non-empty bins now. `largest_free` and `blocks_free` keep their meaning.

**The verifier** (`heap_verify_locked`): walks every bin, requires each
block's `bin_of(size)` to be the bin it sits in, its canary FREE, and the
bins' total to equal `blocks_free`; the bitmap must say set exactly for the
non-empty bins. The regions walk it already does stays.

**Proof.**

- `tools/test_heap_host.c` gains: the measured shape — ten thousand small
  free fragments then 4 KiB requests, asserting `walk_steps` stays under a
  small constant per request; split, merge-forward, merge-backward,
  merge-both and give-back re-run under the bins with `verify` after each;
  realloc growing over a free successor that sits in a bin.
- A new `tools/test_heap_mutants.py` on `mutants_parallel.py`, one mutant
  per rule: insert into the wrong bin; forget to clear the bitmap bit; take
  the head of the request's own logarithmic bin without the first-fit scan;
  stale `largest_free`; `walk_steps` not counted. Each must fail the host
  suite. (Run once, at the end, at most four workers, one run at a time:
  the machine-load rule of 2026-10-27.)
- In the OS: `malloctest` and `mallochavoc` as they stand.
- On the P5: `/proc/<yonder>/heap` across a reload of the PR page —
  `walk_steps` per second, `calls_realloc` per second — and the status
  line's "laid out in"; and the window thread's share of a core with the
  page idle.

**Expected on the P5:** the PR page lays out in about the host's 2 s, with
cascade matching as the next thing to look at (its own row); the idle 7%
becomes the ticker's real cost, well under 1%; every page's stream phase,
which made 10,000 to 25,000 mallocs a second against a growing list, gets
its walks back too.

## 4. Order, ownership, and what to re-decide

Part A first: it is a Tuesday in libgarb with no design question in it, and
it takes the biggest single customer away. Part B second, when heap.c is
opened. Part A does not make Part B optional and Part B does not make Part
A pointless: with bins, 22,000 allocations a cascade are cheap but still
22,000; with the kept scratch, every other allocation on every page still
walks.

Malloc is Chris's by the labour division, so Part B is his to write or to
hand off, and the reviewer should be whoever did not write it. The bins
reverse MALLOC.md rule 3's ruling; the design record should say so in the
same commit, with today's numbers as the reason, and the comment at
`free_list_insert` that explains the ruling must change with the code.

Not in this note: reading sheets once at parse time (the cascade row,
size M, a different slice), the pool's 256-slot request cap (its own row),
and the custom elements that would make the spinners stop (bookmarked).
