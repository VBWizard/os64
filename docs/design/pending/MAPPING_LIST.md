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

## As built (2026-11-26, one slice: the list's half and the page lock)

- **The list** is `kernel/src/memory/maplist.c`, pure: an intrusive list
  linked at the head (a fresh region is found first; a lookup's hit moves
  to the head too, § The walk), each node's holds and `listed` flag, the walk's
  counts and a window since the last read. `tools/test_maplist_host.sh`
  runs it beside a model under ASan with real frees where it says to free;
  `tools/test_maplist_mutants.py` breaks each rule (31, through
  `mutants_parallel.py --with kernel`).
- **`task->mapLock`**, a counted irqsave leaf (`counted_lock.h`), is taken
  by every door in `memory/vma.h` and by nothing else once another thread
  can reach the list. The links live in the mapping, so nothing is
  allocated or freed under it.
- **A mapping's states**, as the design state had them: made, listed,
  delisted and held, freed by the unlink that finds no holder or the last
  release of a delisted one. The fault holds the mapping it resolves in
  (`vma_lookup_hold`); its tail is `page_fault_in_mapping`, which returns a
  kill's reason, so the hold is given back before a kill takes
  `signalLock`. Only a panic does not give it back: a thread is never torn
  down inside the fault path or unmap (a signal redirects only a thread in
  ring 3), so burial should find no hold outstanding; it frees every
  mapping whatever its holds and counts any it finds (`DEBUG_TASK`). The
  lookup is skipped above the user ceiling, where no mapping can be, so a
  ring-0 fault taken under `mapLock` reaches its panic instead of waiting
  on its own lock. `user_range_accessible` copies
  facts (`vma_lookup_facts`); `/proc`'s maps copies under the lock and
  formats after; burial takes the whole list (`vma_take_all`).
- **`task->pageLock`**, counted and irqsave, replaces the global publish
  lock for a task's pages and rests on one task, one address space
  (task.h says so). unmap finds its region with a hold, delists it, then
  walks and removes each page under `signalLock` and the page lock, frees
  the frame after the page lock, and gives its hold back; a second unmap of
  one base that loses the delist is refused. Every install (demand, CoW,
  shared object) rechecks `vma_is_listed` under the page lock and installs
  nothing when the mapping is gone; demand and CoW give their frame back,
  and the shared object's page is the cache's, so it is only not installed. Order: `signalLock`, `pageLock`,
  `mapLock`; the allocator's locks under `pageLock` through table pages.
- **The counts** are in `/proc/<id>/status`: `mappings`, `mapping_walks`
  (lookups, misses, steps, the longest walk), `pages_by_table` (§ The
  walk), `mapping_lock` and `page_lock`
  (taken, waited, the worst wait and hold), each since the task began and
  since the last read.
- **The proof.** The kernel test `vma_list_doors` walks the doors' rules
  on a task of its own (kernel tests here cannot start threads in one
  task). `/tests/maprace` step 3 races the list (map, tag and unmap
  against fresh faults and `/proc` reads); step 4 races unmap against a
  fault in its own block and probes the pages where the fault stopped.
  Step 4 fails on the kernel before the page lock on its first round (a
  page of an unmapped block still reads) and passes after it, at 8 CPUs
  and in the sparse layout; `maprace long` runs a minute.
- **Hand mutants against the fixture**: the installs' recheck taken out is
  caught (round 170 of 300). unmap's page loop outside the page lock is not
  caught in 300 rounds: with the recheck in place it leaves only the few
  instructions between a fault's check and its store. The list lock taken
  out of the fault's lookup is caught by the kernel test only through the
  lock's count, and not by the fixture: a torn walk or a lost hold needs
  timing TCG does not give.
- **Found on the way, booked in DEBTS**: a copy can outlive the check that
  allowed it (an unmap between `user_range_accessible` and the copy is a
  ring-0 fault with no mapping), and thread stacks have no ceiling (the
  per-task stack counter runs into the fixed windows after about 1,400
  threads, since ended threads keep their stacks).

## The walk (2026-11-26, after the P5 run)

Chris's P5 run of the slice above: yonder after a video held 1,304
mappings and walked 233 of them a lookup on average since boot, **1,112 a
lookup since the last read**: nearly the whole list, every time. That is
the cost of a MISS, and the misses came from `user_range_accessible`, which
asked the list about every page of every syscall buffer: a buffer on a
thread's stack, argv or the environment has no mapping, so each page
walked everything, found nothing, and fell back to the page table.

- **The page table first** (`paging_user_page_reachable`): a page ring 3
  could reach right now (present and USER at every level of the walk,
  WRITE too for a copy into it) is vouched for without the list. Only a
  page not yet reachable, a demand page not faulted or a CoW page asked
  for writing, asks the mapping, and a mapping that answers holds for the
  rest of itself, so a `read()` into a fresh megabyte is one lookup, not
  256 (the list asked once; each page still gets the supervisor check
  below). The check it replaced as the fallback looked at the leaf's
  present and write bits only, so a supervisor page in the task's lower
  half, a thread's kernel stack, passed it. Now a page present without
  USER is refused whatever covers it, which matters on this base: a kernel
  stack can lie inside a program image's mapping once its thread counter
  climbs that far (THREAD_STACKS.md). The walk takes no lock: table pages live until the task's
  burial (DEBTS, "A task's page-table pages are never freed before its
  burial", which records that a cure freeing tables must give this walk a
  lifetime rule).
- **Move to front.** A lookup by address that finds its node moves it to
  the head (`maplist.c`, O(1), under `mapLock` like every walk). The hot
  mappings are the old ones (the image, the libraries, the first heap
  arena) and linking new regions at the head had put them behind a
  thousand others. Burial takes all and unmap finds by start; `/proc`
  maps sorts its rows, and past its walk guard (512 rows) shows the
  mappings most recently found or made, where it showed the newest. No pointer is stored, so there is no lifetime question; a
  per-thread hint was weighed and set aside, because the misses were the
  cost and a hint does nothing for a miss.
- **Counts:** `mapping_walks` gains `misses`; `pages_by_table` counts the
  pages the page table answered, each since boot and since the last read,
  so the next P5 run shows steps a lookup before and after.
- **Worst hold.** A hit costs a few steps. A real miss still walks the
  whole list, and is now a wild pointer's lookup (a fault that ends in
  SIGSEGV, or a range a syscall refuses), so the worst hold is unchanged
  in kind and rare in practice: 115 us at 1,542 mappings on the P5.
