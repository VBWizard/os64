# Coherent CPU Accounting Snapshot Plan

## Status

This document describes the planned replacement for os64's current
settle-on-read CPU accounting path. It is a design record, not a claim that
the whole feature is already implemented.

Where it stands:

- Phase 1 is in the tree: there is no continuous TSC recalibration, and the
  boot calibration's measured cycles-per-second value is fixed for the life
  of the boot (the `kTSCCalibrationSeconds` comment in kernel.c carries the
  argument and the default window; TSCCAL= on the cmdline changes it).
- Before it landed, QEMU boot tests passed and a multi-hour bare-metal `top`
  run showed no accumulating rate drift. The familiar 99%/101% idle-task
  readings remained stationary instead of growing with uptime.
- That result closes the first question: changing the cycles-to-microseconds
  exchange rate during a boot is unnecessary for CPU percentages and is
  actively undesirable for monotonic cumulative counters.
- The current accounting settlement IPIs are still enabled. They must remain
  until a replacement has been implemented, compared against the old path,
  and proven on machines whose TSCs cannot be safely normalized.

## Executive summary

The 99% and 101% values are not evidence that an idle core performed more or
less than one second of work in a one-second interval. The per-core ledger is
usually exact. The visible error comes from asking for related numbers through
many independent files at different instants and then treating them as if they
were one observation.

`top` currently reads task status files and per-core time files sequentially.
Those reads can settle accounting at different boundaries. A task runtime may
therefore cover 1.003 seconds while the core-0 denominator covers 0.993
seconds, producing 101%; the next refresh can reverse the mismatch and produce
99%. Over time the cumulative books remain correct, but each displayed delta
can combine endpoints that never belonged together.

The proposed fix is a focused, immutable accounting snapshot:

1. Establish a validated common TSC timeline for all cores at boot.
2. Publish each core's accounting state under a small sequence counter.
3. Capture task, thread, and core data against one normalized timestamp.
4. Virtually close each core's open accounting span in the private snapshot;
   do not modify the live counters and do not wake the core.
5. Render the already-captured data through one versioned proc file.
6. Run the new and old readers side by side before switching `top`.
7. Remove continuous settle-on-read IPIs only after the new path proves it can
   replace them, retaining an explicit fallback for an untrustworthy TSC.

This is deliberately not `/proc/all`. A file claiming to contain everything
would become an accidental ABI for unrelated kernel state. The useful unit is
"everything needed for one CPU-accounting observation," proposed below as
`/proc/accounting`.

## The current model

os64 accounts CPU time primarily at scheduler boundaries:

- Each thread owns `runCycles`, a cumulative count in raw TSC cycles.
- Each core owns `acctZeroTSC`, `acctLastDispatchTSC`, scheduler-cycle totals,
  its current-thread pointer, and the halted/idle redirection state.
- At scheduler entry, the outgoing open span is charged using two TSC reads
  taken on that same core.
- A thread which executes `sti; hlt` outside the idle task is represented by
  `acctCurrentHalted`; its halted span is charged to that core's idle thread.
- `/proc/<pid>/status` and `/sys/cpu/<n>/time` convert the raw cumulative
  cycles through the boot-measured `kCPUCyclesPerSecond` value.

Same-core subtraction is an important existing invariant. It means committed
runtime is not corrupted by a fixed offset between the TSC values reported on
two processors.

The missing piece is freshness. Under the tickless scheduler, an idle or
monopolized AP may go a long time without a scheduling boundary. Its open span
has happened physically but has not yet been transferred into `runCycles`.
The current solution is `mpAcctSettleAll()`:

- A proc/sys read broadcasts `IPI_ACCT_SETTLE_VECTOR`.
- Each target core reads its own TSC, charges its open span, restamps the
  dispatch boundary, and acknowledges the request.
- The broadcast is rate-limited to once per kernel tick so a `top` refresh
  opening many proc files does not send one broadcast per file.

This makes the ledgers fresh, but it has three costs:

1. Reading statistics changes the statistics by interrupting every target.
2. Truly idle cores cannot remain halted while a monitor is running.
3. Rate limiting plus sequential files still does not create one common
   endpoint. Some files observe one settlement and others observe another.

The IPIs corrected stale, lumpy accounting. They did not and cannot make a
collection of independent file reads atomic.

## Why the TSC rate is fixed after boot

The boot calibration produces a cycles-to-time scale. If that scale is
slightly high or low, both a ledger numerator and a ledger-derived denominator
are multiplied by the same constant. The scale error cancels in the CPU
percentage:

```text
(task_cycles / rate) / (core_cycles / rate) = task_cycles / core_cycles
```

Absolute displayed CPU time retains the small calibration error, but the
percentage does not. A longer `TSCCAL=` window remains available when tighter
absolute time units justify a longer boot.

Changing the rate later is worse than leaving a small fixed error. Every
historical cycle count is converted using the new divisor, so cumulative
microseconds can move backward even though raw cycles only increased. That
breaks monotonicity and forces userland to defend against a counter being
"re-priced" once per minute.

The multi-hour observation after disabling recalibration is consistent with
this model: there was no accumulating drift, while the small alternating
99%/101% endpoint error remained.

## Design goals

The completed feature should provide these properties:

- One logical capture time for cores, tasks, and threads in a refresh.
- Cumulative runtime remains monotonic.
- Merely reading accounting does not mutate live runtime counters.
- A monitor does not wake a core whose architectural state permits it to stay
  halted.
- No cross-core TSC subtraction occurs unless the relationship between those
  TSCs has been measured and accepted.
- Failure to validate a core's clock degrades explicitly to the old safe path;
  it never silently adopts a guessed offset.
- Locks and sequence counters protect capture, not text formatting. Allocation
  and formatting happen outside scheduler/task locks.
- Task exit, thread migration, halt redirection, and scheduler accounting are
  all represented without use-after-free or double charging.
- The proc-facing code remains a small router/renderer. Accounting policy and
  capture machinery live outside `procfs.c`.

## Non-goals

- This is not a general `/proc/all` facility.
- It does not replace the scheduler or change scheduling policy.
- It does not make wall-clock time depend on the TSC.
- It does not promise that arbitrary machines have synchronized TSCs. It
  measures what is needed and preserves a fallback.
- It does not initially require a binary syscall ABI. A versioned text file is
  sufficient to prove the semantics and is easy to inspect from husk.
- It does not remove the existing settlement path in the same change that
  introduces the replacement.

## Phase 1: fixed boot-time conversion rate — complete

The continuous `tsc_recalibrate()` scheduler call and implementation are
removed. `tscGetCyclesPerSecond()` runs once, at boot.

The backward-counter guards in `top` remain as cheap defensive checks. They
can be reconsidered after the new snapshot has survived counter-reset and PID/TID
reuse tests.

Acceptance evidence already collected:

- Kernel and userland built cleanly.
- An eight-core Q35 boot passed both test phases.
- The system ran beyond the old one-minute recalibration boundary without a
  recalibration event.
- A multi-hour observation did not reveal accumulating TSC-rate drift.
- The 99%/101% symptom remained, as expected for a torn observation.

## Phase 2: a validated common TSC timeline

### Why it is needed

Committed spans already use same-core subtraction and need no offset. A
read-only snapshot is different: a reader standing on core A must be able to
say how far core B's open span had advanced at the snapshot timestamp without
asking core B to execute an interrupt handler.

That requires mapping a local TSC value from each core onto one common
timeline. The BSP's TSC is the proposed reference:

```text
normalized_tsc = local_tsc + signed_offset_to_bsp
```

The offset affects only virtual, snapshot-private open spans. Existing
committed `runCycles` continues to be charged by same-core subtraction.

### Proposed boot handshake

APs are already brought up one at a time and wait in `sti; hlt` before their
first scheduled task. Immediately after an AP finishes initialization, but
before enabling scheduling on it, the BSP will perform a boot-only clock sync:

1. Read an ordered BSP TSC value (`lfence; rdtsc`).
2. Send a dedicated clock-sync IPI to the AP.
3. The AP handler immediately records its ordered local TSC and acknowledges.
4. Read a second ordered BSP TSC value.
5. Treat the BSP midpoint as the best estimate corresponding to the AP sample.
6. Repeat 32 times and retain the sample with the smallest round-trip time.

For a sample with BSP endpoints `b0` and `b1` and AP value `a`:

```text
round_trip  = b1 - b0
bsp_midpoint = b0 + round_trip / 2
offset       = bsp_midpoint - a
uncertainty <= round_trip / 2
```

Choosing the minimum round trip rejects samples inflated by a host scheduling
pause, a timer interrupt on the BSP, or general virtualization noise. The
initial implementation should use a proposed maximum uncertainty of 250 µs.
That threshold is intentionally a named constant and should be tuned from
QEMU, VirtualBox, and bare-metal measurements rather than folklore.

The dedicated vector should share the AP-initialization vector's LAPIC
priority class. If the initialization handler has set `coreInitialized` but
has not yet issued its EOI, the hardware will hold the sync request pending
instead of nesting a higher-priority interrupt into unfinished bring-up.

These IPIs are not the continuous IPIs under discussion: they occur only
during boot, before the core enters service. Their purpose is to make it
possible to remove read-time IPIs later.

### Stored clock state

Each core will publish, in core-local storage:

- A signed TSC offset to the BSP timeline.
- The measured uncertainty of the selected sample.
- A validity flag.
- Small request/acknowledgement scratch fields owned by the boot sync.

The module should also record whether CPUID reports an invariant TSC. That bit
is useful diagnostic evidence, but measurement remains authoritative. A
hypervisor can expose imperfect feature metadata, and invariant rate alone
does not prove a zero cross-core offset.

Proposed public operations:

```text
accounting_clock_normalize(apic_id, local_tsc, &normalized)
accounting_clock_now(&normalized_now)
accounting_clock_core_valid(apic_id)
```

All return failure for an invalid core. Callers must not substitute offset
zero on failure.

### Failure and fallback

A timeout, excessive uncertainty, impossible arithmetic, unsupported core ID,
or later validation failure marks that core invalid. It is not a boot failure.

During the comparison phase, any invalid core keeps the old settlement path
authoritative. The first production version may conservatively fall back to
the old whole-machine settle if even one active core is invalid. A later
optimization can target only invalid cores, but correctness should precede
that refinement.

Suspend/resume and CPU hotplug are not currently os64 facilities. When they
arrive, they must invalidate and repeat this handshake because a boot-time
offset cannot be assumed to survive either event.

## Phase 3: publish accounting state with per-core sequence counters

The snapshot reader needs to copy a core's state while that core may be
charging a span. A lock taken by the reader would either require cross-core
coordination or make interrupt paths wait on a monitor. A per-core sequence
counter is the better fit because accounting has one writer per core.

Each core gains an `acctSequence` value:

- Even means its published accounting state is stable.
- The local writer increments it to odd before a transaction.
- The writer updates all related fields.
- A release barrier precedes the increment back to even.
- Readers use acquire ordering and retry if the value is odd or changed.

Every accounting writer must participate:

- Scheduler entry charging and scheduler-exit restamping.
- Scheduler-cycle bucket updates.
- `acct_settle_local()` while the old path still exists.
- `mpAcctHaltBegin()` and `mpAcctHaltEnd()`.
- Initial meter setup and dispatch state publication.

The current scheduler charges the outgoing thread at pass entry but does not
move `acctLastDispatchTSC` until pass exit. Between those operations, a reader
would see a committed charge plus the old open-span boundary and would count
the span twice. Therefore the first implementation should leave the sequence
odd across that entire accounting transaction. If scheduler passes later prove
too long for bounded snapshot retries, the accounting transaction can be
restructured, but it must not publish a half-updated state merely to shorten
the odd window.

The sequence field belongs in the accounting portion of core-local storage.
Adding it at the end avoids changing offsets of assembly-visible CLS fields;
the generated offset dependency must still be rebuilt and verified.

## Phase 4: capture one immutable snapshot

### The central consistency algorithm

Exact coherence without either stopping cores or validating that they did not
change during capture is impossible. Reading every core once and attaching one
timestamp is not sufficient: a core could switch after its state was copied
but before the timestamp was taken.

The proposed capture uses optimistic global validation:

1. Ensure a reusable snapshot buffer has enough capacity. Do not allocate
   while holding scheduler or task-list locks.
2. For every active core, read an even `acctSequence` and copy its accounting
   state: current thread, idle thread, halted flag, meter epoch, last-dispatch
   TSC, scheduler cycles, and the fields needed for core buckets.
3. Under the task/thread topology lock, copy stable metadata and committed
   runtime counters into private records. No text formatting occurs here.
   If the preallocated capacity is too small, release the lock, grow, and
   retry.
4. While object lifetime is protected, translate copied thread pointers into
   snapshot record indexes or stable TIDs. After this point the renderer does
   not dereference a live task/thread pointer.
5. Release the topology lock and latch one normalized capture TSC.
6. Re-read every core's sequence counter. The snapshot succeeds only if every
   value is the same even value read in step 2.
7. Validate the task/thread topology generation as well. A topology change
   requires a retry.
8. For each core, virtually close its open span from normalized
   `acctLastDispatchTSC` to the common capture TSC and add that delta only to
   the private record for its current accounting target. If
   `acctCurrentHalted` is set, the target is the copied idle thread.

Why this produces a logical point-in-time result: if a core's sequence did not
change from its initial read through the capture timestamp, the copied state
was still that core's state at the capture timestamp. The private overlay can
therefore advance it to that timestamp without guessing across a switch.

If any sequence changes, discard the attempt and retry the whole capture. The
retry count must be bounded. On a quiet tickless machine most cores will be
stable indefinitely; on a busy periodic machine the capture window should be
kept short enough that a clean attempt remains likely.

### Snapshot-private virtual settlement

Virtual settlement never writes `runCycles`, never updates
`acctLastDispatchTSC`, and never sends an interrupt. It computes:

```text
open_cycles = capture_tsc_normalized
            - normalize(core, acctLastDispatchTSC)
```

and adds `open_cycles` to a private copy of the appropriate thread's runtime.
The same capture timestamp is used to close the core's total ledger. Task
runtime is then the sum of its copied thread records, so task, thread, and core
rows share the same endpoint.

All subtraction requires checked ordering. A normalized last-dispatch value
greater than the capture timestamp, a missing target record, or an invalid
clock is a failed capture, not an unsigned wraparound.

### Task and thread lifetime

The task list is not append-only. The undertaker unlinks tasks and eventually
frees them; new tasks and threads can appear while a monitor reads.

The snapshot must therefore do one of the following under an authoritative
topology lock:

- Copy every field it will need before releasing the lock, or
- Acquire explicit snapshot references which prevent freeing until capture
  completes.

Copying is preferred for the first version because the final product is an
immutable value object anyway. It requires auditing insertion, unlink, and
thread-list mutation so they all participate in the same lock and topology
generation. Any current insertion performed outside that lock must be fixed
before the generation check can be trusted.

No allocation, filesystem I/O, or formatting is allowed while that lock is
held. A count/capacity/retry pattern keeps the critical section mechanical.

### Bounded failure behavior

A reader must not spin forever because one core is constantly scheduling.
After a small named retry limit, the initial comparison build should report
the snapshot as incomplete and use the old settle-on-read result. It should
also increment diagnostics identifying:

- Retry count.
- Core whose sequence changed or stayed odd.
- Topology-generation retries.
- Clock-validation failures.
- Fallback count.

This turns a difficult timing failure into evidence instead of a silent hang.

## Phase 5: the proc accounting endpoint

### File organization

Proposed files:

```text
kernel/include/accounting_clock.h
kernel/src/accounting_clock.c

kernel/include/accounting_snapshot.h
kernel/src/accounting_snapshot.c

kernel/include/driver/filesystem/proc/proc_accounting.h
kernel/src/driver/filesystem/proc/proc_accounting.c
```

Responsibilities:

- `accounting_clock.*`: ordered TSC reads, boot synchronization, validity,
  normalization, and clock diagnostics.
- `accounting_snapshot.*`: sequence protocol, topology-safe capture, virtual
  settlement, retries, fallback decision, and immutable snapshot structures.
- `proc_accounting.*`: versioned text rendering and proc-node callbacks.
- `procfs.c`: only route/open registration for `/proc/accounting`.

The kernel makefile already discovers C files below `kernel/src`, so these
modules should not require a hand-maintained object list.

### Proposed text ABI

The exact spelling should be finalized alongside the parser, but the file
should begin with an explicit version and capture metadata, followed by core,
task, and optional thread records. For example:

```text
accounting_snapshot 1
capture_cycles 123456789
capture_us 352001234
complete 1
clock fixed_tsc
core 0 total_us=... busy_us=... idle_us=... sched_us=...
task 33 runtime_us=... state=running core=0 kernel=1 name=idle0
thread 33 task=33 runtime_us=... state=running core=0 name=idle0
```

Requirements for the format:

- One open creates or attaches to one immutable snapshot. Multiple `read()`
  calls on that descriptor must not recapture.
- Unknown record types and fields are ignorable so the format can grow.
- Names need unambiguous escaping or length-prefixing.
- The header reports completeness, retries, and whether a fallback was used.
- Core and task records use the same fixed cycles-to-microseconds conversion
  rate after capture.
- A documented maximum snapshot size prevents unbounded kernel allocation.
  Truncation must be explicit in the header, never silent.

A later binary syscall can reuse the internal snapshot object if profiling
shows text rendering to be material. It is not required to solve coherence.

## Phase 6: shadow comparison before switching `top`

The old path remains authoritative while the new one is exercised. A
diagnostic mode in `top` or a small test program should read both and log raw
values without changing the display.

Compare, per refresh:

- Snapshot capture interval.
- Per-core total, busy, idle, and scheduler deltas.
- Per-task and per-thread runtime deltas.
- Sum of thread rows versus their task row.
- Sum of core buckets versus core total.
- Difference between old IPI-settled cumulative values and new virtual values.
- Retry/fallback diagnostics.

Expected behavior:

- Cumulative old and new accounting should agree over longer windows within
  the measured clock uncertainty and interrupt overhead.
- The new one-refresh deltas should be smoother because all rows share one
  endpoint.
- A fixed-rate snapshot must never move cumulative runtime backward.
- `/idleN`, which is pinned to one core, must not exceed 100% when divided by
  that same snapshot's core interval.

The raw `top -l` checkout log is the right evidence channel. Screenshots are
useful confirmation, but raw microsecond deltas make endpoint errors
auditable.

## Phase 7: switch consumers and retire continuous settlement IPIs

After comparison is green:

1. Teach `top` to read `/proc/accounting` once per refresh.
2. Keep legacy per-task and per-core files for compatibility, but stop using
   them to build one display frame.
3. Verify that running `top` no longer increments settlement-broadcast or IPI
   diagnostics on a machine whose clocks are valid.
4. Confirm in QEMU monitor and on bare metal that idle APs remain halted while
   `top` runs.
5. Retain a callable legacy settlement fallback for invalid clocks during an
   initial release.
6. Remove automatic `mpAcctSettleAll()` calls from individual proc/sys reads.
7. Only after the fallback policy is settled should the old vector, ack array,
   and tripwire machinery be deleted.

The deletion must be its own controlled change. If a new accounting symptom
appears immediately afterward, regression-first analysis should compare it to
the last build with fallback available before changing snapshot math.

## Concurrency and correctness invariants

The implementation should state and enforce these invariants in code comments:

1. **Committed deltas are same-core.** Raw `runCycles` is never built by
   subtracting an unnormalized timestamp from another core.
2. **One conversion rate per boot.** `kCPUCyclesPerSecond` is fixed after boot
   calibration.
3. **One writer per core accounting record.** Remote readers observe through
   the sequence protocol; they do not update a core's fields.
4. **Sequence covers the whole transaction.** No even sequence value may
   expose a charged runtime with an old dispatch boundary.
5. **Snapshot capture is read-only.** Virtual settlement modifies only private
   records.
6. **One endpoint.** Every overlay and denominator in a snapshot uses the same
   normalized capture TSC.
7. **No live pointers after topology unlock.** The renderer consumes copied
   values and indexes, not task/thread objects which can be reaped.
8. **No allocation or formatting under scheduler/task locks.** Capacity grows
   outside and capture retries.
9. **Invalid clock means fallback.** There is no implicit offset of zero.
10. **Retries are bounded and diagnosed.** Contention degrades to an explicit
    fallback/incomplete result, never a permanent syscall spin.

## Important gotchas to audit during implementation

### Logical core indexes versus APIC IDs

Current code sometimes indexes `kCoreLocalStorage` with a loop index and
sometimes with an APIC ID. QEMU's contiguous IDs hide any mismatch. The clock
table and snapshot records must choose one canonical identity and translate at
the boundary. Sparse APIC IDs on real hardware must not become array indexes
without validation against `MAX_CPUS`.

### Scheduler reentrancy and interrupt priority

The accounting sequence must agree with the existing scheduler reentrancy
rules. The boot clock-sync vector must not preempt unfinished AP
initialization, and the later snapshot must never wait for an
interrupt-delivered answer while syscall entry has IF masked.

### Halted non-idle threads

The compositor demonstrated that `hlt` can occur while a normal task remains
the architectural current thread. Snapshot overlay must preserve
`acctCurrentHalted` and redirect the open span to the copied idle-thread record
exactly as live settlement does.

### Thread migration

A thread can move between cores, but it cannot run on two cores at once. Core
sequence validation plus topology locking must cover the transition so a
snapshot sees the thread either before or after migration, never as the open
target of both cores.

### Task removal and PID/TID reuse

Snapshot records should carry stable IDs and enough generation information to
distinguish an object reused after exit. `top`'s previous-value cache must drop
an old row when identity changes instead of subtracting the old process's
runtime from a new process with the same numeric ID.

### Counter width and signed offsets

TSC offsets are signed; cycle counters are unsigned. Normalization needs
checked addition/subtraction, including the negative-offset case, before any
timestamp ordering comparison. Cycles-to-microseconds conversion should divide
before multiplying where needed to avoid overflowing 64-bit intermediates.

### Snapshot buffer lifetime

Synthetic filesystem callbacks may render a file over multiple reads. The
snapshot object must be owned by the open file/handle and released on close.
A static global render buffer would race two readers and is not SMP-safe.

## Verification matrix

### Build and structural checks

- Kernel and userland compile with `-Werror`.
- Generated assembly offsets rebuild after CLS changes.
- `git diff --check` is clean.
- Symbol and reference searches find every accounting writer inside the
  sequence protocol.

### Boot modes

- `nosmp` / one core: reference offset zero, no sync IPI required.
- QEMU Q35 with 2 and 8 cores.
- Tickless scheduler, the default and primary target.
- Periodic scheduler as a high-write-rate retry test.
- VirtualBox, where host scheduling pauses have historically distorted timing.
- Bare-metal Bosgame/P5, including the normal multi-hour idle observation.

### Workloads

- Fully idle system with `top` refreshing normally and rapidly.
- One pinned CPU hog.
- Multiple hogs across cores.
- Frequent task creation/exit to stress topology capture and reuse.
- Thread migration and wakeups.
- GUI compositor alternating work and `sti; hlt`.
- Long scheduler/debug logging passes to force sequence retries.
- A deliberately forced invalid clock/large uncertainty to exercise fallback.

### Acceptance criteria

- Both built-in test phases pass.
- No panic, hang, unsigned runtime spike, or backward cumulative runtime.
- Core bucket cycles close exactly against that core's snapshot total before
  display rounding.
- Thread runtime closes against its task aggregate.
- A pinned idle task remains within 0–100% of its own core interval.
- Long-window old/new cumulative differences stay within documented clock and
  observer-overhead bounds.
- Once the new path is authoritative on valid clocks, a `top` refresh sends no
  accounting settlement broadcast and does not wake otherwise idle APs.
- An invalid or contended capture produces an explicit diagnostic and safe
  fallback, not guessed data.

## Recommended change sequence

Keep each item separately buildable and bootable:

1. The fixed-rate change (Phase 1, landed).
2. Add `accounting_clock.*`, boot-only TSC synchronization, diagnostics, and
   no accounting consumer changes.
3. Add the per-core accounting sequence and audit every writer, while leaving
   existing readers unchanged.
4. Add the internal immutable snapshot and kernel-side consistency tests.
5. Add `/proc/accounting` rendering outside `procfs.c`.
6. Add shadow comparison tooling; keep old display/output authoritative.
7. Switch `top` to the snapshot after comparison data is green.
8. Verify idle cores remain halted and fallback works.
9. Remove continuous settle-on-read calls.
10. Remove obsolete IPI machinery only in a later cleanup change.

This ordering preserves a known-good comparison lane at every risky step. It
also makes regressions legible: clock normalization, publication, capture,
formatting, consumer conversion, and IPI retirement each have their own first
bad build instead of arriving as one indivisible accounting rewrite.
