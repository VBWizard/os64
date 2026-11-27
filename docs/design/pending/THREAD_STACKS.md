# Thread stacks

A thread's stacks come from its task's one door for address space, take
frames for what is mapped and nothing more, and end with the thread rather
than with the task. The release waits for every CPU to flush its
translations before a frame is freed: the waited invalidation, built here
as a primitive and taken by the release alone.

Ruled by Chris on 2026-11-26 ("Definitely go with your leans"), on the
design state Fable reviewed in four rounds of questions. It follows the
mapping list (MAPPING_LIST.md), whose page lock it uses.

## What was wrong

- **The counter.** Every stack's range was drawn from a bump counter at
  0x10000000 with no lock and no ceiling. Thread creation took no lock, so
  two threads of one task starting threads at once could be handed one range.
- **The ceiling.** app_bases.py links programs anywhere from 0x00400000 to
  0x6f000000, the window the counter climbed through, so most programs'
  stacks reached their own image first (gterm after about 600 threads,
  desktop about 568), and the rest reached the fixed windows at
  0x6f000000 after about 1,350. The next stack was mapped over whatever was
  there.
- **What an ended thread kept.** Both stacks, guard frames included (user
  1 MB + 32 KB, kernel 64 KB + 32 KB), its 3,840-byte thread_t, and its 1 MB
  I/O scratch if it ever did file I/O, until its task was buried.
- **A lost thread.** Two threads making threads at once both spliced the new
  thread after the chain's head with a read and a store; one could be lost,
  never signalled at the task's death and never buried.

## Stacks

- **One door.** Every task with its own PML4 takes a stack's range from
  `task_va_reserve`, the CAS-guarded door its mappings use, with
  `THREAD_STACK_GUARD_PAGE_COUNT` (4) unmapped pages each side. The heap
  range opens in `task_initialize` before the first thread is made.
  `task_stack_reserve` counts what the stacks took (`stackBytes`,
  `stackRanges`).
- **ktask** runs on kKernelPML4, whose lower half is nobody's heap range. It
  keeps a draw of its own, `ktask_reserve_virt`: a CAS from
  `KERNEL_TASK_MEMORY_BASE`, refusing any range that would reach
  `TASK_ARGV_VIRT`, refusing any other task. Its arena tests use it too.
- **Frames for the mapped middle only**: one run of the usable size, so an
  ended thread's guards cost nothing but address space, as a range's do.
  The thread keeps each run's physical address (`esp0BaseP`, `esp3BaseP`),
  so neither the release nor the burial walks a table to find it.
- **Mapped under the task's page lock** (task.h `pageLock`): a sibling
  faulting a page in at the same moment draws table pages from the same
  arena, which is caller-serialized.
- **A refusal leaves nothing.** createThread takes both stacks' frames,
  then both ranges, then maps both, which cannot fail. A refusal before the
  mapping frees the frames taken, none ever mapped, so no CPU can hold a
  translation of them; ranges are never handed out again, by the door's own
  rule. A thread a program asks for may be refused (`mayRefuse`); a task's
  first thread panics on exhaustion, as it always has.
- **The exit stub's write** goes through the HHDM alias of the user stack's
  own run, at RSP's offset into it. No table is walked and no address ring 3
  named is written through, so thread_join_create has no failure after
  createThread, and the thread_t it used to leak on one is gone.
- **The chain's splice is a CAS.** A walker sees the chain before or after
  it, both whole.

## The reap

| State | Entered by | Stacks | Who reads the stack fields, and what they see |
|---|---|---|---|
| made | createThread: frames, ranges, mappings; published by the queue insert | mapped | its maker, until published |
| running | the scheduler | mapped | the thread; the fault's `stack_guard_side` and forensics (current thread only); `/proc` maps rows, which load each address once |
| exiting | `syscall_thread_exit` sets `reapOnExit`, then `exited`, and triggers a pass; the thread runs on its kernel stack until the pass's entry moves RSP to `mp_schedStack` (scheduler.S, before `scheduler_do`) | mapped | `/proc` rows; no signal delivery (`exited`) |
| zombie, handed | **the pass that files it ZOMBIE**, already off the thread's stacks, after `scheduler_store_thread`: `thread_reap_hand_locked` puts it on the reap list under `kReapLock` (a leaf taken under the queue lock), counts it in `task->reaps.pending`, and wakes kworker. `processSignals` wakes kworker too while the list is not empty, for a hand-off that came while kworker was awake | mapped | `/proc` rows |
| reaped | kworker claims every pending thread, up to 256 a pass (PENDING to RUNNING, off the list), clears each `esp0BaseV`/`esp3BaseV`, unmaps all their stacks under `task->pageLock`, calls `tlb_shootdown_wait` ONCE for all of them, frees the frames and the I/O scratch, then marks each DONE. One wait a round, not one a thread: otherwise the acknowledgement's latency would set the reap's rate | gone | `/proc` reads 0 and draws no row (or, mid-release, a row for a stack about to go: text) |
| joined | a join reads the join object, which never touches the thread_t | — | — |
| thread_t's end | `task_destroy`, as before | — | — |

- **A thread that ends with its task is never handed.** Its stacks go at
  burial, as every stack did before: by then every CPU that ran the task has
  loaded another address space.
- **Burial** (`thread_reap_at_burial`) frees the stacks of a thread never
  handed, skips one DONE, and claims and frees one still PENDING (a batch
  limit can leave one). One RUNNING panics by name: kworker is the
  undertaker and the only reaper, and finishes every batch before it buries,
  so burial never waits on a reap and cannot deadlock against one.
- **kworker drains before it buries**, at most 256 threads a pass under one
  wait; a pass that released any counts as work, so the next comes at once.
- **The backlog.** A task with more than `THREAD_REAP_BACKLOG` (16) threads
  handed and not yet released makes no new thread until kworker catches up:
  `syscall_thread` parks a tick at a time, and a signal ends the wait as it
  ends a sleep. **The park is bounded** (`THREAD_REAP_PARK_MOST_US`, half a
  second): a thread's creation never hangs on a daemon, so past it the
  creator proceeds, and `/sys/memory/reap` counts that and the longest park. **This departs from the design state**, which had the
  creator release its own task's entries: a syscall runs with interrupts off
  (SFMASK), and the release waits for every CPU's acknowledgement, which
  must not be waited for with interrupts off (the next section). So the
  creator waits for kworker instead of doing kworker's work.
- **The thread_t stays until burial.** Twenty lockless walkers of
  `task->threads` would each need a lifetime rule to survive a removal; that
  is a slice of its own, booked, for 3,840 bytes a thread.
- The pure core is `reaplist.c` (the list, each node's NONE, PENDING,
  RUNNING, DONE, and its owner's count; every call refuses a node in the
  wrong state, so a node is released at most once whoever reaches it):
  `tools/test_reaplist_host.sh` beside a model, `tools/test_reaplist_mutants.py`.

## The waited invalidation

`tlb_shootdown_wait()` (smp_core.c) numbers a round, sends
`IPI_INVALIDATE_TLB_VECTOR` to every other running CPU, reloads its own CR3,
and returns once every one of them has flushed for that round or a newer
one. The handler (`inv_tlb_ISR`) reads the newest round BEFORE its CR3
reload and records it after, so the reload covers every round up to it:
those rounds' table changes were made before their numbers were. Several
waiters may run rounds at once; a flush for a later round answers an earlier
one too.

- **Every running CPU**, not only those that ran the task: a CPU keeps a
  task's translations after it last ran the task whenever it loads the same
  CR3 again without a reload, and a per-task set of CPUs would have to be
  kept exactly. A CPU MAXCPUS left parked never loaded a task's tables.
- **It never gives up.** A wait past 100 ms is logged once a round under
  DEBUG_EXCEPTIONS and counted; the wait goes on; no frame is freed
  unacknowledged.
- **From the round's number to its own flush the caller stays on its CPU**
  (interrupts off for those few instructions only), so the CPU it skips is
  the one it flushed. Moved while it waits, it runs on a CPU that was sent
  the IPI and answers it at once.
- **Counted** in `/sys/memory/shootdown`, with the longest wait and the CPU
  that answered it last. A syscall runs with interrupts off, so a CPU long in
  one holds every round until it ends or parks; that line is what says
  whether one did.
- **Taken by the reap alone.** `unmap` taking it is its own later slice
  (DEBTS, "A mapping released under a running task is invalidated on the
  calling CPU only").
- The pure core is `shootdown.c`: `tools/test_shootdown_host.sh` steps
  waiters and handlers in random orders over a model of page tables and
  per-CPU translations, and checks that an answered round leaves no CPU
  with a translation older than the waiter's change;
  `tools/test_shootdown_mutants.py` breaks the numbering, the answer and the
  books. The handler's order is the kernel's and the model's, not the
  core's, so no mutant of the core can break it; the model shows it is the
  order that works.

### Where the wait may be taken

| Rule | Why | What proves it |
|---|---|---|
| Never with `signalLock`, `pageLock` or `mapLock` held, nor any other irqsave lock | A CPU spinning for that lock with interrupts off cannot take the IPI the waiter waits for | `tlb_shootdown_wait` panics unless IF is set at entry (`if (!(flags & 0x200)) panic(...)`); every one of those locks is taken irqsave, so IF set proves none is held on this CPU. The reap's one call sits after `unmap_stacks` has released `pageLock` and before `free_stacks` (thread_reap.c, `thread_reap_drain`) |
| Never from a context with interrupts off on a CPU that itself owes an acknowledgement | Two such waiters would wait for each other forever | The same check: the wait cannot begin with interrupts off, and it spins with them on, taking the other waiter's IPI as it comes. No syscall path calls it: a syscall runs with interrupts off (SFMASK), and the creator's backlog waits for kworker instead of releasing anything itself |
| Only in thread context | An interrupt handler runs with interrupts off | The same check; kworker is a kernel thread (RFLAGS 0x202) |

A CPU that spins with interrupts off for something only kworker can give
would still deadlock against kworker's wait; none is known, and the 100 ms
line is what would name one.

## Counts

- `/sys/memory/reap`: pending now, the most, the most since the last read;
  handed; released by kworker and by burial; kworker's rounds (one
  invalidation each); stack bytes and scratch blocks given back; ticks
  thread makers parked for the backlog, how many proceeded past the bound,
  and the longest park; nodes refused for a wrong state.
- `/sys/memory/shootdown`: rounds, the late ones, the longest wait and the
  CPU that answered it last, each since boot and since the last read.
- `/proc/<id>/status`: `threads <made> made, <live> live` (the first number
  is what the line always said), `reaps_pending`, and `stacks <bytes> in <n>
  ranges` beside `heap`, whose range now holds the stacks too.

## The proof

- `/tests/stackchurn` (testrun): 2,000 threads made and joined in turn,
  then 2,000 more from four makers at once, every thread filling a quarter
  megabyte of its stack with a tag of its own and checking it after a yield,
  live threads checking their stacks do not overlap; then argv, PATH and a
  caught SIGINT read back through the fixed windows; then one live thread and
  the free bytes back, less what ended threads keep until burial.
  `stackchurn long` makes four times as many.
- `maprace`, `threadtest` and testrun beside it; `make run-sparse`.

## Booked

- **Page-table pages are never freed** before burial: a long-lived task that
  maps and unmaps at addresses never reused keeps about 1/512 of every byte
  of address space it ever used. The stacks add about 2.4 KB a thread to it.
  DEBTS has the row and the arithmetic.
- **The thread_t until burial**, 3,840 bytes a thread (above).
- **unmap's invalidation** stays on the calling CPU until its own slice.

## As built

(Filled in at the end of the slice.)
