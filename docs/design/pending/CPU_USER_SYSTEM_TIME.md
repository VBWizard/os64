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
  during the rate-limited accounting IPI and the compositor's halt bookends.
  The scheduler's own pass goes to `cls->acctSchedCycles`, outside thread
  runtime. All committed deltas use two TSC readings on the same core.
- `/proc/<pid>/status` sums the task's thread `runCycles`; the thread status
  reports its own. Both render `runtime_us` after `mpAcctSettleAll()`.
  `libos64` parses those files and its monitor computes interval CPU%. `top`
  uses that monitor and can expand multi-threaded tasks with `t`.
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
and interrupt/exception work attributed to the interrupted thread. A kernel
thread has zero user time. The scheduler's dispatch pass retains its separate
per-core bucket. A thread's blocked, sleeping and zombie intervals add to
neither bucket. In particular, a syscall that parks for 100 ms and performs
2 ms of work earns about 2 ms of system time, not 102 ms.

Idle accounting is a special case. The existing idle thread's `runCycles`
represents idle occupancy, including a redirected `sti; hlt` span; it must
not be advertised as 100% system *work*. Add `idleCycles` for this existing
idle-thread attribution. Its user and system counters remain zero while
parked. The full invariant is therefore
`runCycles = userCycles + systemCycles + idleCycles`; for ordinary threads,
`idleCycles` is zero. Expose `idle_us` in status and make `top` render user
and system percentages as unavailable for idle rows. Core busy/idle/scheduler
percentages continue to come from the existing core ledger.

The names on the wire are `user_us`, `system_us`, and `idle_us`, beside the
existing `runtime_us`, in task and thread status. Compute a task's values by
summing the same linked threads used for `runtime_us`. Keep raw cycles in the
kernel and convert each cumulative sum at read time with the fixed boot TSC
rate. Independent integer rounding can leave a few microseconds of difference
between the printed fields; the raw-cycle invariant is authoritative.
These additive text keys preserve readers that ignore unknown fields.
Today's status reads can race a thread running on another core, and separate
task, thread and core files have different observation points. The raw-cycle
partition is exact at each charge; a cross-core text census is still subject
to the coherence limits described in the snapshot plan.

## One charging mechanism

Extend the core-local open-span state with the mode of the current thread:
`user`, `system`, or `idle`. Reinterpret `acctLastDispatchTSC` as the start of
the *uncommitted* open span. Every existing or new charge site calls one
local routine that:

1. Reads the local TSC at its boundary and closes the current open span.
2. Adds that same delta to `runCycles` and exactly one of the three thread
   counters, choosing the existing idle redirection when halted.
3. Restamps `acctLastDispatchTSC` and, if crossing privilege or dispatching,
   sets the mode for the next span.

The routine runs on the owning core, with ordinary maskable interrupts
excluded while it changes the mode and counters. It does not take a global
lock, allocate, convert units or subtract timestamps taken on different
cores. A thread that migrates carries only cumulative cycles; its new open
span begins with a TSC from the destination core. Update the comments on
`acctLastDispatchTSC`, `runCycles`, the scheduler charge, and the proc keys
when implementing this change: several currently say runtime is charged only
at context switches.

| Event | Close/open-span action |
| --- | --- |
| First dispatch | Start a local system span, or an idle span for the idle thread; an eventual ring-3 return changes system to user. |
| Ring 3 to kernel (`syscall`, interrupt, fault) | Close user; open system for the interrupted thread. |
| Nested ring 0 interrupt/exception | Leave system mode in place; avoid double charging. |
| Kernel return to ring 3 (`sysretq`, `iretq`) | Close system; open user for the resumed thread. |
| Scheduler pass entry | Close the outgoing span using its current mode, then start the existing per-core scheduler bucket. |
| Scheduler pass exit | Close the scheduler bucket and start a system span for the incoming thread, or idle for the idle thread; its `iretq` may then open user. |
| Settle-on-read IPI | Close/restamp the open span without changing its mode. |
| Compositor halt begin/end | Close active system work, redirect the halted span to idle, then restore system attribution after wake; a dispatch can clear the halt state. |
| Exit without a user return | The last switch-out closes system time; no synthetic user interval is added. |

The stamps belong at the assembly boundary where the interrupted CS and
return CS are known, not in arbitrary C handlers. A ring-3 entry closes user
time before substantive handler work; a ring-3 return closes system time near
`sysretq` or `iretq`. The short register-save/restore tail between stamp and
privilege transfer has bounded classification error and must preserve the
register, stack, flags and CR3 contracts of each stub. The implementation
should measure that overhead on QEMU and the P5 before calling the result
useful for sub-millisecond syscalls. This is event-level accounting, not a
claim to time individual instructions perfectly.

The scheduler's interrupt prologue must classify a user interruption before
`scheduler_do()` starts its pass. It must not reclassify a thread already in a
kernel syscall. At dispatch, the saved CS in the actual `iretq` frame decides
whether to open a user span. This also covers a blocked syscall resumed on a
different core: the thread resumes in system mode until its own `sysretq`.
Signal delivery paths that edit a syscall or exception return frame are
classified by the actual return privilege, rather than by the handler's
original entry path.

### Entry and return audit before implementation

Inventory every active IDT stub and return path, including the `EXCOLD`
fallback, before changing assembly. The default exception path, PIT/keyboard/
mouse/NIC handlers, AP/IPI handlers, and the scheduler stub each have an
`iretq` that can return to ring 3. The `sysretq` path is separate. A shared
macro/helper is useful only if its use is verified at each live door. Keep
the scheduler's existing re-entry guard and atomic exit window intact.

The NMI probe can arrive while ordinary accounting is mid-update, so it
cannot safely call the ordinary writer. Its time should remain in the
interrupted span in the first implementation, with that limitation stated in
the user-visible documentation. A later NMI-safe bucket is a separate design
if probe overhead becomes material. Fatal exceptions do not resume; their
unclosed final span is irrelevant to a live status read. This limitation
keeps the initial implementation's meaning honest without putting new locks
or partially updated ledgers into an NMI/IST path.

## Presentation and compatibility

Keep the existing public proc and monitor record layouts intact. Add a new
typed split-time accessor in `libos64` for the three cumulative status keys;
the monitor may keep those values in private side arrays keyed to its sampled
tasks and threads. A new monitor accessor returns the current interval split
for a selected PID/TID. This avoids growing caller-allocated proc records or
changing the layout of borrowed monitor snapshots when an older binary loads
a newer library. Parse the split while collecting each status file so `top`
does not double its proc I/O.

The monitor computes user and system deltas against the previous sample with
the same PID/TID and denominator used by total CPU%. `top` adds `USR%` and
`SYS%` columns on task and expanded thread rows. `CPU%` remains total core
share and `TIME` remains cumulative total. The split columns are **interval
percentages of one CPU**, so a task
running on two cores can exceed 100%; the task columns sum its threads. An
unavailable first sample, a sample gap, missing fields or counter reset is
shown as `-`, rather than zero. A partial thread census invalidates affected
thread rows; a task's own status sum remains usable. TID/PID reuse with a
higher new counter can look like continuity in today's monitor; retain its
existing limitation or add a process/thread generation key before promising
reuse detection. Use raw deltas to compute percentages before independent
rounding; one-point display differences between `CPU%` and `USR% + SYS%` are
rounding, not new time.

Do not repurpose `task_t::usage` as a cross-core accumulator. Consider filling
its two `timeval` fields only when a real rusage reader is designed, from the
canonical cycle counters.

## Build sequence and acceptance

1. **Ledger and boundary audit.** Map all live entry/exit paths, generate any
   new assembly offsets, and document the exact point of each stamp. Check
   the ring-3 syscall path with an invalid user stack and the scheduler's
   guarded exit path before touching either.
2. **Kernel split.** Centralize cycle charging and add the three counters.
   Convert `scheduler_do()`, `acct_settle_local()`, and halt redirection to
   the common routine. Add privilege transition stamps to the audited
   assembly paths. Preserve `runCycles` and core ledgers. Review every
   changed comment and run `tools/stale_refs.sh` before a commit that moves
   or renames anything.
3. **Status and consumer.** Emit the new proc fields from one settled read,
   parse them through a new library accessor without changing the existing
   public struct layouts, then add monitor deltas and `top` columns. Keep
   existing total CPU% and TIME behavior.
4. **Validation.** Exercise user-only spin, syscall-heavy copy/spin,
   repeated short syscalls, recoverable page faults, blocking inside a
   syscall, thread migration, kernel worker, compositor halt, and idle
   rows. Check raw-cycle partition equality, monotonicity and no sleeper
   charge across a switch. Run one-core and SMP QEMU boots, then compare a
   P5 `top -t` reading with Yonder's paint diagnostics and compositor row.
   Measure timing overhead with split accounting enabled and disabled.

The first useful reading is Yonder main-thread `USR%`/`SYS%` alongside the
compositor's own `SYS%`. If the main thread is overwhelmingly user time,
paint instrumentation is the next tool; if its system share is high, time the
publish syscall and `kGuiLock` separately. The CPU split narrows that choice
without pretending to profile individual functions.
