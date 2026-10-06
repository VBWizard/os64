# FRAMES: the physical allocator at the scale of the machine

Status: DESIGN, approved by Chris and reviewed by Fable (2026-10-06).
Phase 1 is approved to build; Phase 2 waits on its P5 measurement.
Evidence: Quinn's P5 diagnostic (`codex/p5-image-load-diag`,
`docs/design/pending/P5_IMAGE_LOAD_DIAGNOSTIC.md`, the DEBTS.md handoff
"Physical allocator scalability"). Author: Opus.

## The failure

Yonder loading the 007 Museum page on the P5 grows the allocator's ledger
from about 2,800 entries to 70,767, 66,220 of them live, page-aligned,
exactly-4 KiB extents: roughly 255 MiB of demand-faulted heap, which is
what a 256 MiB retained-image budget looks like. At about 200 images the
ledger reaches its 100,000-row wall and the kernel panics. Long before
that, external commands and htop stall for seconds, and the counters show
why: an ordinary allocation examines 48,000-63,000 ledger rows, under one
global spinlock, with interrupts off, 15,000-17,000 times every ten
seconds.

None of that memory is a leak. Almost all of it comes back when Yonder
exits. The machine was asked to keep 66,000 pages alive, had the RAM to do
it, and could not, because the allocator's bookkeeping is the wrong shape
for the job.

## What the allocator is today

`kernel/src/memory/allocator.c` is ONE ledger, `kMemoryStatus`: a flat
array of `{startAddress, length, in_use}` extents covering every usable
byte of RAM, byte-granular (8-byte rounding). (CLAUDE.md calls it
"bitmap-based"; it is not, and the code commit fixes that line.) Every
consumer shares it:

- **page frames**: demand-faulted anonymous pages (`vma_resolve_backing_page`,
  one `allocate_memory_aligned(PAGE_SIZE)` per fault), CoW copies, task
  stacks with guard pages, GUI canvases, NIC rings, the paging pool;
- **kernel objects**: every `kmalloc`, from a 24-byte list node to a 16 MiB
  log ring, carved at 8-byte granularity from the same extents.

Every operation is a linear walk of that array under `kMemoryStatusLock`
(irqsave):

| Operation | Walks |
|---|---|
| allocate (any size) | full table when no exact-size hole exists (exact-fit-first) |
| free | one walk to find the extent by containment, one more to find merge neighbours |
| "is this page live?" (`phys_page_live_locked`) | full table, five times per page copied out of a task by `allocator_copy_from_task_va` (four table levels plus the leaf) |
| memory snapshot (`SYSCALL_MEMORY`, top) | full table |
| health report, maintenance, compaction | full table |

Three costs follow, and the P5 run shows all three.

1. **Capacity.** The array is fixed at `INITIAL_MEMORY_STATUS_COUNT`
   (100,000 rows) and cannot grow, because it is the allocator's own
   metadata and growing it would allocate through itself. One row per live
   page means 100,000 live pages (390 MiB) is the ceiling on a machine with
   gigabytes free.
2. **Search cost grows with live allocations, not with fragmentation.**
   The exact-fit-first policy (2026-08-07) cured the table bloat of that
   day, when free holes accumulated. Today's table is full of LIVE rows,
   and each allocation that finds no exact hole walks all of them.
3. **Every lookup scans, not just allocation.** A free is two walks.
   htop reading each process's command line goes through
   `proc_add_task_string` → `allocator_copy_from_task_va`, which costs five
   full walks per page with interrupts off. Burying Yonder frees about
   66,000 pages, one `free_memory` each: two walks apiece against a 70,000-row
   table, which is billions of row visits on the way out.

A bigger table fixes cost 1 and worsens 2 and 3. An index over the same
flat array fixes 2 and leaves 1. Quinn's handoff says exactly this, and it
is the frame for everything below.

## What must survive the change

These are the properties the current allocator earned the hard way. The
new design keeps every one; the "Where it lives" column is where to check.

| Property | Why it exists | Where it lives after |
|---|---|---|
| Allocated ⇔ HHDM-mapped; freed memory unmapped (the use-after-free tripwire) | lazy HHDM, July 2026 | Frame table: map on alloc, unmap + shootdown on free, per run. Inside a lent LEDGER chunk the ledger owns HHDM state, as today (Phase 1 below) |
| Zero on allocate, at one choke point | stale data never leaks between owners | frame table and ledger (Phase 1), object heap (Phase 2); after the HHDM map |
| Poison on free (`ALLOCATOR_POISON_ON_FREE`) | caught a freed `thread_t` instantly, 2026-08-15 | every layer that frees |
| A free must name an allocation's base, never its interior (exact-base tripwire) | the scribbled-text hunt, 2026-08-14 | frame table: the frame must be a run HEAD; ledger: unchanged; object heap (Phase 2): an object boundary in a slab |
| Repeating sizes recycle their own holes | the 2026-08-07 procfs churn autopsy | frame table: exact-length run lists; ledger: unchanged exact-fit; object heap (Phase 2): size classes recycle by construction |
| `free + used == usable`, exactly | the ledger-drift tripwire `memory_test` asserts every boot | O(1) running counters, plus an audit walk tests can call |
| Page 0 and non-usable memory are never handed out | | frame table marks them RESERVED at init |
| Fallible allocation refuses before changing ownership, with HHDM tables prepared first (`allocate_memory_try`) | | unchanged contract |
| Fault and IRQ contexts may allocate; nothing sleeps | demand paging, CoW | irqsave spinlocks, bounded critical sections, no scans under the frame table's lock |
| No metadata allocation through the allocator it describes | | frame table sized once at boot; the ledger's table stays fixed; slab headers (Phase 2) live inside slab pages |

## The design: two layers, split by granularity

Today one structure serves two populations with nothing in common: a
handful of thousands of small, oddly-sized kernel objects, and tens of
thousands of page frames that are all the same size. The design gives each
the structure that fits it. This is the shape every Unix-lineage kernel
arrived at: a page-frame allocator underneath (boundary tags are Knuth's,
TAOCP vol. 1, 1968), and a size-class object allocator on top (Bonwick's
slab allocator, USENIX 1994).

**It is built in two phases, and the second is conditional** (Chris,
2026-10-06). Every P5 symptom MEASURED so far is page-shaped, so Phase 1
is the frame table alone, with today's ledger demoted to serving small
`kmalloc` from pages the frame table lends it. Phase 2, the object heap,
is built only if measurement after Phase 1 shows the ledger still matters,
by its scan cost OR by its capacity (see the residual wall below). Both are
described here so that Phase 1 leaves the right seam.

**The GitHub reproduction is page-shaped too** (Chris, P5, 2026-10-06).
`github.com/gkostka/lwext4` reached the wall in about two minutes when
navigated to from other pages, but loads when opened directly. Opened
directly, it takes the ledger from 8,155 rows to 69,878: +57,360 live
exactly-4 KiB rows (224 MiB) of +61,723, and +291.8 MB live, so one page
alone reaches 70% of the wall, and arriving with an earlier page's memory
still live crosses it. Searches examined 44,412-47,112 rows each while it
loaded (5,848 at idle). When Yonder exited, the next ten-second report came
6.7 seconds late, with 65,158 merges in between: one full-scan free per
page, holding the lock. Phase 1 covers this case; it is no evidence for
Phase 2. Why the page needs 278 MiB is a separate Yonder question.

### Layer 1: the frame table

**One descriptor per 4 KiB frame**, from frame 0 to the highest usable
frame, in one array placed at boot:

```c
typedef uint64_t frame_t;  // 8 bytes: state (4 bits) | a (30 bits) | b (30 bits)
```

What `a` and `b` hold depends on which descriptor of a run it is:

| Descriptor | `a` | `b` |
|---|---|---|
| head of a run of 2+ frames | length in frames | FREE: free list `next`; LEDGER: overlap count |
| tail of a run of 2+ frames | length in frames | FREE: free list `prev`; LEDGER: overlap count |
| a 1-frame free run (head and tail at once) | free list `next` | free list `prev` (length is 1 by its state) |
| allocated 1-frame run | length (1) | LEDGER: overlap count |
| interior frame | unused | LEDGER: overlap count (live ledger extents overlapping this page) |

Every free run is therefore on an ordinary doubly linked list, so a
neighbour absorbed by a coalesce unlinks in O(1). Frame numbers are 30 bits:
4 TiB of RAM.

A **run** is a span of contiguous frames with one owner, free or
allocated. Its head and tail descriptors both carry its length: Knuth's
boundary tags. That makes the neighbours of any run reachable in O(1) on
free: the frame just below a run is some other run's tail, and the frame
just past it is another run's head. Interior descriptors carry only their
state. That is all a liveness check needs.

**Capacity is RAM.** The table describes every frame there is, so it cannot
fill before memory does. The 100,000-row wall disappears rather than
moving. The cost is 8 bytes per 4 KiB, 0.2% of RAM, allocated once at
boot, plus the holes in the physical map below the highest usable frame:
44 MiB for the P5's 22 GiB, 16-20 MiB on QEMU's 8 GiB guest depending on
where it places RAM above the 4 GiB hole. Today's ledger costs a fixed
2.4 MiB and can describe at most 390 MiB of live pages; Linux spends 64
bytes per page on the same job.

**Free runs live on segregated lists:**

- an exact list for each length from 1 to 64 frames, plus a 64-bit mask of
  which lists are non-empty;
- above 64 frames, two-level segregated fit (TLSF, Masmano, Ripoll, Crespo
  and Real, 2004): power-of-two bins, each split into 8 sub-bins, with a
  bitmap at each level.

The links are frame numbers inside the descriptors, never inside the free
pages themselves. A free page is unmapped (the tripwire), so it cannot hold
anything.

**Allocate k frames** is best fit by class. Take the exact list `k` if it
has a run (the repeating-size recycling, now O(1)). Otherwise the first
non-empty list above `k`, found with one `bsf` on the mask. Otherwise the
TLSF bins: round `k` UP to the next sub-bin boundary, and the head of the
first non-empty list at or above it fits by construction (two `bsf`s, no
walk). Split, set boundary tags, return the remainder to its list, mark the
k frames RUN. Cost: O(1), plus the frames' state writes. The rounding can
skip a run in `k`'s own sub-bin that would have fitted, so before refusing,
the allocator walks that ONE list; that walk exists only on the path that
would otherwise refuse, and it is counted.

**Free a run** reads the descriptor for the address. Anything but a RUN
HEAD is a stray or interior free and panics, naming the address and the run
it landed in: today's exact-base tripwire, with no scan. Then mark it FREE,
coalesce with the neighbour below (its TAIL says where it starts) and the
neighbour above (its HEAD), unlink them from their lists, and push the
merged run. Cost: O(1) plus the frames' state writes. No merge pass, no
dead entries, no compaction, no `allocator_maintain`.

**"Is this page live?"** is one load: RUN (or SLAB in Phase 2), or LEDGER
with a non-zero overlap count (Phase 1 below).
`allocator_copy_from_task_va` keeps its exact shape and safety argument,
and that includes the lock: it holds the allocator's locks across the
whole walk and the copy, as it holds `kMemoryStatusLock` today, so no core
can free a frame between the check and the `memcpy`. The task's own page
tables cannot be what keeps a frame alive; they are the thing a concurrent
burial may free mid-walk (round seven's race). Its five checks become five
loads instead of five table walks.

**Alignment.** Every run starts on a page boundary, so `allocate_memory_aligned`
and `kmalloc_aligned` are just allocations of whole frames. The second
legal free shape (an aligned allocation freed by its rounded-up address
while the extent starts below it) disappears, because nothing is ever
carved from a misaligned start again. Checked (Fable, 2026-10-06): every
aligned caller passes a size only, and none asks for more than page
alignment. A future caller that needs more (a DMA ring wanting 64 KiB
alignment) is adding a feature to the frame table, not finding a bug.

### Phase 1: the ledger keeps the small objects

In Phase 1 the frame table owns all physical memory, and requests route by
shape:

- **page-sized or larger, or page-aligned** (demand-faulted pages, CoW
  copies, stacks, canvases, NIC rings, `kmalloc_aligned`, `kmalloc` of a
  page or more, including the task page-table arenas' buffers): a frame run;
- **smaller and unaligned** (`kmalloc` of under a page): today's ledger,
  unchanged in code and policy (exact-fit-first, merge, compaction), except
  that it no longer owns RAM. When no free extent fits, it takes a chunk
  of frames from the table (a frame run marked LEDGER) and adds it as a free
  extent. In Phase 1 it keeps chunks it has taken; its footprint is its
  high-water mark, reported in the health line.

The ledger's population drops to the kernel's small objects: about 2,900
live rows on the P5 after Yonder exited, where Quinn's early-boot interval
averaged 2,274 rows per search. That is today's normal, not the stall.
**Its 100,000-row wall remains, and that is a known Phase 1 limit, not only
a leak detector:** enough legitimate small objects (many concurrent
connections' kernel buffers, say) can reach it. The ledger's existing
high-water announcement (every 10% crossed) is the capacity evidence that
can trigger Phase 2 on its own.

**The one seam that needs care: liveness inside a ledger chunk.** A frame
in a LEDGER run is allocated as far as the table is concerned, but a free
hole inside the chunk is HHDM-unmapped, exactly as a free ledger extent is
today. Believing "LEDGER means allocated" would let
`allocator_copy_from_task_va` dereference an unmapped page and fault ring
0, and asking the ledger would put its walk back under the frame lock. So
every LEDGER frame's spare descriptor field counts the live ledger extents
overlapping that page, and liveness is "count > 0", one load. The ledger
maintains the counts under the frame lock (its order is ledger -> frames):
it increments after the carve's HHDM map, and decrements before the free's
HHDM unmap. A count above zero therefore means mapped, because a page under
a live extent is never inside a freed extent's fully-contained range. The
cost is one or two counter writes per small-object carve or free.

**HHDM state inside a chunk belongs to the ledger.** The frame table lends
a chunk UNMAPPED, as every free run already is, and does not map it on the
way out. The ledger then maps on carve and unmaps on free inside the chunk
exactly as it does today, partial-page rule included. If a chunk is ever
returned, the frame table unmaps the whole run before freeing it, because
that partial-page rule can leave a page shared by two freed extents
mapped. Either layer mapping what the other owns would double-map on lend,
or unmap a page the other believes is mapped at the first ledger free.

### Layer 2: the object heap (Phase 2, on evidence)

`kmalloc` below a threshold (2 KiB is the proposal) is served from **size
classes**: 16-byte steps to 128 B, then four classes per power of two to
2 KiB. Each class holds slab pages drawn from Layer 1. A slab page's header
sits inside the page (class, free-object list, live count), so the heap
needs no metadata from anywhere else. Its frame descriptor says SLAB, which
is how `kfree` and `free_memory` route an address to the right layer in
O(1).

- **Allocate**: pop the class's free list. When a slab is full, take one
  frame from Layer 1 (lock order heap → frames, never the reverse).
- **Free**: the address must sit on an object boundary of a SLAB frame, or
  it panics (the exact-base tripwire, object edition). A per-slab bit
  catches a double free, which the ledger could only catch when the
  extent had not been reissued. Poison, push, and return an empty slab's
  frame to Layer 1 once a class has a spare.
- **Repeating sizes recycle by construction**: a class IS a pool of one
  size. The procfs churn that bloated the table in August becomes pushes
  and pops on one list.
- At or above the threshold, `kmalloc` takes whole frames from Layer 1:
  the 16 MiB log rings, arenas, big buffers.

**HHDM for small objects is unchanged in strength.** Today an object that
shares a page with live neighbours already keeps its page mapped after
free (the partial-page rule in `paging_hhdm_unmap_range`), so a small
object's use-after-free is caught by poison, not by a fault. That stays
true. A slab page becomes unmapped when its last object goes and its frame
returns to Layer 1, the same moment it would today.

### Locks and contexts

- Layer 1: one irqsave spinlock, held for O(1) list surgery and descriptor
  writes. Zeroing moves OUT of the lock: a freshly carved run already
  belongs to its caller and nobody else can reach it. Today a 16 MiB zero
  runs with every other core's allocations waiting behind it.
- Phase 1: the ledger keeps its own irqsave lock and takes the frame
  table's only to borrow a chunk (lock order ledger -> frames, never the
  reverse), and to update its overlap counts.
  `allocator_copy_from_task_va` holds only the frame lock across its walk
  and copy: every liveness answer, LEDGER frames included, is in the
  table, and no free can unmap a page while the counts still say live.
  Phase 2: one
  irqsave lock per size class, taking the frame table's only to add or
  release a slab frame.
- No scans under the frame table's lock (the ledger's walks stay under its
  own, in Phase 1), so page faults get short, bounded
  holds.
- Per-CPU frame caches (each core keeping a few free frames to skip the
  global lock) are deliberately NOT in this design. They are the next step
  if, after this, measurement shows lock CONTENTION rather than scan cost.
  The P5 counters measure scans.

### Bootstrap

`allocator_init` runs before any allocator exists, reading Limine's memory
map through Limine's full HHDM. That doesn't change:

1. find the highest usable frame and size the table from it;
2. place the table in the first usable region big enough, above the
   reserved bootstrap pages and page 0;
3. mark everything RESERVED, then each usable region FREE as one run (on
   its list), then the table's own frames as an allocator-owned RUN.

From then on the table is never resized, so nothing the allocator does ever
needs the allocator. `init_os64_paging_tables`' retro-map pass walks RUN
heads in the table, and the ledger's live extents inside LEDGER chunks,
since the ledger owns HHDM state there.

### Burial

Freeing 66,000 single pages one call at a time is O(1) per page with this
design, but each free still unmaps its HHDM alias and broadcasts a TLB
shootdown IPI. That is 66,000 IPIs to every core on the way out. A batched
free for burial (collect the task's frames, unmap them, one shootdown,
then release) is in scope for the feature, but as its own slice. It goes in
only if the measured exit cost after the table change says it should.

### Observability

**`/sys/memory/frames` and `/sys/memory/ledger`**, read with `cat`, no
logging needed. `frames`: the books (usable = free + used, with a
`balanced` or `DRIFT` verdict), frames by owner, free runs and the longest,
allocations, refusals and fallback walks, live and free runs by length
(1, 2-4, 5-16, 17-64, 65-256, 257-1024, 1025-4096, 4097+), and the frame
lock. `ledger`: rows against its wall and the high water, entries, chunks
borrowed, live and free bytes, the policy counters, the commonest free
holes, and the ledger lock. Each lock reports acquisitions, how many had to
wait, total wait, and the worst wait and hold twice: since boot, and since
that file was last read, so reading it before and after a workload
measures just the workload. The run histograms are counters kept at every
list insert, removal, allocation and free, and `frames_audit` checks them;
the lock numbers come from the locks timing themselves (two TSC reads per
acquisition, and none for the wait unless the first try fails). Reading the
ledger file walks the ledger under its lock, and that hold is counted too.
The ledger's lock waits and holds, and its rows against the wall, are the
evidence Slice 5 is decided on.

The health line reads the same counters. An **audit walk** (every
descriptor, every list, every boundary tag, the histograms, and
`free + run + ledger == usable` recomputed from scratch) exists for tests
and debug boots, and never runs on a timer: on the 8 GiB guest it holds the
frame lock for about 260 ms, which `/sys/memory/frames` shows as boot's
worst hold.

## What goes away

In Phase 1:

- the ledger's ownership of RAM and its seeding from the memory map;
- `physical_page_is_allocated_on`: no callers.
- Byte-granular **explicit-address** allocation (`allocate_memory_at_address`
  with `use_address`): no callers in the tree, so it is retired (Ruled).
- `shutdown.c`'s ledger dump gains a frame-table summary.

In Phase 2, if it happens: `kMemoryStatus`, `memory_status_t`,
`INITIAL_MEMORY_STATUS_COUNT`, `compact_memory_array`, `merge_freed_block`,
`allocator_maintain` and its kworker call, dead-entry counting and the
free-path compaction backstop.

`tools/stale_refs.sh` runs over every retired name in the commit that
retires it.

## How it gets built

The frame table is data-structure code with no hardware in it, so it is
built and proven on the HOST first, the way `ansi.c`, `psf2.c` and
`tty_reflow.c` were. Slices 1-4 are Phase 1; Slice 5 is Phase 2.

**Slice 1: the frame table as a host-tested library.** `frames.c` and
`frames.h`, pure C with no kernel headers and no external symbols, built
both for the host and with the kernel's own flags. `tools/test_frames_host.sh`
runs it under ASan/UBSan:

- a **differential** run against a deliberately dumb reference model (one
  owner byte per frame, a list of live runs): about 1.6 million random
  allocate, free, pin, unpin and stray-free operations over three tables.
  The two may choose different addresses, so each allocation is judged
  against the model's ownership (inside usable memory, overlapping nothing
  live), while counters, free runs, the largest free run, every frame's kind
  and liveness, and refusals (only when the model has no long-enough free
  stretch) are compared exactly;
- `frames_audit` after every operation on the two smaller tables, and
  periodically on the 4 GiB one: run structure, boundary tags, interior
  frames, pin counts, full coalescing, list membership and back links, the
  masks, and `free + run + ledger == usable`;
- hand cases: coalescing on both sides, exact-length reuse, memory maps with
  holes, adjacent regions and frame 0, the boot-time "already allocated"
  description, and every tripwire refusing with the state unchanged
  (interior, tail, free, reserved and out-of-range frees, a pinned ledger
  run, pins below zero or on non-ledger frames);
- the P5 shape: 150,000 live single frames, freed in random order, back to
  one run;
- the churn bound: under 400,000 operations of repeating sizes, free runs
  never outnumber live runs plus one;
- the TLSF bound: with many separated 70-frame holes and one big run,
  requests for 66 and 100 frames walk no list, and the one-list walk
  happens only when nothing longer exists;
- **mutants** (`tools/test_frames_mutants.py`), as D9's harness does: 22
  copies of `frames.c`, each breaking one rule (a coalesce, a boundary tag,
  exact reuse, the TLSF round-up or fallback, a mask, a link, a counter, a
  tripwire, pin liveness, page zero), and the suite catches all 22.

**Slice 2: the kernel switches.** `allocator.c` keeps its public API and
becomes the routing layer: frame runs for page-shaped requests, the ledger
(on lent chunks) for small ones, liveness answered as described. The HHDM
hooks, zero and poison move as described, along with the retro-map,
snapshot, health line and shutdown summary. CLAUDE.md's allocator section
is rewritten in the same commit. Kernel-level refusals are tested here,
not in Slice 1: `allocate_memory_try` on a full table, a ledger that cannot
borrow a chunk, and zero-on-allocate through the HHDM. Verified with every
built-in QEMU test, `memory_test`'s reconciliation, mallochavoc, and a new
built-in test holding well over 100,000 live frames (600 MiB on the 8 GiB
guest), then releasing them with the books checked.

**Slice 3: burial batching**, if Slice 2's measured exit says so.

**Slice 4: the P5.** The Museum load to completion with `ALLOCATOR_P5_REPORT`'s
successor counters, external commands and htop timed while loading, then
exit and reclamation. Three and six image jobs compared, and Yonder's image
concurrency chosen from that measurement rather than kept at the
diagnostic's three. Then the GitHub reproduction the way it failed:
navigated to from other pages, through style-sheet loading to visible
content, with no wall reached. The same runs' counters
(ledger scan cost and high-water) say whether Phase 2 is needed.

**Slice 5 (Phase 2), only if the ledger's scan cost or capacity says so:** the object heap, host-tested
the same way (`heap.c`, a differential run, mutants), then the ledger
retires.

This is kernel lifetime and concurrency work, so under "match the reviewer
to the risk" it earns an outside review round on Slice 2, beyond Quinn's
and Fable's.

## Alternatives considered

- **Raise `INITIAL_MEMORY_STATUS_COUNT`.** Moves the wall and makes every
  walk longer. The P5 stalls are the walk.
- **Index the flat ledger** (a tree or hash beside the array). Fixes search
  and lookup and leaves the capacity panic, and the index has to survive
  compaction relocating rows. That is the stale-pointer hazard the
  2026-08-15 review already found once in this file.
- **One balanced tree of extents with a growable node pool.** Keeps byte
  granularity everywhere and costs memory in proportion to live
  allocations rather than RAM. It also makes every page fault an O(log n)
  tree operation, needs a node pool that refills from the allocator it
  describes (a reserve watermark, and a recursion argument to get right
  under fault context), and leaves 4 KiB pages with ~64 bytes of node
  each. That's eight times the frame table's cost for the dominant
  population. The frame table is simpler where it matters most.
- **A buddy allocator** for Layer 1. Fine for frames, but it rounds every
  multi-page run up to a power of two (a 3 MiB canvas costs 4 MiB) and
  needs the same descriptors anyway. Segregated exact lists recycle
  repeating sizes more precisely, which is the property the August work
  established.

## Ruled (Chris, 2026-10-06, with Fable's review folded in)

1. **Descriptor size: 8 bytes** per frame, packed as above (44 MiB on the
   P5). The 16-byte layout would cost 88 MiB for simpler bit handling; the
   host suite is what makes the packing safe.
2. **kmalloc rounding: accepted.** Phase 1 rounds allocations of a page or
   more up to whole pages; Phase 2, if it happens, rounds small objects up
   to their size class.
3. **Explicit-address allocation: retired.** It has no callers, and an
   unreachable path cannot be tested.
4. **Per-CPU frame caches: deferred** unless measurement after Slice 2
   shows lock contention.

## Not in this feature

- Recycling the paging-table pool (`get_paging_table_page`): separate, as
  Quinn's handoff says. It stayed at 529 of 12,616 pages.
- Yonder's image concurrency: decided in Slice 4 from measurement, not
  here.
- The browser PRs: unaffected. This is an independent kernel branch off
  `userland`.
