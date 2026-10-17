# Per-thread user and system CPU time

*Design, 2026-10-17. Branch: `codex/cpu-user-system-accounting-design`.*

## The question this answers

For a thread that is using a core, how much of its CPU time ran in ring 3 and
how much ran in the kernel? Yonder's publish copy and a spin on `kGuiLock`
run inside its main thread's syscall and should appear in that thread's system
time. Its own paint and script work should appear in user time. The compositor
runs as a separate kernel task, so its composite and flush do not belong in
Yonder's figures. Time spent blocked or halted is not CPU work by the
waiting thread.

The answer is cumulative per thread, summed for a task, and shown as interval
percentages in `top` (including its expanded thread rows). It is a broad
diagnostic: a high system share says where to instrument next; it does not
identify which syscall or kernel function consumed the time. A low share for
Yonder's main thread does not exclude compositor work, another thread, or
time waiting off-CPU.

## Current ledger and boundaries

- `thread_t::runCycles` accumulates TSC cycles. `scheduler_do()` charges the
  outgoing thread at pass entry; `acct_settle_local()` closes the open span
  during an accounting IPI and the compositor's halt bookends. The broadcast
  caller `mpAcctSettleAll()` limits those IPIs to one round per tick.
  The scheduler's own pass goes to `cls->acctSchedCycles`, outside thread
  runtime. All committed deltas use two TSC readings on the same core.
- `/proc/<pid>/status` sums the task's thread `runCycles`; the thread status
  reports its own. Both render `runtime_us` after `mpAcctSettleAll()`.
  `libos64` parses those files and its shared monitor computes interval CPU%.
  Both `top` and `htop` use that monitor; `top` expands multi-threaded tasks
  with `t`.
- `task_t::usage` contains `ru_utime` and `ru_stime`, but neither is populated.
  Their `timeval` shape is not a raw-cycle ledger and should not become the
  single source of truth for this feature.
- `syscall.S` uses `syscall_Enter` and `sysretq`; `scheduler.S` resumes both
  privilege levels with `iretq`. The default exception path is
  `exception_entry.S`, with an `EXCOLD` fallback. Other IDT doors have their
  own assembly stubs: timer, keyboard, mouse, NIC, AP/IPI and NMI. A stamp in
  the C syscall dispatcher alone would miss interrupts, faults and the
  scheduler's direct return to ring 3.
- `mpAcctHaltBegin/End` redirect a compositor `sti; hlt` span to the idle
  thread. That rule must survive the split.

This design is separate from `docs/cpu_accounting_snapshot_plan.md`, which
addresses coherence across many concurrently read task/core files. The split
must work with today's settle-on-read path and remain representable in that
future snapshot. It does not require cross-core TSC normalization.

## Accounting contract

For a non-idle thread, maintain three cumulative raw-cycle values:

```text
runCycles = userCycles + systemCycles
```

`userCycles` counts time while this thread executes ring 3. `systemCycles`
counts its kernel execution, including syscall work, recoverable page faults,
and the scheduler-interrupt prologue attributed to the interrupted thread.
Short device/timer/IPI bodies can remain in the interrupted mode in this
slice, as described below. A kernel thread has zero user time. The scheduler's
dispatch pass retains its separate per-core bucket. A thread's blocked,
sleeping and zombie intervals add to
neither bucket. In particular, a syscall that parks for 100 ms and performs
2 ms of work earns about 2 ms of system time, not 102 ms.

Idle accounting is a special case. The existing idle thread's `runCycles`
represents idle occupancy, including a redirected `sti; hlt` span; it must
not be advertised as 100% system *work*. Add `idleCycles` for this existing
idle-thread attribution. Its user and system counters remain zero while
parked. The full invariant is therefore
`runCycles = userCycles + systemCycles + idleCycles`; for ordinary threads,
`idleCycles` is zero. Emit `idle_us` on idle task and thread status; its
absence means zero on other rows. `top -i` can show the idle rows, with user
and system percentages unavailable (`-`). Core busy/idle/scheduler
percentages continue to come from the existing core ledger.

The names on the wire are `user_us`, `system_us`, and `idle_us`, beside the
existing `runtime_us`, in task and thread status. Compute a task's values by
summing the same linked threads used for `runtime_us`. Keep raw cycles in the
kernel. Convert the raw `runCycles`, `userCycles`, and `idleCycles` sums with
the fixed boot TSC rate; print `system_us = runtime_us - user_us - idle_us`.
This makes the identity exact in text as well as raw cycles. Read a coherent
tuple of each thread's counters before subtracting so a concurrent charge
cannot make the remainder transiently negative or make system time go back.
These additive text keys preserve readers that ignore unknown fields.
Today's status reads can race a thread running on another core, and separate
task, thread and core files have different observation points. The raw-cycle
partition is exact at each charge; a task's sum across several thread tuples
and a cross-core text census are still subject to the coherence limits
described in the snapshot plan.

## One charging mechanism

`acctLastDispatchTSC` becomes the start of the *uncommitted* open span.
Do not store a persistent user/system mode in CLS. A return-tail stamp may
run with IF=1 after a blocked syscall resumes: a timer can preempt between
that stamp and `sysretq`, redispatch the thread into ring 0, and leave a
stored `user` mode charging an arbitrarily long ring-3 run as system. The
class of a thread span instead comes from the interrupted CS at its next
close. The syscall entry is ring 3 by construction; the scheduler and
exception paths use their saved frames. The settle IPI passes its interrupted
CS to the local charge routine. A close invoked directly by kernel C uses
system, unless idle redirection applies. A ring-3 return stamp closes its
current system span and restamps, but does not predeclare the next mode.

The only persistent class is `acctSchedSpanActive`: it marks an open span
owned by `acctSchedCycles`, not by a thread. The local charge routine reads
the local TSC, closes exactly one span, updates the relevant core and thread
counters, then restamps `acctLastDispatchTSC`. Its priority is:

1. If `acctSchedSpanActive`, charge `acctSchedCycles`.
2. If the current thread is idle or `acctCurrentHalted`, charge this core's
   idle thread.
3. Otherwise charge user when the boundary's saved CS is ring 3; charge
   system when it is ring 0.

Every ordinary thread charge adds the same delta to `runCycles`, its user
or system counter, and this core's `acctUserCycles` or `acctSystemCycles`.
The idle charge adds to the idle thread's `runCycles` and `idleCycles`; the
core file continues to take idle time from that thread.
The routine runs on the owning core with maskable interrupts excluded during
each short update. Direct kernel C callers use system only after a stamped
user entry; audit each caller for that precondition. The routine takes no
global lock, allocates nothing, converts no
units and never subtracts timestamps taken on different cores. A migrating
thread carries only cumulative cycles; the destination core starts a new
span with its own TSC. Re-read and update the comments on
`acctLastDispatchTSC`, `runCycles`, the scheduler charge, and proc/sys keys:
several currently say runtime is charged only at context switches.

| Event | Close/open-span action |
| --- | --- |
| First dispatch | Stamp a fresh span; the next boundary's CS classifies it. An idle thread is recognized by its thread flag. |
| Ring 3 syscall or recoverable fault | Close user from the ring-3 entry frame; kernel work begins at the new stamp. |
| Ring 0 exception | Close system unless the scheduler span is active; nested delivery does not add a second span. |
| `sysretq` or `iretq` return to ring 3 | Close the kernel tail as system and restamp near the transfer; do not store a future user mode. |
| Scheduler pass entry | Close the outgoing span using the scheduler interrupt's saved CS; set `acctSchedSpanActive` after restamping. |
| Scheduler pass exit | Close the scheduler span, clear its marker and restamp for the incoming thread. Its ring-3 `iretq` tail gets a system close. |
| Settle-on-read IPI | Close the active scheduler span, or use the IPI's interrupted CS for a thread span; restamp either way. |
| Compositor halt begin/end | Close active system work, redirect the halted span to idle, then restore system attribution after wake; a dispatch can clear the halt state. |
| Exit without a user return | The last switch-out closes system time; no synthetic user interval is added. |

The current settle handler skips a scheduler pass because the outgoing charge
at entry leaves an old `acctLastDispatchTSC` until pass exit. With a scheduler
span this double-billing hazard disappears: a settle that lands during the
body charges scheduler time and restamps. Keep the skip for the short
`mp_inScheduler` prologue/epilogue windows where the scheduler span marker
has not yet been established or has just been cleared; the owning pass will
close those windows. Remove the mid-pass skip only after checking this phase
ordering in the assembly and C paths. The settle handler's present comment
about the old, un-restamped entry must change with the code. The snapshot
plan's phase-3 reasoning about holding a sequence odd across that old
entry-to-exit gap must also be reconciled when this implementation lands.
The local `mpAcctSettleAll()` path and remote IPI handler must follow the
same scheduler-phase rule.

Each short charge transaction publishes a coherent tuple: `runCycles`,
`userCycles`, `systemCycles`, and `idleCycles` for its thread, and the
corresponding core totals, span timestamp and scheduler marker. A per-thread
sequence and per-core `acctSequence` bracket those updates while local
maskable interrupts are off; readers copy, check the sequences and retry a
bounded number of times. The core reader also copies its idle thread's
counter under the same core sequence. A failed coherent copy reports an
unavailable measurement instead of subtracting inconsistent values. The
short core transactions are also a foundation for the broader snapshot
plan; a scheduler pass is no longer one half-updated transaction.

The per-core counters give the machine its own user/system split without
summing migrating threads. Extend `/sys/cpu/<n>/time` with `user_us` and
`system_us`, leaving its existing keys in place. At a committed raw-cycle
boundary the two checkable identities are:

```text
busy = user + system
total = busy + idle + sched
```

Read a coherent core tuple. Convert `total`, `idle`, `sched`, and `user` to
microseconds; derive printed `busy = total - idle - sched` and
`system = busy - user`, after checking the raw identities. That makes the
text identities exact despite integer rounding. The shared monitor parses
the new core fields, and `top` shows a machine `us/sy/id/sched` summary whose
four shares use the sum of sampled core intervals as their denominator.
Existing total CPU% remains based on the same core ledger. A failed identity
or coherent read is an unavailable sample,
not a wrapped percentage.

Stamp the three substantial privilege-boundary doors: `syscall.S`, the
default `exception_entry.S`, and `scheduler.S`. The stamp uses the saved CS
at entry and the actual return CS near `sysretq` or `iretq`; signal delivery
can edit that return frame. Keep the short register-save/restore tail in the
classification error budget. Measure that overhead on QEMU and the P5 before
calling the result useful for sub-millisecond syscalls. This is event-level
accounting, not a claim to time individual instructions perfectly.

The scheduler's saved interrupted CS classifies the outgoing span before
`scheduler_do()` starts its pass. It must not call a ring-0 syscall body
"user" because it belongs to a user task. At dispatch, the actual `iretq`
frame decides whether the return-tail stamp is needed. A blocked syscall
resumed on another core therefore charges its kernel tail as system without
carrying a mode across cores.

### Entry and return audit before implementation

Record the register/stack contract at each of the three stamped doors.
`rdtsc` overwrites both RAX and RDX. At syscall entry they carry the syscall
number and an argument; at return RAX is the result and RDX may become the
status half of a future result pair. Preserve both across the stamp. At the
scheduler exit RAX indexes the AP arrays and RDX has already been restored
from the incoming thread; place the stamp before that restore or preserve
both. The exception path already saves both registers in its common frame.
No stamp may borrow the untrusted user stack or reopen the scheduler's
cli-bracketed exit window.

Inventory the remaining IDT stubs, but leave short timer, keyboard, mouse,
NIC and other IPI bodies unstamped in this slice. Their time remains in the
interrupted span's class; a device interrupt from ring 3 therefore makes a
small amount of kernel work appear as user time. Add a stamp at an individual
stub only if a measurement makes that error material. The opt-in `EXCOLD`
exception path also remains unstamped: split accounting is not maintained in
that diagnostic mode. The default common exception path and its recoverable
faults are covered. Keep the scheduler's existing re-entry guard and atomic
exit window intact.

The NMI probe can arrive while ordinary accounting is mid-update, so it
cannot safely call the ordinary writer. Its time should remain in the
interrupted span in the first implementation, with that limitation stated in
the user-visible documentation alongside the short-ISR and `EXCOLD` limits.
A later NMI-safe bucket is a separate design if probe overhead becomes
material. Fatal exceptions do not resume; their
unclosed final span is irrelevant to a live status read. This limitation
keeps the initial implementation's meaning honest without putting new locks
or partially updated ledgers into an NMI/IST path.

## Presentation and compatibility

Keep the existing public proc and monitor record layouts intact. Extend the
shared `libos64` status and core-time parsers internally so the monitor
collects the split keys in the same file reads it already makes. Keep the
values in private monitor side arrays keyed to its sampled tasks, threads and
cores, and compute their interval deltas in `monitor.c` alongside the existing
total CPU delta. New monitor accessors return the split for a selected
PID/TID or core and the machine summary. This puts collection, validation
and arithmetic in the code shared by `top` and `htop`, without growing
caller-allocated proc records, changing borrowed
snapshot layouts, or doubling proc I/O.

The monitor computes user and system deltas against the previous sample with
the same PID/TID and denominator used by total CPU%. `top` adds `USR%` and
`SYS%` columns on task and expanded thread rows. `CPU%` remains total core
share and `TIME` remains cumulative total. The split columns are **interval
percentages of one CPU**, so a task running on two cores can exceed 100%;
the task row reads the kernel's task status sum from one file, independently
of whether the monitor sampled every thread row. An unavailable first sample,
a sample gap, missing fields or counter reset is
shown as `-`, rather than zero. A partial thread census invalidates affected
thread rows; a task's own status sum remains usable. TID/PID reuse with a
higher new counter can look like continuity in today's monitor; retain its
existing limitation or add a process/thread generation key before promising
reuse detection. Use raw deltas to compute percentages before independent
rounding; one-point display differences between `CPU%` and `USR% + SYS%` are
rounding, not new time.

`top` consumes the new shared-monitor accessors in this slice. `htop` keeps
using the existing total CPU and TIME fields; new columns or controls there
are outside this slice. Build and exercise `htop` against the changed library
to catch a regression, but do not make an `htop` UI change a gate for shipping
the accounting and `top` work.

Do not repurpose `task_t::usage` as a cross-core accumulator. Consider filling
its two `timeval` fields only when a real rusage reader is designed, from the
canonical cycle counters. A dead task's threads stay on its list until the
undertaker buries it, so a future `wait`/`time(1)` consumer must capture the
task's split at child collection, before `retValCollected` permits burial.
Any parent total for collected children belongs at that same door, derived
from the child's canonical thread counters; it needs no parallel live ledger
in this slice.

## Build sequence and acceptance

1. **Ledger and boundary audit.** Map all live entry/exit paths, generate any
   new assembly offsets, and document each stamp's RAX/RDX, stack, flags and
   saved-CS plan. Check the ring-3 syscall path with an invalid user stack,
   the syscall return after a park, and the scheduler's guarded exit path
   before touching either. Measure a pre-change overhead baseline.
2. **Kernel split.** Centralize cycle charging; add thread user/system/idle
   counters and core user/system counters. Convert `scheduler_do()`,
   `acct_settle_local()`, and halt redirection to the common routine with a
   scheduler span marker and short publication sequences. Stamp the three
   audited assembly doors. Preserve `runCycles` and the existing core keys.
   Re-read the comments at each changed site, especially `scheduler_do`'s
   ISR-time claim, `acct_settle_ISR`'s mid-pass skip, `thread.h`'s `runCycles`,
   `smp.h`'s accounting and halt doctrine, `sysfs.c`'s derived-busy rule,
   and `ABI.md`'s syscall-path walk. Reconcile the snapshot plan's old
   scheduler transaction, and run `tools/stale_refs.sh` before a commit that
   moves or renames anything.
3. **Status, shared monitor and `top`.** Emit the new proc fields from one
   settled read and new core-time fields; extend the internal `libos64`
   parsers and monitor collection, validation and delta calculation. Expose
   task, thread and core splits through new monitor accessors without changing
   existing public struct layouts. Add `top` columns and a four-way machine
   summary; keep existing total CPU% and TIME behavior. Check that `htop`
   still builds and displays its existing fields.
4. **Validation.** Exercise user-only spin, syscall-heavy copy/spin,
   repeated short syscalls, recoverable page faults, blocking inside a
   syscall, thread migration, kernel worker, compositor halt, and `top -i`
   idle rows. Run a monopolizing `hog` on a tickless AP under `top -d 5000`
   to catch settle-mid-pass double billing. Run a console-read/`ptyprobe`
   loop long enough for a scheduler tick to hit the resumed syscall tail.
   Assert `user_us + system_us + idle_us == runtime_us` on every coherent
   sample and the two per-core identities above; check monotonic counters and
   no sleeper charge across a switch. Run one-core and SMP QEMU boots, then
   compare a P5 `top -t` reading with Yonder's paint diagnostics and the
   compositor row. Measure post-change overhead against the pre-change
   commit, without adding a second build configuration.

The first useful reading is Yonder main-thread `USR%`/`SYS%` alongside the
compositor's own `SYS%`. If the main thread is overwhelmingly user time,
paint instrumentation is the next tool; if its system share is high, time the
publish syscall and `kGuiLock` separately. The CPU split narrows that choice
without pretending to profile individual functions.
