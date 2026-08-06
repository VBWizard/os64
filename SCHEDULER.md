# os64 Scheduler & SMP Subsystem

*The design record for scheduling and multi-core operation. Written so a
future contributor (human or model) can extend this without re-deriving any
decision — and, more importantly, without re-earning any scar. Most of what's
in here was paid for in multi-hour debugging sessions; the failure
fingerprints at the bottom are the receipts.*

## What this is

A preemptive, multi-core scheduler with global thread queues and per-core
LAPIC-timer-driven scheduling passes. Every core — BSP and APs alike — runs
the same entry path (`_schedule_ap` in scheduler.S) and the same policy code
(`scheduler_do` in scheduler.c); cores differ only in who fires their timer
and who processes signals (the BSP). Threads are the schedulable unit; tasks
own threads (see task.h/thread.h).

## Layer map

| Layer | Files | Job |
|---|---|---|
| Entry/exit asm | `scheduler.S` (`_schedule_ap`) | Interrupt entry, register capture to `mp_isrSaved*` arrays, CR3/stack switch, signal check (BSP), restore + `iretq`. Both scheduler vectors (timer 0x7E and manual 0x81) point here |
| Policy | `scheduler.c` | Queues, thread selection, store/load thread state, `scheduler_trigger`, the donuts |
| Per-core plumbing | `smp_core.c` | `send_ipi`, AP wake-up/init, LAPIC timer calibration + arming, TLB-shootdown ISR, per-core SYSCALL MSRs |
| SMP bring-up | `smp.c` | `init_SMP`: MP/MADT parsing, `kCPUInfo`, MAXCORES clamp, CLS array allocation |
| Signals | `signals.c` | `sigaction` (SIGSLEEP/SIGLOGFLUSH), `processSignals` (ISLEEP wake scan) |
| Core-local storage | `smp_core.h`, CLS struct in smp.h | `gs:0` self-pointer; per-core scratch that is valid under ANY CR3 |

## One scheduling pass, end to end

```
LAPIC timer (vector 0x7E, periodic) or self-IPI (0x81, from scheduler_trigger)
→ _schedule_ap:
   1. mp_inScheduler guard: if already set, EOI + iretq immediately.
      Set it BEFORE sti — see invariant 4.
   2. Read ALL FIVE interrupt-frame fields (RIP,CS,RFLAGS,RSP,SS) — while
      still on the interrupted thread's stack, BEFORE the CR3 switch
      (invariants 1-3).
   3. (BSP only) _check_signals → processSignals: wake expired ISLEEPers.
   4. Save GP registers into the per-core mp_isrSaved*[apic_id] arrays.
   5. mov cr3, kKernelPML4; switch RSP to this core's scheduler stack.
   6. call scheduler_do:
      - take kSchedulerSwitchTasksLock (test-and-set spinlock)
      - peek the runnable queue (justBrowsing) — if the winner is already
        the current thread, shortcut out (no switch)
      - else: "Time to make the donuts. (switch threads)" and
        scheduler_run_new_thread():
          · store mp_isrSaved* → outgoing thread->regs; requeue it
            (RUNNABLE, or ISLEEP if SIGSLEEP, or ZOMBIE if exited)
          · pick incoming thread (scheduler_find_thread_to_run, for real
            this time — increments everyone's starvation ticks)
          · load incoming thread->regs → mp_isrSaved*; move to RUNNING;
            tss_set_rsp0 so SYSCALL lands on the right kernel stack;
            cls->currentThread/task updated
   7. Restore GP regs from mp_isrSaved*, restore the thread's CR3 (skipped
      if unchanged), push the full 5-QWORD frame (SS,RSP,RFLAGS,CS,RIP),
      clear mp_inScheduler, EOI, iretq.
```

`iretq` is the unified return for BOTH rings: a ring-3 CS in the frame makes
it perform the privilege drop and stack switch natively, and it restores
RFLAGS from the frame — so user tasks resume with IF=1. (The old
`retfq`-based ring-3 path left user code running with interrupts off.)

> **OPEN BUG — an unhealed scar.** Something writes 8 stray bytes into
> `mp_isrSavedRFlags[core]` between `scheduler_load_thread` and the `iretq`.
> The thread structures are clean; only the per-core array is hit. It has been
> masquerading as an intermittent `#GP` on `/idle2` for weeks, and finally
> confessed as a `#DB` on 2026-08-02 when `DR6.BS` named single-stepping as the
> cause. Full forensics, method and next step: **`SCHEDULER_STRAY_WRITE.md`**.

## The core invariants (each one is a healed scar)

1. **Read all five interrupt-frame fields BEFORE the CR3 switch.** The frame
   lives on the interrupted thread's stack. For any task with its own PML4 —
   ring-0 kernel tasks AND ring-3 user tasks (whose RSP0 kernel stack is
   task-local, not HHDM) — that stack is NOT mapped in kKernelPML4.
   Dereferencing the frame after `mov cr3` reads the wrong physical page or
   faults.
2. **RSP comes from the frame's +24 FIELD — never from frame-base
   arithmetic.** In 64-bit mode the CPU aligns RSP down to a 16-byte boundary
   *before* pushing the frame, so "frame base + 40" recovers only the aligned
   value: an interrupted RSP not on a 16-byte boundary silently loses its
   remainder (up to 15 bytes) and the thread resumes reading byte-shifted
   qwords off its own stack. `-O0` C masked this for years (every
   `leave`/`ret` resyncs RSP from RBP); the syscall path's hand-written asm
   exposed it. The +24 field holds the TRUE pre-alignment RSP, pushed by the
   CPU itself.
3. **Long mode ALWAYS pushes and pops all 5 QWORDs — even ring0→ring0.**
   The 32-bit rule (3 QWORDs on same-privilege entry) does not exist in
   x86-64. Never "optimize" the frame down to 3; save/restore stays
   symmetric.
4. **Every scheduler entry is a real LAPIC interrupt, and the
   `mp_inScheduler` guard goes up before `sti`.** There used to be a
   `scheduler_yield()` that entered via software `int` — a software `int`
   never sets the APIC in-service bit, so it had none of the EOI-based
   nesting protection hardware entries get; a pending timer could re-enter
   `_schedule_ap` mid-prologue and clobber the half-saved context (first
   seen as a #GP at a byte-shifted non-canonical RIP on a test's first
   yield). It's gone: `scheduler_trigger()` sends a genuine self-IPI (0x81)
   instead, giving every entry identical interrupt semantics. The
   guard-before-sti ordering in the prologue stays regardless — it is
   correct for any entry path, present or future.
5. **`kSchedulerSwitchTasksLock` serializes all queue surgery.** Takers:
   `scheduler_do`, `processSignals`, `scheduler_reap_zombie_thread`. Anything
   new that walks or edits qRunnable/qRunning/qISleep/etc. takes it too.
   It's a raw test-and-set spinlock — do not hold it across anything that
   can sleep or re-enter the scheduler.
6. **`scheduler_trigger()` executes `sti` and may context-switch the
   caller.** Never call it where interrupts must stay off, and NEVER from an
   ISR (this is the scheduler-side half of the GUI's "IRQ handlers only
   enqueue" rule). Its wait loop is checked-`hlt`, not bare `sti;hlt`,
   because the self-IPI can fire and switch us away before we reach the
   `hlt`; on reschedule the flag is already clear and the loop exits.
7. **Kernel stack sizes MUST be page multiples.** `THREAD_KERNEL_STACK_SIZE`
   was once `0xFFFF` — "64KB" minus one byte — which made every kernel RSP
   odd and guaranteed invariant-2's alignment loss on every preemption.
   Allocation sizes for anything RSP will point at end in 000.
8. **CLS (`gs:0`) is the only per-core state you may touch across a CR3 or
   RSP switch.** It lives in the shared upper half, so it resolves under any
   address space; locals on the old stack do not (see CLAUDE.md's
   context-switching chapter for the full recipe).

## Timing and cadence (all the clocks in one place)

- **`kTicksSinceStart`** — THE global clock: IRQ0/PIT-fed on the BSP,
  `TICKS_PER_SECOND` = 100 (10ms/tick). Cross-core-safe by construction.
- **Per-core LAPIC timers** drive scheduling passes. Each AP calibrates its
  LAPIC frequency against `kTicksSinceStart` at bring-up
  (`mp_determine_local_APIC_timer_speed`, 3-round average), then arms
  periodic mode with `apicTimerCount = apicTicksPerSecond /
  MP_SCHEDULER_RUNS_PER_SECOND` (=100, raised from 10 for the GUI) — and
  since 2026-07-11 the config is the truth: a genuine ~100 passes/sec/core.
  (History: an arming multiplier `SMP_MAGIC_NUMBER=3` silently divided this
  to ~33 for most of the project's life — see "The magic number autopsy"
  below before ever reintroducing one.)
- **Signal processing** (`processSignals`) runs from the scheduler prologue
  on the **BSP only**, every `SIGNAL_PROCESS_TICK_FREQUENCY` (=1) passes.
  SIGSLEEP wake granularity = BSP pass cadence. This is the number that was
  10/sec (100-500ms perceived latency) before the cadence bump.
- **Never compare TSC values across a possible preemption or migration.**
  Per-core TSCs under QEMU/WSL2 are desynchronized enough that a cycle
  target computed before a preemption can resume unreachable — a
  near-eternal spin. Use `kTicksSinceStart` for any deadline a scheduler
  pass could interrupt. (This killed the GUI compositor's first pacing
  loop.)

## Thread selection (the policy)

Selection is starvation-based: each real pass, every non-idle runnable
thread's `prioritizedTicksInRunnable` grows by
`(RUNNABLE_TICKS_INTERVAL - task->priority) + 1` — lower `priority` value =
faster accumulation = scheduled sooner. Highest accumulator that *can* run
on this core wins; the winner's accumulator resets on requeue to RUNNABLE.
Idle threads never accumulate, so they win only when nothing else can run
(and if truly nothing can, that's the "No runnable threads found" panic —
idle threads must always be present and runnable).

- **Affinity:** `thread->mp_apic` = `THREAD_NO_AFFINITY` (run anywhere) or
  an APIC id (run only there). `scheduler_thread_can_run_on_core` is the
  single gate.
- **Wake boost:** `scheduler_wake_isleep_task` adds
  `HIGH_PRIORITY_TICKS_BOOST` (10M) so a woken thread wins its next
  eligible pass outright.
- **Known weakness (open):** a CPU-bound thread on an AP starves same-core
  siblings between passes; there is no intra-pass timeslicing beyond the
  pass cadence itself.

## Tickless mode — THE DEFAULT (sharp edges)

Tickless (`kTicklessScheduler`, default true since 2026-08-05; opt out with
`SCHED=periodic`) is the park-and-nudge scheduler: AP LAPIC scheduler timers
are left MASKED (`enableAPScheduling_ISR` bails for APs), and APs run only
when woken by `scheduler_nudge_parked_aps` sending a manual-schedule IPI —
pinned work nudges its designated core, unpinned work recruits the first
idle one. The name is aspirational on purpose: the BSP still ticks at 100Hz
and busy APs don't preempt yet; SCHEDULER_REDESIGN.md (net branch) is the
path to earning it fully. Born as the misnamed `BSPSCHED` flag — misnamed
because the BSP neither owned the nudging (any core nudges) nor the
scheduling (every nudged core runs its own selection pass); flag retired,
no alias, all boot entries migrated the same day. Consequences, learned the
hard way:

- A thread pinned to an AP under tickless is **never preempted there** — no
  timer, no pass, nothing. A busy-spin wedges that core forever (observed:
  GUI compositor dark on core 1). Nudges are the only scheduling events.
- Wake latency on APs is nudge-driven and bursty.
- The GUI boot entries carry an explicit `SCHED=periodic`, and `gui_start()`
  refuses to pin the compositor under tickless.
- `SCHED=periodic` is also the REPRO MODE for the open /idle2 stray write
  (SCHEDULER_STRAY_WRITE.md) — the dedicated Limine entry at the bottom of
  limine.conf must outlive that bug.

**Is the GUI incompatibility fixable? Yes — it's a fossil, not physics.**
The wedge dates from the compositor's tick-spin era, when it busy-waited
between frames; with no AP timer to preempt it, the pin was fatal. Today's
compositor hlt-waits (frame pass → `sti;hlt`), which is exactly the
citizenship tickless wants from a pinned thread: input IRQs already route
to its core on TPR-safe vectors and end the halt directly. One wake path is
missing — a client on ANOTHER core publishing damage doesn't interrupt the
halted compositor core, which would sit dark until the next input event.
The fix (small, unbuilt): `gui_damage_add()` sends a nudge IPI at the
compositor's core when that core is remote and parked. With it, tickless
keeps its long-parked idle cores AND gets a live GUI. Tradeoff to accept
knowingly: under tickless a compositor bug that spins is still a wedged
core with no preemption rescue — tolerable only because the compositor's
whole design is "do a frame, halt." Until the damage-wake exists,
`gui_start()`'s pin refusal stays.

## Vectors, IPIs, and the TPR trap

| Vector | Name | Handler |
|---|---|---|
| 0x7B | IPI_INVALIDATE_TLB | `inv_tlb_ISR` (CR3 reload) — broadcast by `mpSendInvTLB` (no-op until kSMPInitDone: never IPI a parked core) |
| 0x7C / 0x7D | disable / enable AP scheduling | `disableAPScheduling_ISR` / `enableAPScheduling_ISR` (LVT timer mask/unmask + re-arm) |
| 0x7E | IPI_TIMER_SCHEDULE | `_schedule_ap` — the periodic per-core timer vector |
| 0x7F | IPI_AP_INITIALIZATION | `ap_initialization_handler` (per-core SYSCALL MSRs — STAR/LSTAR/SFMASK/EFER.SCE — CLS, timer calibration) |
| 0x81 | IPI_MANUAL_SCHEDULE | `_schedule_ap` — `scheduler_trigger` self-IPI and tickless nudges |

- **APs run with LAPIC TPR = 0x30** (set in `ap_wakeup_after_stack_switch`):
  every vector below 0x40 is silently held on APs — no fault, no log, the
  LAPIC just never delivers it. Any IOAPIC route or IPI targeting an AP must
  use a vector ≥ 0x40 (the GUI's input IRQs use 0x41/0x4C for exactly this
  reason; 0x21/0x2C exist only as legacy-PIC fallbacks). The BSP has no such
  TPR, which is why BSP-routed low vectors always worked and the trap stayed
  hidden.
- **EOI discipline:** every path out of a scheduler/IPI handler writes EOI
  exactly once (`_write_eoi` / `write_eoi`). The early-exit
  (already-in-scheduler) path EOIs too — miss one and that core never takes
  that interrupt class again.
- **`send_ipi` panics rather than hangs:** the ICR delivery-status wait is
  bounded; a wedged target core produces a loud "stuck busy" panic naming
  sender, target, and vector instead of an invisible spin.

## SMP bring-up (and MAXCORES)

`init_SMP` parses MADT/MP tables into `kCPUInfo`, clamps `kMPCoreCount` to
`MAXCORES=<n>` if given (the single point of truth — everything downstream
just sees fewer cores; capped-off cores stay parked in Limine's AP loop,
never woken). Then `ap_wake_up_aps`, per AP: write the AP's Limine
`goto_address` → AP runs `ap_wakeup_entry` (own CR3/GDT/TSS/IDT, real stack,
CLS) → handshake on `coreAwoken` → init IPI (0x7F: MSRs + timer calibration)
→ handshake on `coreInitialized` → enable-scheduling IPI (or a single manual
kick under tickless). Finally `kSMPInitDone = true`, which un-gates
TLB-shootdown broadcasts.

**MAX_CPUS is 24.** The 3900X (24 threads) sits exactly at the boundary —
index 24 would be one past the arrays. Anything bigger, or any APIC id ≥ 24,
needs MAX_CPUS raised (it sizes every `mp_isrSaved*` array, kCPUInfo, CLS
array, and friends) or MAXCORES used as the guard rail.

## The donuts (mandatory)

`"Time to make the donuts. (switch threads)"` prints (under
DEBUG_SCHEDULER) on every actual thread switch. It has been in every one of
Chris's kernels since day one of his FIRST OS. It is not debug noise; it is
load-bearing tradition. Gating *when* it prints is acceptable — donuts
print when donuts are made. Removing or rewording it is not. You have been
warned.

## Recipes

**Sleep and wake:**
```c
sigaction(SIGSLEEP, NULL, kTicksSinceStart + nTicks, NULL); // sleeps CURRENT thread
// ... BSP's processSignals moves it back to RUNNABLE when ticks expire ...
scheduler_wake_isleep_task(task);  // early wake + priority boost + trigger
```
Wake granularity is the BSP pass cadence (~10-30ms); don't build anything
that needs finer.

**Pin a thread to a core:** set `thread->mp_apic = <apic_id>` before it
becomes runnable. Remember: under tickless it will be nudge-only and
unpreemptable there; any IRQ it depends on must ride a vector ≥ 0x40 if the
core is an AP.

**Add a new IPI:** pick a free vector ≥ 0x40 (APs!), `set_idt_entry` it in
idt.c, keep the handler enqueue-only or scheduler-safe, EOI exactly once,
`iretq`. If it touches thread queues, take kSchedulerSwitchTasksLock and get
out fast.

**Touch scheduler state from anywhere else:** interrupts off, take
`kSchedulerSwitchTasksLock`, do the minimum, release. If you need a
reschedule afterwards, release FIRST, then `scheduler_trigger` (it sti's —
invariant 6).

## Scheduler debug logging vs. the serial wire (do the math first)

COM1 runs at 115200 baud, 8N1 — ten wire bits per byte (start + 8 + stop),
so the port moves at most **11,520 bytes/second**. (Observed QEMU/WSL2
throughput can be lower still — see the unsolved serial slow-walk.) Budget
against the scheduler's chatter BEFORE enabling DEBUG_SCHEDULER on
many-core hardware:

- Plain DEBUG_SCHEDULER emits ~300 bytes per pass (banners + status). At
  the true 100 passes/sec, ONE core produces ~30KB/s — **nearly 3× the
  wire by itself.** (At the old ~33/sec effective cadence one core just
  fit, which is how the problem stayed hidden for years.)
- Twelve cores: 12 × 100 × ~300B ≈ **360KB/s — thirty times the wire.**
- Add DEBUG_DETAILED and `scheduler_find_thread_to_run` prints a line per
  runnable thread per pass — multiples worse again.

The log path buffers what the wire can't drain (5MB/core of runway, and
the never-drop rule means nothing is discarded); sustained oversubscription
eventually reaches the forced-flush path, whose LOGFULL tripwire prints the
moment a producer starts paying the drain cost itself. This family of
overload is what crashed the 12-core Bosgame with default logging (fixed
with `nolog` + `MAXCORES=4` on that entry). Rules of thumb: on more than ~2
cores boot `nolog` and enable narrow subsystem bits (DEBUG_GUI) instead of
DEBUG_SCHEDULER; if you truly need scheduler logs on wide hardware, cap
cores with MAXCORES and accept the slowdown.

### The magic number autopsy (2026-07-10/11) — read before "optimizing" cadence

For most of this project's life, a `SMP_MAGIC_NUMBER 3` multiplier in the
LAPIC arming silently divided the configured 100 passes/sec to ~33. The
two-day investigation that removed it is this subsystem's best cautionary
tale, and every layer was MEASURED (RTC-referenced probes; the technique is
in VERIFICATION.md):

- The PIT, the tick clock, the APIC calibration, and the arming were all
  proven EXACT — wall clock 100.0 ticks/sec against the CMOS RTC, per-core
  timer fires matching the armed rate on every core. The multiplier was
  compensating for none of them.
- What it actually rationed: **DEBUG_SCHEDULER's printd volume.** Each
  verbose pass performs ~6 printds — two vsprintf-class formats each —
  largely INSIDE `kSchedulerSwitchTasksLock`, INSIDE the un-EOI'd
  interrupt. Under QEMU/TCG at -O0 that cost 5-23ms per pass (measured;
  single passes up to **1.3 seconds**).
- Consequences, each confirmed numerically: all cores convoy on the one
  lock (~195 passes/sec system-wide ceiling ÷ 4 cores ≈ the observed
  44-56/sec each); the BSP's long in-service windows starve IRQ0 — the
  LAPIC pending bit holds exactly ONE tick, so at ~2.3 ticks arriving per
  window, survival ≈ 1/2.3 ≈ 43% (measured: 48 ticks/sec); and AP timer
  calibrations performed against the starved tick clock came out ~2× wrong.
- On VBox the same config froze outright at ~100 passes/sec (still an open
  case — the non-irqsave lock's self-deadlock is the prime suspect; see
  DEBTS).

**The lesson: cure the pass COST, never the pass RATE.** The remedies (all
in DEBTS): move printds outside the scheduler lock; the prologue reorder
that legalizes an early EOI (read the five frame fields, THEN EOI — with
the `mp_inScheduler` guard checked BEFORE the `temp_rsp` store so a nested
entry can't clobber it; the whole pre-`sti` prologue runs with IF=0, which
makes that reordering atomic); and/or routing IRQ0 above the scheduler's
priority class so the wall clock is unstealable no matter what a pass does.

## Failure fingerprints (symptom → cause)

- **Garbage values that look like real kernel addresses shifted by a byte**
  (e.g. `0x4f000000000008b0`), #GP at a non-canonical RIP, crashes that
  appear/disappear with -O0/-O2: an RSP recovered by arithmetic instead of
  the frame's +24 field (invariant 2), or a non-page-multiple stack size
  (invariant 7) re-creating the misalignment.
- **#PF/#GP immediately after the scheduler's CR3 switch:** a frame field
  read AFTER `mov cr3` (invariant 1).
- **A core wedges hard with one thread and heartbeats stop, system otherwise
  alive:** pinned thread busy-spinning on an AP under tickless (no timer =
  no preemption), or a spin on a TSC deadline captured before a preemption
  (use kTicksSinceStart).
- **Device interrupts silently never arrive, but only on APs:** vector
  < 0x40 vs. the AP TPR of 0x30. Reroute ≥ 0x40.
- **A core stops taking scheduler interrupts entirely:** a handler path that
  missed its EOI.
- **`send_ipi: ICR delivery-status stuck busy` panic:** the *target* core is
  wedged (see tickless/TSC causes above) — the panic is the messenger.
- **`scheduler_store_thread: AP storing CS=0` panic:** the mp_isrSaved slot
  for that core was never populated — an entry path skipped the register
  save (historically: first pass on a fresh AP; `mp_CoreHasRunScheduledThread`
  gates this).
- **`No runnable threads found` panic:** the idle threads are missing from
  qRunnable or the queue links are corrupt — check recent queue surgery for
  lock violations (invariant 5).
- **Everything schedules but sleeps oversleep massively:** signal cadence —
  check `SIGNAL_PROCESS_TICK_FREQUENCY` and that the BSP (the only signal
  processor) isn't starved.
- **Wall clock runs slow (ticks/sec below TICKS_PER_SECOND) under verbose
  logging:** the autopsy's disease — long un-EOI'd passes are eating PIT
  ticks (the pending bit holds exactly one). Measure pass cost, get the
  printds out of the pass/lock; don't touch the timer, it's innocent.
- **Many-core boot dies early with verbose logging (bare metal) or crawls
  (QEMU):** serial oversubscription — see "do the math first" above.
  `nolog` + MAXCORES, then re-enable narrow debug bits.

## Known limitations / future work

1. Fork's register-load path is unfinished (`scheduler_load_thread` panics
   on `justForked` — the fork/exec work will finish it).
2. No intra-pass timeslicing: AP sibling starvation under a CPU-bound
   thread.
3. An IRQ-safe wake primitive (event-driven, sub-tick) — only if a real
   need appears; the 100Hz cadence covers the GUI.
4. Printds execute inside `kSchedulerSwitchTasksLock` — the convoy the
   autopsy convicted. Move them out (queue surgery stays locked;
   find-then-run stays atomic). Requirement from Chris: DEBUG_DETAILED must
   be slow-but-honest, never a lockup.
5. MAX_CPUS=24 vs. the 3900X boundary (above).
6. Tickless + GUI coexistence: needs only the damage-wake nudge IPI (see
   the tickless section) — `gui_start()`'s pin refusal is tick-spin-era
   conservatism kept until that exists.
7. Wall-clock hardening (the autopsy's structural fixes): early EOI via the
   prologue reorder, and/or IRQ0 above the scheduler's priority class.
8. `kSchedulerSwitchTasksLock` is raw and non-irqsave, and is taken from
   thread context (reap) — self-deadlock possible (the open VBox-freeze
   suspect). Fix: irqsave + a bounded-spin panic tripwire (send_ipi's ICR
   pattern).
9. The VBox freeze at 100 passes/sec + DEBUG_SCHEDULER: unexplained on
   native-speed hardware (the TCG convoy math doesn't transfer). Hunt with
   the #8 tripwire.
