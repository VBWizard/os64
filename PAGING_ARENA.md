# PAGING_ARENA.md — task page tables move out of the pool

*Chartered 2026-08-11 (Chris + Fable 5), branch `paging-arena` (worktree
`~/src/os64-paging`, base 3cc5256). The main tree keeps working exceptions;
whichever lands first absorbs the other.*

## The problem (the customer is Chris's watch(1))

The paging-page pool (paging.c, `get_paging_table_page`) is a bump allocator
that never frees — sized at boot, panics at exhaustion. Its two customers:

1. **Kernel lazy-HHDM maintenance** — map-once, effectively permanent.
   Monotone consumption is CORRECT here; the sizing formula computes it.
2. **Task page tables** — PML4 (task.c:916) + every PDPT/PD/PT drawn while
   mapping a task's regions (~a dozen pages per task). Never returned at
   task death. `watch` spawns a task per interval → the pool bleeds ~12
   pages per iteration until the exhaustion panic. DEBTS row #3 of the
   task-teardown residuals.

## The ruling (Chris, 2026-08-11)

Task tables leave the pool ENTIRELY. They come from a per-task, kernel-side
arena and die with the task. (This supersedes an earlier pool-freelist +
teardown-walk plan — the arena kills the walk, which was that plan's whole
risk budget.)

Why arena beats pool-recycling:
- **Teardown is wholesale** — `arena_destroy` at phase-2 burial returns every
  table page; no depth-first walk of a possibly-corrupt dying task's tables.
- **Free use-after-free tripwire** — arena pages are kmalloc-backed, kfree
  HHDM-unmaps them, so a dead task's entire map becomes trapped memory: any
  stale walker faults loudly. The pool could never give this (it stays
  eternally mapped).
- **The pool becomes deterministic** — kernel-only customers, exactly what
  the sizing formula already computes. Exhaustion panic stays as a tripwire
  but should never fire again.
- Per-task table accounting falls out free (the arena knows its size).

## Design decisions already made

- **Plain `arena_t`, NOT `task_arena_t`.** task_arena_t is task-VA-mapped
  (it has to_task_ptr/to_kernel_ptr — it exists for things ring 3 may see).
  Page tables are the one thing ring 3 must NEVER see. `task_t` gains
  `arena_t *tableArena` — same ownership, kernel-visible only.
- **Address math:** arena blocks come from kmalloc → HHDM-window pointers →
  `phys = virt - kHHDMOffset`, zeroed at the allocator choke point,
  page-aligned via `arena_alloc_aligned(a, PAGE_SIZE, PAGE_SIZE)`.
- **Source selection via CLS scratch** (the house cikc_ pattern, SMP-safe,
  no signature sweep through paging.c): per-core `pts_tableSource` pointer;
  NULL → pool (kernel maps). Set in exactly TWO places:
  1. `handle_page_fault` — from `cls->task->tableArena` (a fault only ever
     builds the CURRENT task's tables), cleared on every exit path.
  2. `task_create`/elf-load — around building a CHILD's tables (builder runs
     as the parent; the child's arena is the source), set/clear bracketed.
  Draw helper: `pts_tableSource ? arena draw : pool bump`.
- **Teardown:** `task_destroy` phase 2 calls `arena_destroy(task->tableArena)`
  after the final CR3-away is guaranteed (the existing grace period already
  provides this for lockless /proc walkers).
- **Spawn-failure unwind:** a task that dies mid-construction destroys its
  half-filled arena — one call, and strictly better than walking half-built
  tables. (task_create's general unwind is its own DEBTS row; this piece
  slots into it.)

## Build order (each step compiles + boots before the next)

1. **Growable arena** — arena.c: chain a new kmalloc block when a stretch
   can't satisfy an alloc (~30 lines; task table count is unknowable at
   spawn — demand paging draws PTs as the task tours VA space).
2. **CLS table-source + draw-helper fork** — paging.c honors it; nothing
   sets it yet; boot proves zero behavior change.
3. **task_create hookup** — create arena, bracket the child-table build,
   PML4 draw at task.c:916 included. #PF handler bracket for demand draws.
4. **Burial hookup** — arena_destroy in task_destroy phase 2 + the
   mid-construction unwind.
5. **Proof** — QEMU: a watch-style spawn loop (or `TESTRUN`-style fixture)
   for 1000+ iterations; `[pool]` line must go FLAT after boot settles.
   Suite green both roots. THEN the pool sizing formula loses its task
   fudge (separate commit, easy to revert if sizing was hiding something).

## Verification notes

- The `[pool]` minute-line is the instrument: flat slope under task churn =
  victory. Watch it across a long soak, not one iteration.
- Boot itself is the demand-paging regression test (thousands of resolved
  faults); ext2 AND FAT roots per VERIFICATION.md.
- A deliberate spawn-fail (bad ELF path) exercises the unwind.

## Standing cautions

- **The kCPUInfo poisoner** (see qemu_com1.log crashes of 2026-08-11, settle
  tripwire in the main tree) is still at large. This branch reshuffles heap
  layout/lifetimes, which can shift where its scribbles land. That is WHY
  this is a branch: the main tree's victim farm keeps a stable layout. Do
  not merge into a tree mid-stakeout without telling Chris.
- Kernel-side empty-PT reclamation (HHDM unmap path) is EXPLICITLY out of
  scope — its own slice, needs empty-detection + shootdown care.
- The pool's exhaustion panic and the [pool] odometer STAY — tripwires over
  silence.
