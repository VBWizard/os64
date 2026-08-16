# os64 Memory Subsystem

*The design record for physical allocation, the kernel heap, paging, and the
HHDM contract. Written so a future contributor (human or model) can extend
this without re-deriving any decision. The single most important thing in
this file is the HHDM contract — read that section twice; almost every
memory scar in this kernel traces back to some code that didn't respect it
(or, before July 2026, to the fact that it didn't exist yet).*

## What this is

Four cooperating layers:

| Layer | Files | Job |
|---|---|---|
| Physical allocator | `memory/allocator.c` | The extent ledger (`kMemoryStatus`): who owns every byte of RAM. Single alloc/free choke points; owns the allocator spinlock and the HHDM map/unmap calls |
| Kernel heap | `memory/kmalloc.c` | Thin veneers over the allocator: `kmalloc`, `kmalloc_aligned`, `kmalloc_dma`, `kfree` |
| Paging | `memory/paging.c`, `include/memory/paging.h` | 4-level tables, the page-table page pool, map/unmap/walk, HHDM maintenance, `init_os64_paging_tables` |
| Task memory | `memory/vma.c`, `memory/arena.c`, `memory/task_arena.c`, `elf_loader.c` | VMAs (demand paging, file-backed regions), grouped-lifetime arenas |

Boot intake: `memory/memmap.c` parses Limine's memory map; `allocator_init`
seeds the ledger from the USABLE entries.

## The HHDM contract (THE invariant of this kernel)

**Physical memory is HHDM-mapped in the kernel page tables exactly while the
allocator considers it allocated.**

- Any allocator-owned extent — `kmalloc`, `kmalloc_aligned`, AND
  `allocate_memory_aligned` (task memory) — is dereferenceable at
  `phys | kHHDMOffset`, on every machine and every memory map. No more
  "works on QEMU because the layout happens to line up."
- Freed or never-allocated RAM is **deliberately unmapped**: touching it
  through the HHDM page-faults into a "use-after-free or wild pointer?"
  panic. This is a designed tripwire (DEBUG_PAGEALLOC in spirit), chosen
  over an eager Linux-style full direct map. When it fires, it is doing its
  job — find the stale pointer, don't weaken the trap.
- **Mechanics:** the allocator's single choke points call
  `paging_hhdm_map_range()` on allocate and `paging_hhdm_unmap_range()` on
  free. Nothing else may call these two functions.
- **Boundary-page asymmetry (read carefully):** mapping rounds OUTWARD
  (every page the extent overlaps gets mapped — a caller handed bytes
  anywhere in a page must be able to touch that whole page's alias);
  unmapping rounds INWARD (only pages fully contained in the extent — a
  partial boundary page may host live 8-byte-granularity neighbours and
  stays mapped). Mapping is idempotent because HHDM virt↔phys is a fixed
  1:1 relation.
- **TLB:** unmap `invlpg`s locally and broadcasts a shootdown IPI
  (`mpSendInvTLB`, a no-op until `kSMPInitDone` — never IPI a parked core).
  Fire-and-forget is safe *specifically because* the 1:1 relation means a
  stale entry can never produce a wrong translation — it can only let a
  core miss the tripwire for a beat.
- **Early boot:** `kHHDMMaintenanceEnabled` is false until
  `init_os64_paging_tables()` builds the real kernel tables and switches
  CR3 (before that, Limine's own full-HHDM tables are live). Allocations
  made earlier are retro-mapped during the table build.
- **Conversions:** `phys | kHHDMOffset` (valid iff currently allocated);
  `hhdm - kHHDMOffset` for the reverse. MMIO and reserved regions are NOT
  allocator-owned and need explicit `paging_map_pages` calls.

**Zero-on-alloc rides the same choke point:** every allocation is memset to
0 through its freshly mapped HHDM alias — one place, covering kmalloc,
task memory, and demand-paged anonymous/BSS/heap pages, so no allocation
ever leaks a previous owner's data. (Exception: `kmalloc_dma` re-zeroes
through its uncached mapping for device coherency.) Poison-on-free exists
as a commented-out one-liner in `free_memory`; it's off because it would
also poison pages still live in task address spaces — enable deliberately
or not at all.

## The physical allocator

`kMemoryStatus` is a flat ledger of extents (`startAddress`, `length`,
`in_use`), first-fit scanned. Plain requests are internally rounded to 8
bytes; page-aligned requests carve from an unaligned block by recording the
*unaligned* start in the ledger while returning the aligned address — so a
`free_memory(aligned_ptr)` still finds its entry by containment, not
equality. Frees mark the extent, drop its HHDM interior pages, then merge
with adjacent free extents and (every 10th free) compact the ledger — all
under the lock; **any saved index or entry pointer is invalid afterwards.**

**Locking:** every entry point takes `kMemoryStatusLock`, an
interrupts-disabled (irqsave) spinlock. This is not optional politeness:
allocations happen in page-fault context on multiple cores concurrently
(CoW privatization, demand paging, shared-object resolution all allocate
while handling a fault). Two rules follow:

1. **Never call allocate/free while holding another spinlock that a fault
   path might also take** — the fault-context allocator spinner has IF=0
   and will never be preempted off the lock it's waiting for.
2. Keep fault handlers' allocation needs simple; they inherit the whole
   rule set above.

Page 0 is never handed out (the ledger starts above it — and see the
open TODO to unmap VA 0 entirely so kernel NULL derefs stop silently
succeeding through the identity window).

## The kmalloc family — which one, when

| Call | Returns | Mapping | Use for |
|---|---|---|---|
| `kmalloc(n)` / `kmalloc_aligned(n)` | HHDM VA | HHDM (write-back) | Kernel structures that live at kernel discretion. `kfree` takes either the HHDM VA or the phys |
| `allocate_memory_aligned(n)` | **physical** address | HHDM while allocated | Task-owned memory (stacks, heap pages, GUI canvases): mapped into the task's PML4 by the caller; kernel can still reach it via the alias |
| `kmalloc_dma(n)` | physical (identity) | identity + PCD (uncached) | Device buffers where hardware sees physical addresses (AHCI) |
| `kmalloc_dma32_address(a,n)` | the given phys | identity + PCD | Hardware that demands a specific address |
| `arena_create` / `task_arena_create` | arena handle | via kmalloc | Many small allocations with ONE lifetime — free the arena, not the pieces |

`kmalloc(0)` panics on purpose. All kmalloc-family memory is zeroed by the
allocator choke point (above) — don't add per-caller memsets.

## Paging

- 4-level tables; kernel at 0xffffffff80000000, HHDM at `kHHDMOffset`,
  task heap at 0x70000000 (see CLAUDE.md's address-space layout).
- **The page-table page pool** backs every table allocation
  (`get_paging_table_page`): a bump allocator that never frees, sized in
  `init_os64_paging_tables` at one pool page per 16MB of physical RAM,
  PLUS explicit funding for the ramdisk module's retro-map (one page table
  per 2MB of module — a 512MB module is 256 pool pages, half the base pool
  on an 8GB machine). Exhaustion panics loudly; before July 2026 it walked
  silently into allocator-owned pages — two owners, one page, corruption
  with no fingerprints. If the panic fires, grow the funding formula; do
  not bypass it.
- `paging_map_pages` self-heals unaligned inputs (masks the addresses,
  bumps the count for a straddling VA) — but don't lean on that; pass
  page-aligned values (CLAUDE.md pitfall #2).
- `paging_walk_paging_table(pml4v, va)` resolves a VA through any task's
  tables; returns the physical address or the `0xbadbadba` sentinel /
  0 — **check for both** before using the result.
- `init_os64_paging_tables()` builds the real kernel tables (Limine's are
  live until then), retro-maps the boot modules at their HHDM VAs (console
  font, PCI IDs, ramdisk — same recipe each time), retro-maps early
  allocations, switches CR3, and flips `kHHDMMaintenanceEnabled`.
- After editing live tables: `RELOAD_CR3` (or `invlpg` for single pages);
  cross-core visibility needs the shootdown IPI. The HHDM paths do this
  for you; hand-rolled mappings do it themselves (CLAUDE.md pitfall #4).

## VMAs and demand paging

A task's regions (ELF segments, heap, file-backed mappings) are `vma_t`s:
`vma_create` → `vma_add(task, vma)`; the page-fault handler uses
`vma_lookup(task, addr)` and `vma_resolve_backing_page()` to materialize
the page — anonymous pages come pre-zeroed from the allocator choke point;
file-backed pages are read through the VFS *from fault context* (this is
the code whose kernel-context trampoline and CLS scratch-variable
discipline are documented in CLAUDE.md — `cikc_*`; read that chapter
before touching it). Demand paging is for *task* pages only; kernel-side
consumers of task memory must respect the eager-backing rules their
subsystem defines (e.g. GUI canvases are NEVER demand-paged — see
GRAPHICS.md).

## Recipes

- **Kernel writes to task memory:** resolve through the TASK's tables,
  then write the HHDM alias — `phys = paging_walk_paging_table(task->pml4v,
  task_va)`; check for 0/0xbadbadba; `*(uint64_t*)(phys | kHHDMOffset) =
  value`. No temporary mappings, no TLB dance. NEVER resurrect the old
  scratch-VA technique, and never map anything at 0xffffffff80000000 — 
  that's the kernel link base; mapping there clobbers kernel text.
- **New permanent kernel structure:** `kmalloc`. **New task memory:**
  `allocate_memory_aligned` + `paging_map_pages(task->pml4v, ...,
  PAGE_USER)`. **Device buffer:** `kmalloc_dma`.
- **MMIO:** explicit `paging_map_pages` with `PAGE_PCD`; MMIO is never
  allocator-owned and never HHDM-maintained.
- **Never touch a C local across a CR3/RSP switch** — full recipe and the
  -O0 trap that makes it *look* safe are in CLAUDE.md's context-switching
  chapter; CLS (`gs:0`) is the only safe cross-switch storage.

## Failure fingerprints (symptom → cause)

- **A multithreaded program dies with a segfault at an absurd address
  (`0xffffffffffffff8a`), and the fault reporter's "bytes at RIP" DO NOT
  MATCH what `objdump` shows at that address:** the demand pager filled a
  page from the wrong file offset. `seek(file, offset)` then `read(file,
  ...)` is only atomic if nobody re-seeks in between, and a `vfs_file_t`
  holds ONE position — so two threads of one task faulting on two
  different pages of their own executable each received the other's
  offset, and a code page came back full of real, valid machine code from
  elsewhere in the binary. Execution then wandered off and died with no
  relationship to the actual bug. **Fixed 2026-08-15** by `pos_lock` in
  `vfs_file_t`, taken around the seek+read pair in `kernel_read_file`
  (vma.c) — the same lock `shared_object_t::io_lock` already held for the
  dynamic-linking path, one layer down. Diagnostic that isolates it in one
  run: pre-fault the text on the main thread before starting the threads
  (`WARMUP=1 malloctest threads 8`) — if the crashes vanish, it is the
  pager, not the workload. **The general rule this leaves behind: a file
  position is per-handle state, and any seek+read pair on a handle more
  than one thread can reach must be atomic.**
- **Page fault panic "use-after-free or wild pointer?" on an HHDM
  address:** exactly what it says — something touched freed or
  never-allocated RAM through the alias. Recent frees of that range are
  suspect #1; teardown-order bugs (object unlinked after its pages were
  freed — see GRAPHICS.md's "windows die before pages") are suspect #2.
- **Early-boot hard hang, no output:** the allocator's out-of-memory path
  is currently `cli; hlt` — a *silent* stop (known limitation below). If
  boot dies mute right after heavy allocation, suspect exhaustion or a
  fragmentation-induced "no fitting block."
- **`get_paging_table_page: paging page pool exhausted` panic:** the pool
  funding formula didn't anticipate a new large mapping (this is how the
  512MB ramdisk announced itself). Fund it in `init_os64_paging_tables`.
- **One core spins forever inside the allocator:** lock-ordering violation
  — an allocator call made while holding a lock that a fault path takes,
  or vice versa (the irqsave rationale in allocator.c).
- **`0xbadbadba` used as an address (#PF on it):** an unchecked
  `paging_walk_paging_table` result.
- **Corruption whose location shifts with memory layout / RAM size, no
  consistent fingerprint:** the historic double-owned-page signature. The
  pool tripwire closed the known instance; if it recurs, hunt for any
  other bump allocator or hand-carved physical range overlapping ledger
  memory.

## Known limitations / future work

1. Allocator exhaustion is a silent `cli; hlt` — should be a panic naming
   the requested size and largest free extent. (Both no-fit branches.)
2. First-fit with linear scans everywhere; merge scans the whole ledger
   per free, compaction every 10th free. Fine at current scale; measure
   before caring.
3. Debug residue to delete when next in there: the `requested_length >=
   200000000` breakpoint stub in the allocator and its twin in
   `kmalloc_dma`.
4. `free_memory`'s return value is only an error signal — post-merge/
   compaction it must not be used as an index (documented at both ends).
5. Unmap VA 0 / audit the low identity window so kernel NULL derefs trap
   instead of silently succeeding (open TODO; mind the AP trampoline).
