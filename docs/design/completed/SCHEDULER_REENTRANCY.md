# Scheduler ISR re-entrancy: the corruption zoo of 2026-08-09/10

**Status: DIAGNOSED 2026-08-10 by Fable; IMPLEMENTED 2026-08-10 by Opus 5
(Fix 1 + Fix 2 as specified, plus a third change the review turned up — the
scheduling IPI's priority class, § The vector map, below). Awaiting Chris's
soak.**
Diagnosed by Fable via frozen-guest monitor forensics (the QEMU-monitor
technique, same family as SCHEDULER_STRAY_WRITE.md); fix design amended by
Chris's regression testimony (the clock-tick loss that motivated removing the
old CLI/STI pair). This document is the implementation handoff: everything the
implementer needs is here — the evidence, the mechanism, the exact fix spec,
the verification plan, and the traps.

**Read this before touching `kernel/src/scheduler.S`. Do not re-derive.**

---

## The symptom zoo (why this wore so many masks)

All of these, under the 5-VT repro (VT1=top, VT2=hog, VT3=`ls /bin -l`,
VT4=`ps -ef`, VT5=top) in **periodic** scheduler mode, over minutes:

- `#GP` err=0 on AP idle tasks dereferencing `cls->currentThread` whose value
  was **non-canonical garbage that decodes as x86 instruction bytes**
  (crash site: the `*Shortcut!` `printd` at scheduler.c:1286, whose argument
  list dereferences even with DEBUG_SCHEDULER off)
- `#GP` in `sigaction` (signals.c:47) with a garbage thread pointer, from the
  `syscall_sleep` path
- No-VMA panic: ring-0 `#PF` at a **task-local kernel-stack VA** (e.g.
  `0x1011bfa8`) taken from **idle context** whose CR3 doesn't map it
- User tasks segfaulting at absurd addresses (top killed by a read at `0x81`)
- Pre-hoist (before 2026-08-09): the same events presented as reproducible
  `#DF` → triple fault in ~2 minutes

One disease, many coroners' reports. The stale-CR2 values captured at two
panics (`0xc590c7c748c68b64`, `0xba41b0458b48c9fd`) decode as REX-prefixed
instruction bytes — pointers were being replaced with *copied program text*,
which is what "resumed a thread with another context's registers" looks like
downstream.

## What was ruled out (with proof)

- **CLS content corruption**: after Chris moved `kCoreLocalStorage` from
  kmalloc to a BSS array (aligned(64) struct, `kCLSInitialized` replacing the
  null checks), the very next crash's frozen guest showed the array PRISTINE
  — all `self` pointers, `apic_id` 0-3, all `currentThread` values canonical
  and valid. CLS was a *victim class*, not the disease. (The BSS move stays:
  it is architecturally right regardless — static per-CPU state, no allocator
  dependency, no heap neighbors.)
- **`task_exit_with_retval` trampoline**: Chris's cli/sti bracket around it
  changed nothing.
- **Heap-neighbor overrun of CLS**: mooted by the BSS move; the crash
  recurred anyway.

## The forensic chain (2026-08-10, frozen guest)

1. No-VMA panic on AP1, address `0x1011bfa8`, "excepting task" idle-family
   (thread 0x22 = idle1). Guest paused via monitor (`stop` on port 55555).
2. AP1's stack walked by hand: **both** exception frames intact.
   Outer frame: idle1 interrupted at `task_idle_loop`, its real hardware
   frame based at `0x40013f98` (its own stack). Inner frame: `#PF` err=0 at
   the scheduler ISR prologue's frame-copy read (`scheduler.S` label
   `over_check_signals` region, the `mov rbx,[rbx]` reading frame field 0
   through `temp_rsp[apic_id]`).
3. Arithmetic check: the prologue's `lea rbx,[rsp+16]` for THIS entry
   provably computed `0x40013f98` (matches the inner frame's RSP), and wrote
   it to `temp_rsp[1]`. ~20 instructions later the read-back produced
   `0x1011bfa8` — a **task**-stack frame base. The slot was overwritten
   mid-prologue.
4. Post-mortem slot dump: `temp_rsp[1]` contained `0xffff800005f85f98` — a
   **scheduler-stack** frame base. The ONLY way a scheduler-stack VA enters
   `temp_rsp` is an interrupt taken **while already running on the scheduler
   stack**. That is a fossil of a penetration; it cannot occur if the
   critical section actually held.
5. `grep -c cli kernel/src/scheduler.S` → **zero**. One `sti` (prologue,
   after the guard store). The entire pass — prologue, signal processing,
   dispatch, exit, `iretq` — runs with IF=1, guarded only by
   `mp_inScheduler[]`, which gates the *code paths* but not the *data*.

## The mechanism — three windows, one magnet

`mp_inScheduler[apic]` makes a nested scheduler entry exit "harmlessly"
(the bounce path: restore regs, `_write_eoi`, `iretq`). But:

### Window 1: `temp_rsp` is written BEFORE the guard is checked
Prologue order today: `lea rbx,[rsp+16]` → `mov [temp_rsp+rax*8],rbx`
(scheduler.S:113-115) → **then** `mov bl,[mp_inScheduler+rax]` / branch
(118-121). So every bounce — by design "harmless" — **clobbers the outer
entry's saved frame pointer on its way through the door.** The outer entry
then copies the WRONG frame's RIP/CS/RFLAGS/RSP/SS into `mp_isrSaved*`
(lines 155-191) and that garbage becomes the interrupted thread's saved
state. The thread later "resumes" into nonsense.

### Window 2: `processSignals` runs guard-up with IF on
The BSP calls `_check_signals` → `processSignals` (a full C function) inside
the same unprotected span (scheduler.S:137-139 → 57). Any bounce during it is
a Window-1 hit with a much wider target. (`_check_signals` itself is clean —
it push/pops everything including RAX.)

### Window 3, the magnet: EOI → guard-clear → `iretq`
Exit order today (scheduler.S:450-453):
```
call _write_eoi                      # releases APIC in-service hold
mov byte ptr [mp_inScheduler+rax],0  # drops the software guard
mov rax,[mp_isrSavedRAX+rax*8]
iretq
```
The APIC's in-service bit really does shield the scheduler vector for the
whole pass (the old prologue comment is right about that). **The EOI is where
the shield expires** — three instructions before `iretq`, with IF=1 and the
guard down. Every scheduler-vector interrupt that went pending during the
pass delivers **exactly there**: a FULL nested entry (guard is 0, so no
bounce) that captures "mid-exit, on the scheduler stack, under the outgoing
thread's CR3" as that thread's saved state. The poisoned thread is re-queued;
when any core later dispatches it, it "resumes" onto the original core's
scheduler stack — **two cores, one stack** — and the mutual scribbling
produces every fingerprint in the zoo, including pointers full of
instruction bytes. This is what wrote the fossil in step 4.

### Why the #DF stack hoist "caused" this on 8/9 (it didn't)
Pre-hoist, a nested interrupt in the CR3/RSP gap double-faulted in ~2
minutes — the corruption died before it could express. The hoist made
nesting *survivable*, unmasking the data damage. The hoist is correct; keep
it.

### Why tickless mode survives for days (and the P5 record is real)
The magnet only fires when a scheduler-vector interrupt is PENDING at EOI
time. Periodic mode: 100Hz per core × 4 cores, and WSL2 can deschedule a
vCPU mid-pass for milliseconds, making straddled ticks routine. Tickless:
AP timers are parked; APs receive scheduler interrupts only as nudge IPIs,
almost always while HALTED, essentially never pending mid-pass; only the BSP
still ticks. Tickless is protected by **silence, not correctness** — the bug
exists there too, starved of triggers. (22-hour P5 bare-metal uptime with hog
pinned, 2026-08-10, tickless — consistent.)

### Why NOT "cli for the whole pass" (Chris's regression testimony — binding)
The wall clock is IRQ0's own micro-handler
(`kernel/src/driver/system/handler_irq0_timer.S`): increment counters, EOI,
`iretq`, touches nothing else. The LAPIC IRR holds only ONE pending instance
per vector: full-pass CLI + a WSL2 stall spanning multiple tick periods
collapses them — this was tried historically and **lost clock ticks in bulk**.
Do not resurrect it.

**Correction (2026-08-10 review, before implementing):** the original draft of
this section justified the ruling by saying the clock stays true *because
IRQ0 can nest into a long pass at any point*. It cannot, and has not been able
to since IRQ0 moved to the IOAPIC. IRQ0 is **vector 0x20** (`kernel.c:306`,
`kIRQ0UsesLapic = true`), priority **class 2**; a pass entered on 0x7E holds
ISR class 7, so the LAPIC refuses to deliver IRQ0 for the WHOLE pass, `cli` or
no `cli`. Ticks are *queued in the IRR and released by the EOI*, not nested.
So the mechanism of the historical tick bleed was something else (the
pre-0x7E era, or the ExtINT/PIC path, which genuinely does bypass APIC
priority). **The RULING still stands, on the surviving half of its reasoning:**
one IRR slot per vector means any long IF=0 span coalesces ticks, and the fix
below keeps that span to about a dozen instructions. The premise is corrected
here so nobody builds on it — and because it is exactly what makes the IRQ0
promotion (below) worth doing.

### The vector map — and why the software guard was never the primary defense
Resolved during implementation review; this is the "open item" the diagnosis
flagged and did not answer.

| Vector | Class | Who |
|---|---|---|
| 0x20 | 2 | IRQ0 / PIT — the wall clock |
| 0x41 / 0x45 / 0x4C | 4 | keyboard, e1000 INTx, mouse |
| 0x7B–0x7F | 7 | TLB shootdown, sched enable/disable, **scheduler timer 0x7E**, AP init |
| 0x81 | 8 | **manual scheduling IPI** ← the problem |
| 0x82 | 8 | accounting settle |
| 0xF0 / 0xF1 | 15 | spurious / base timer |

LAPIC delivery is arbitrated by priority *class* (vector >> 4): an interrupt
is held pending unless its class is strictly greater than the current PPR
class. So the in-service bit on 0x7E shields a pass against everything at
class ≤ 7 — but the scheduling IPI sat at **0x81, class 8, and outranked it**.
Every nudge from another core (`scheduler.c:194/246/971`, `task.c:172`,
`smp_core.c:343/360` — including the *self*-IPI) could therefore be delivered
into a core already mid-pass, walking straight through Window 1. The remote
check at `smp_core.c:143` ("don't send if the target's `mp_inScheduler` is
set") narrows the race but cannot close it: it reads another core's guard with
no atomicity, and the target can enter the scheduler in the gap.

**Fix 3: `IPI_MANUAL_SCHEDULE_VECTOR` moved 0x81 → 0x7A**, into the timer
vector's own class, where the hardware holds the second entry pending until
the first EOIs. 0x81 was an accidental number, not a ruling. `mp_inScheduler`
is now the backstop it was always meant to be rather than the only wall.
Rule for anyone renumbering: **both scheduler entry vectors must be
neighbours in one class** — "≥0x40 for the AP TPR" is necessary, not
sufficient.

Note what this table also says about the EOI: a non-specific EOI clears the
*highest* in-service bit, and a handler that was allowed to preempt is by
definition the highest — so properly-nested handlers always clear their own.
The scheduler's shield can only be stolen by something delivered *below* its
class, which the LAPIC will not do.

### Follow-on — TRIED AND REVERTED 2026-08-10: promote IRQ0
**Do not re-attempt from this section alone** — the argument below is sound and
the outcome was still a frozen hypervisor. The authoritative postmortem is the
`IRQ0_APIC_VECTOR` headstone in `kernel/include/smp_core.h`; the DEBTS row
carries the remaining work.

**What happened:** promoted 0x20 (class 2) → 0xE0 (class 14). QEMU was perfect —
24+19 green both modes, healthy clock, e1000 INTx confirmed on GSI 20. **VBox
froze at boot**: the e1000 probe reported every candidate GSI silent, on both
emulated card models, and the OS wedged at scheduler start. A DIRECTLOG boot
gave the decisive reading — `kTicksSinceStart` climbed normally to 560, then all
nine "GSI n stayed silent" lines carried the SAME timestamp, 597, across ~4.5
seconds of wall clock. The clock did not slow; it STOPPED, and every other
interrupt with it. That is the signature of a **stuck LAPIC ISR bit**: at class
14 it pins PPR and blocks the NIC (class 4), the scheduler (class 7), and IRQ0
itself. Almost certainly not a bug the promotion created — one it made lethal,
since a stranded bit at class 2 blocks nothing anyone would notice.

**The transferable lesson:** raising an interrupt's priority does not merely
speed it up, it changes what a pre-existing fault can reach. And no vector high
enough to outrank the scheduler's class 7 can avoid outranking devices at class
4, so this hazard is inherent to promoting an interrupt-COUNTED clock. The
answer is to stop counting interrupts and read a counter.

The good half of this section — the diagnosis of why ticks are lost at all —
remains correct and is why the DEBTS row still exists. The original reasoning
follows.


The clock is class 2 — nearly the lowest-priority thing in the machine — so
every scheduler pass holds it off entirely, and a pass that spans two tick
periods (a WSL2 vCPU deschedule, or the 45-second polled-UART stall on the
open-items list) loses ticks in bulk to IRR coalescing. On the 8259 IRQ0 was
priority **0**, the highest interrupt in the PC/AT, deliberately, because
timekeeping must never be starved; the APIC made priority a function of vector
number and IRQ0 kept its legacy 0x20 out of inertia, which silently demoted
the system clock to the bottom of the machine. Promoting it to ~0xE0 (above
every vector in use, below spurious) restores the original intent, and is safe
precisely because the handler is a true micro-handler: three `lock inc`s on
globals nobody else owns, no locks, no subsystem state, and it cannot nest
into itself.

Held for a SECOND commit on purpose: verification step 2 below measures clock
drift across the soak. Run it with this fix alone for a baseline, then promote
and measure again — otherwise neither number is attributable. Traps for that
slice: keep an IDT entry at 0x20 as well (before `remap_irq0_to_apic` runs,
IRQ0 arrives via the legacy PIC), and note that `apicGetHZ` (apic.c:144)
programs LVT_TIMER with vector 32 = 0x20 during calibration — it never fires
(one-shot, count 0xFFFFFFFF, 10ms wait), but it *looks* like IRQ0 and wants a
comment rather than a surprise.

---

## THE FIX (specified — implement exactly this shape)

Keep IF=1 for the entire scheduler body. Two surgical changes:

### Fix 1: move the `temp_rsp` write below the guard
Nothing before the guard branch may write ANY global. The bounce path must
touch only its own stack, `_write_eoi`, and `iretq`.

Suggested prologue order at `not_in_scheduler_continue` (register state
there today: one push deep — original RBX on stack; RBX holds original RAX;
RAX holds apic_id):
```
mov byte ptr [mp_inScheduler + rax], 1   # guard up (already first — keep)
mov [mp_isrSavedRAX + rax * 8], rbx      # save original RAX (frees RBX)
pop rbx                                  # original RBX; RSP now = frame base
mov [mp_isrSavedRBX + rax * 8], rbx
mov [temp_rsp + rax * 8], rsp            # frame base, written POST-guard
sti                                      # open the body only after the above
```
Then the BSP-only `_check_signals` call, then the existing CR3 save and the
five frame-field reads (which MUST stay before the CR3 switch and the stack
hoist — do not reorder those; their comments explain the constraint).
Note the current code's `lea rbx,[rsp+16]` dance exists only because the
write happened two pushes deep; after both pops, `mov [temp_rsp+rax*8], rsp`
is exact and needs no scratch register. Delete lines 113-115's early write
and the lea. Preserve the existing comments' content where still true; update
the prologue's big sti-safety comment to name the EOI expiration (this doc).

### Fix 2: make the exit atomic — `cli` from frame build through `iretq`
In the exit path (scheduler_continue region), place `cli` BEFORE the first
push of the outgoing 5-QWORD iretq frame (before the SS/RSP pushes — find
the first push of the five; it is above the RFLAGS-tripwire block), and do
not re-enable. Required order:
```
cli
<build 5-QWORD frame: SS, RSP, RFLAGS(tripwire), CS, RIP>
<mp_lastIretqRIP breadcrumb, guard sanity check — unchanged>
mov rbx,[mp_isrSavedRBX + rax*8]
call _write_eoi                          # EOI now INSIDE the cli region
mov byte ptr [mp_inScheduler + rax], 0
mov rax,[mp_isrSavedRAX + rax*8]
iretq                                    # restores the thread's IF atomically
```
Effect: the interrupt released by the EOI (and any pended IRQ0) delivers
AFTER `iretq`, into a fresh scheduler entry with a clean frame — which is
the correct meaning of "a tick was pending." The IF-off span is ~a dozen
instructions; a clock tick is lost only if the host stalls inside it for >2
tick periods, versus the old full-pass exposure. The RFLAGS tripwire block
stays exactly as is (it reads the frame value, not live RFLAGS).

### Explicitly unchanged
- The #DF stack hoist (stack-before-CR3) — load-bearing, keep.
- The bounce path (`already_in_scheduler_exit`): still EOIs its own
  interrupt instance and `iretq`s. After Fix 1 it is data-clean.
- `handler_irq0_timer.S` — not part of this; the clock keeps nesting freely.
- `scheduler_halt_and_catch_a_coffee2` sanity trap.

### While in there — dispositions (2026-08-10)
- **The disabled-`printd` hazard: BOOKED AS DEBT, not patched.** It is not one
  line — `printd` is a varargs *function*, so the level test happens inside
  the callee and EVERY call site in the kernel evaluates its arguments whether
  or not they are printed. Chris's ruling: honest debt, known from the
  beginning, and the cure (a gating macro) needs a side-effect sweep of every
  call site first. Row is in DEBTS.md § Kernel robustness.
- **The vector map question: ANSWERED, and it changed the fix** — see § The
  vector map above. It was not a documentation item; the scheduling IPI
  outranked the shield, which made Fix 1 load-bearing rather than hygiene.
- **`scheduler_halt_and_catch_a_coffee2` now panics** instead of spinning
  silently — after Fix 2 that spin sits inside the `cli` region, where it
  would be a mute hard hang, the worst possible ending for a tripwire. It
  calls `panic_no_shutdown` (NOT `panic`: reaching that label means scheduler
  state is provably corrupt, and the orderly-shutdown path would ask the
  broken machinery to walk itself to the door). Test-fired 2026-08-10 by
  turning the `je` into a `jmp` for one boot: banner, core number, emergency
  flush, and post-flush repeat all landed on the wire. A tripwire nobody has
  watched fire is a tripwire you do not know works.

## Traps for the implementer

1. **Intel syntax, destination first, in EVERYTHING** — inline asm included.
   On 8/9 an AT&T-order `mov %%cr3` diagnostic WROTE to CR3 instead of
   reading it and triple-faulted every exception for an hour. The idiom to
   copy is in handle.c:129 / syscall.c:228. When touching scheduler.S, `.S`
   files here are already `.intel_syntax noprefix`.
2. The five frame-field reads must remain BEFORE the CR3 switch (task stacks
   are not mapped under kKernelPML4) and must read the +24 RSP FIELD, never
   frame-base+40 arithmetic (the CPU's 16-byte alignment eats the remainder
   — see the x86-64 interrupt-frame house memory and the comment at the
   read site).
3. Do not move `_write_eoi` outside the cli region, and do not EOI twice.
4. `_check_signals` preserves RAX today (push/pop) — if anyone "simplifies"
   it, the slot index for every save after the call is in RAX. Leave it.
5. Chris tests before any commit. Build both modes; he drives the repro.

## Verification plan

1. **The repro**: periodic mode, 5 VTs (top / hog / `ls /bin -l` / `ps -ef`
   / top). Baseline: died in 20s-to-minutes (segfault storm, then #GP or
   no-VMA). Target: 30+ minute soak, zero anomalies in serial log
   (`grep -E 'exit code 139|#GP|No VMA'`).
2. **Clock integrity** (the regression this fix must NOT reintroduce):
   compare `kUptime` / the serial log's per-minute `[pool]` cadence against
   wall clock across the soak; drift should match the pre-fix tickless
   baseline, not the old full-pass-CLI bleed.
3. **Fossil check**: after the soak, pause via monitor and dump
   `temp_rsp[0..3]` (`x/8gx <temp_rsp addr from nm>`): task/idle frame bases
   only. Any `0xffff8000...` scheduler-stack VA in a slot means a window
   still exists — the fix failed. This is the sharpest single test we have.
4. Both modes boot; kernel suite + `/tests/testrun` green (24+35 at last
   count); tickless remains the default.
5. Optional but valuable: re-run the 8/9 hog #DF repro to confirm the hoist
   still holds with the new exit.

## What else this hunt caught (fixed, recorded here because the technique is the point)

**The 10-20 second whole-machine pauses were a SECOND bug, unrelated to
re-entrancy** — and the frozen-guest technique found it in one pass, from a
guest Chris paused mid-freeze. All four cores, at that instant:

| CPU | Where | IF |
|---|---|---|
| 0 | `mpAcctSettleAll` ack spin (smp_core.c) | **0** |
| 1 | `scheduler_do` spinning for `kSchedulerSwitchTasksLock` | 1 |
| 2 | `printd`, inside a scheduler pass, holding that lock | 1 |
| 3 | the **same** ack spin | **0** |

with `mp_acctSettleAck[0]` and `[3]` both false — each waiting for the other's
ack, neither able to take an interrupt. `mpAcctSettleAll` is called only from
procfs (i.e. inside a syscall, where SFMASK clears IF), so it was ALWAYS
waiting interrupts-off for an answer that only an interrupt could deliver.
Two `top` refreshes overlapping is all it takes. The machine recovered only
because the spin was bounded — the bound was the sole thing between this and
a dead OS. Fixed by not waiting when we cannot receive (full argument in the
code comment at the fix site); soak-confirmed the same day.

Two lessons worth keeping: **never wait for an interrupt-delivered answer with
interrupts off**, and CPU2's position is a live sighting of the already-booked
"move scheduler printds outside `kSchedulerSwitchTasksLock`" debt — a slow
call inside that lock stops scheduling machine-wide.

## Open items this does NOT close

- The 45-second polled-UART core stall (serial.c:49 busy-wait draining the
  logd queue at 115200 baud) — separate real bug, on the board.
- The AP preemption backstop (tickless starvation debt) — this fix is its
  prerequisite; the backstop adds exactly the AP interrupt traffic these
  windows fed on.
- The honeypot at the old CLS kmalloc site (`kDebugHoneypot`) — allocated
  but its sentinel fill is wrong (`memset` truncates the pattern to one
  byte, 0x04) and it has no checker. With CLS exonerated it is lower
  priority; either finish it (uint64 sentinel loop + kworker sweep) or
  remove it before it fossilizes.
