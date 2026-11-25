# A task's mapping list: a lock, and a lifetime for a mapping

Fable's design note, 2026-11-25, for a kernel slice of its own. The defect
was found while the one door for a task's address range was built
(VIDEO_PLANE.md § 3.2); that change does not fix it. DEBTS (Kernel
robustness, "A task's list of mappings has no lock") is where its state is
kept.

**Ruled by Chris, 2026-11-25:** a high-priority debt. It is taken up when
the video plane's performance is where it should be, and sooner if a panic
appears that it could explain.

Kernel lifetime and concurrency work: design state before code, an outside
review before merge.

## What is wrong

`task->mmaps`, the list of a task's mappings (`vma_t`), has no lock, and
`dlist` has none inside.

- `vma_add` (the `map` syscall), `dlist_remove` then `vma_destroy` (`unmap`),
  `vma_lookup` (the page-fault handler, and the user range check in the
  syscall path), procfs's maps walk and burial all touch it bare.
- ring 3's allocator holds its own lock across the `map` and `unmap` it
  makes, so those two rarely meet. A page fault takes no such lock: a thread
  touching fresh memory walks the list while a sibling's `free` delists a
  mapping and frees it.
- The handler goes on using the pointer `vma_lookup` gave it, through demand
  paging, after the walk.
- The cost when it happens: the walk or the handler reads a freed mapping,
  which is poisoned and gone from the kernel's direct map, so the kernel
  panics ("use-after-free or wild pointer"); or two adds lose one of two
  links. Any program with threads that map can reach it.

## The rules to build

1. **One lock for the list**, the task's own, interrupts off, held for a few
   instructions. Every read and every write of the list is under it: add,
   delist, lookup, the user range check, procfs's walk, burial. Nothing is
   allocated, freed, formatted or slept on while it is held; it is a leaf,
   because a fault path takes it and the allocator's locks are taken on
   fault paths too.
2. **A mapping handed out of the lock is held.** A lookup whose caller will
   use the mapping after the lock returns it with a hold, taken under the
   lock, and the caller gives it back. A caller that needs only facts
   (protection, flags, bounds) gets a copy made under the lock.
3. **A mapping is freed when it is off the list and nothing holds it.**
   `unmap` delists under the lock; the free is the last release's, outside
   the lock.
4. **`unmap` delists first**, so no fault can begin on the mapping after
   that, then releases its pages as it does now (each under `signalLock`,
   the rule that keeps signal delivery from writing a page as it is freed).
5. **A fault that was under way when its mapping was delisted leaves nothing
   behind**: no page mapped in a range whose mapping is gone, and no frame
   lost. The builder designs it (a recheck of "still listed" at the install,
   under the list's lock or under whatever serializes the task's page-table
   writes) and states what does serialize two threads' page-table writes for
   one task. If nothing does, that is a second finding, to be raised before
   anything is built on it.
6. **procfs** copies what it prints out of the list under the lock, into its
   bounded buffer, and formats after.

## Design state before code

A table: a mapping's states (listed; delisted and held; freed), who may
read and write in each and under what, what a fault racing each transition
sees, and the lock order with `signalLock`, `handleLock` and the allocator's
two locks.

## The proof

- The list with its holds as a pure core if it cuts cleanly (no kernel
  headers), beside a model, with mutants through `tools/mutants_parallel.py`:
  a free with a hold outstanding, a lookup with no hold, a delist that
  leaves the entry findable, a release that frees twice.
- A kernel test: threads on one task adding, delisting and looking up with
  holds.
- A ring-3 fixture: threads that map, touch and unmap large blocks while
  others fault fresh pages in, every page keeping its tag, for long enough
  to matter; in QEMU at 8 CPUs and under `make run-sparse`.
  `/tests/maprace` is the starting point.
- A hand mutant with the lock taken out of the fault path fails the fixture,
  or the change says plainly that it could not be made to.
- The DEBTS row is paid.

## Not in this slice

The invalidation the kernel waits for on other CPUs at `unmap` (its own
DEBTS row). Planes. Any change to what a mapping is.
