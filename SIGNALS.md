# SIGNALS.md — delivering signals to ring 3 (design, and the record of building it)

*2026-08-23. The design conversation, recorded before the writing — the
discipline MALLOC.md and SIGINT.md set. Chris ruled the shape at the keyboard;
this file exists so the understanding survives the night it was had, and so
the decisions below are not re-litigated by whoever builds the next piece.*

*STATUS (2026-08-25): BUILT. Registration, and all three delivery paths — §5
the syscall exit, §10 the scheduler's visit to a spinner, §9 the page-fault
handler — shipped 2026-08-23/24 and went through the Codex #29 gauntlet. The
design sections below are kept as written, because the reasoning is the
record; where the building changed the shape, the "as built" notes say so in
place. "Size and order" at the bottom is the ledger of what is done. (The
header said NOT BUILT YET until rd19 pointed out it had been false for two
days.)*

*Prompted by four customers arriving one after another and getting the same
answer. `SIGPIPE`: a program that wants to survive a vanishing reader cannot.
`SIGTERM`: Chris asked how to subscribe the day the shutdown ladder was built,
and the honest answer was "you can't". `SIGHUP`: a GUI app cannot say "wait —
unsaved work" when its terminal hangs up. And `SIGWINCH`, which does not exist
yet and has nowhere to be delivered even if it did — which is why a maximized
gterm still reports 38x100 and `ls` still double-spaces.*

## What already exists, and is already right

More than half of this is built. The parts below are NOT up for redesign; they
are the foundation the rest stands on.

| Piece | Where | State |
|---|---|---|
| A pending-signal set per thread | `thread_t.signals.sigind` | built |
| Aiming a signal at a whole task | `task_signal_all_threads` | built |
| The same, plus a scheduling IPI for a remote sender | `task_signal_and_nudge` | built |
| Default action = death, enforced by the kernel | `raise_terminating_signal_and_die` | built |
| Nine checkpoints where a parked thread notices | see below | built |
| `128 + signo` exit encoding | `SIGNALS_EXIT_*` | built |
| A per-task, ring-3-visible, read-only **executable page** | `TASK_EXIT_TRAMPOLINE_VIRT` | built |

**The nine checkpoints**, because everything here hangs off them — these are
every place a thread can be parked or looping, found the hard way by the
terminating-signal work:

| Site | What parks there |
|---|---|
| `syscall.c` dispatcher | every syscall return |
| `syscall.c` sleep | `sleep` |
| `console.c` | `console_read` |
| `pipe.c` ×2 | `pipe_read`, `pipe_write` |
| `gui_client.c` | `gui_event_wait` |
| `thread_join.c` | join |
| `signals.c` | the sleep sweep |
| `scheduler.c` | the forced-syscall push (a ring-3 spin loop's only boundary) |

Each asks exactly one question today — *"do I have a pending terminate?"* —
and answers it by dying, **in the victim's own context**, which is what makes
`task_exit` safe there. That property is load-bearing and does not change.

## The problem (as it stood on 2026-08-23 — the premise the design answers; see STATUS above)

**Ring 3 could not install a handler** (the state this design set out to
change; it can since 2026-08-23). `signals_t` had a `sighandler[32]` array
and a `sigaction()` function, but nothing in the syscall table reached them,
so every signal's behaviour was the kernel's default and the default for the
interesting ones was death.

And the array cannot be used as-is. Its own header says so:

> CAUTION, `sighandler[]` / `sigdata[]`: both are indexed by the signal's BIT
> VALUE, not its bit NUMBER — `sigdata[SIGSLEEP]` is `sigdata[2]`. That works
> only for bits 0..4; `SIGSTOP` (32) and everything above it would index past
> the end of a 32-entry array.

Twelve signals are defined; eight of them are above bit 4. A ring-3 API that
says "install a handler for signal X" walks straight into this.

## The design

### 1. Identity is a NUMBER; the pending set is a bitmask of those numbers

Every signal gets a number. `SIGHUP = 1`, `SIGINT = 2`, `SIGKILL = 9`,
`SIGSEGV = 11`, `SIGTERM = 15` — the POSIX numbers, kept **on merit**: a
corpse tagged 143 is legible to anyone who has ever read a shell's exit
status, and `SIGNALS_EXIT_SIGTERM 143` already encodes 128+15, so the numbers
are half-adopted already. This finishes that job rather than inventing a
second numbering beside it.

The pending set stays a bitmask, because a set is what it is — but the bit is
now derived, `1 << signo`, instead of being the identity. The handler table is
indexed **by number**, which is what kills the landmine above: twelve signals
need twelve slots, not a sparse array addressed by `1 << 12`.

This is the one change with no user-visible behaviour at all, and it is the
prerequisite for everything after it.

### 2. Three things, three homes: AIM, PENDING, HANDLER

This is the heart of the design and the part most likely to be got wrong by
someone reasoning from POSIX alone.

- **AIM — per TASK.** A signal aimed at a program means the program, all of
  it. `task_signal_all_threads` ORs the bit into every thread. *This is
  already built, and it was earned:* every delivery site used to write
  `task->threads->signals.sigind |= sig` — the FIRST thread — which was
  complete when a task had one thread and became a silent half-measure the day
  it could have several. On 2026-08-02 `echo kill > /proc/N/ctl` retired the
  main thread and left four workers burning four cores, audible as fan noise.
  The workers were never ignoring the signal; nobody had told them.

- **PENDING — per THREAD.** `thread_t.signals.sigind`, as today. A thread
  parks and wakes on its own pending set, and a fault-derived signal
  (`SIGSEGV`) is inherently the faulting thread's business.

- **HANDLER — per TASK.** ← the only new home. `sighandler[]` moves from
  `thread_t.signals` to `task_t`.

**Why the handler is per-task, decided by the scar above.** Because the aim is
already a broadcast, per-thread handlers would run one `SIGTERM` **N times** —
once per thread — and "save my unsaved work" firing four times in a
four-threaded app is not a policy anyone wants. Per-task handlers also answer
three questions that per-thread handlers only raise: a thread created after
registration has the handler automatically (per-thread, it would have none,
and behaviour would depend on which thread happened to notice); there is no
inheritance rule to invent (copy-at-create goes stale, sharing IS per-task with
extra steps); and a handler is, plainly, a property of the PROGRAM.

The cost is one rule, stated next.

### 3. The delivery rule: one signal, one handler run

**The first thread to reach a checkpoint with a pending, handled signal runs
the handler and clears that bit on every thread of the task.**

Thread-aimed signals cost nothing under this rule — only the faulting thread
ever had the bit, so "clear it everywhere" clears it in one place.

### 4. Registration

One syscall, `sigaction`-shaped and no more: given a signal number and a
handler address, install it; given a NULL handler, restore the default. It
answers with the previous handler, because "install mine, remember theirs" is
how a library that must not stomp its host behaves, and that is worth having
from the first day rather than retrofitting.

**`SIGKILL` is refused, loudly.** It is the one signal that must always work,
because it is the answer to a program that has stopped answering. A kernel
that let a program decline to die has no last resort.

### 5. Delivery: the frame, the redirect, and the stub

At a checkpoint, with a pending signal that has a handler:

1. Save the interrupted register state into a **signal frame pushed onto the
   thread's USER stack** (written through the HHDM alias of the stack's
   physical page — the user stack VA is mapped only in the task's own PML4,
   the same idiom `task_setup_ring3_exit_path` already uses to seed a return
   address).
2. Set the thread's return-to-user `RIP` to the handler, `RDI` to the signal
   number (the SysV first argument — a handler is an ordinary C function).
3. Set the return address the handler will `ret` to: a **stub in the existing
   trampoline page**, which invokes `sigreturn`.
4. Return to user. The handler runs, on the thread's own stack, at CPL 3.

**THE TRAMPOLINE MUST BE A SEPARATE EXECUTABLE PAGE, and os64 already has
one.** Historic Unix pushed the return stub onto the *stack* and jumped to it
— which requires an executable stack, and is exactly what os64's NX arc
outlawed (`nx_test` asserts that executing the stack kills the program).
Linux carried the stack trampoline for years and then moved it into the vDSO
for the same reason. os64 skips that whole era: `TASK_EXIT_TRAMPOLINE_VIRT` is
already a per-task page, mapped `PAGE_USER` without `PAGE_WRITE` and
executable, holding a code template copied in at task creation. The signal
stub is a second template in the same page — read-only to the program that
runs it, which is precisely the property a return path wants.

### 6. `sigreturn`

A syscall a program never calls deliberately: it exists because the stub
calls it. It takes the frame's address, validates it, restores the saved
registers, unblocks the signal that was being handled, and returns to the
interrupted context.

**The validation is not optional.** `sigreturn` is a "restore arbitrary
register state" primitive, and it is reached from ring 3. The frame must
carry the kernel's magic, and a handler for the frame's signal must actually
be running on the calling thread — the check that makes the call useless to
anyone who did not arrive the intended way.

**RFLAGS is SANITIZED, never trusted** (amended 2026-08-24, review). The
design as first written claimed "the frame we wrote" as a defence, and the
claim does not survive contact with where the frame lives: the user's own
writable stack, every word of it ring 3's to forge. `sysretq` loads RFLAGS
from R11 nearly verbatim — IF and IOPL included — so a forged IF=0 parks a
core beyond the timer's reach forever, and IOPL=3 hands ring 3 the I/O
ports. The frame's rflags therefore keeps only the bits a user program owns
(arithmetic flags, TF, DF, AC, ID) and the rest are forced: IF on, IOPL 0
(`SIGNAL_RFLAGS_*`, signals.h). A stack-range check on the frame POINTER —
which this section originally specified — is deliberately absent: it proves
nothing, because the frame's contents are user-writable wherever it sits.
The sanitization is the defence. §10's full-frame `sigreturn` inherits the
same mask with higher stakes: its road home is `iretq`, which swallows
RFLAGS whole and CS/SS besides — flags through this mask, selectors from
kernel constants, never from the frame.

### 7. Masking, kept to the minimum that works

While a handler runs, **its own signal is blocked**. Nothing else is.

That single rule is what stops a `SIGSEGV` handler that itself faults from
re-entering forever — the failure mode Chris's os32 test app could have hit and
never did, which is the usual way one learns this exists. There is no
`sigprocmask` in v1: DEBTS ratified "os64's answer to sigaction, without
POSIX's restart/mask warts", and a general mask API is a separate slice with
its own consumer.

**A blocked signal is HELD, not fatal** (amended 2026-08-24, review). The
same signal arriving again while its handler runs stays pending and delivers
at the dispatcher exit right after `sigreturn` unblocks it. The checkpoints
therefore count a masked-with-handler signal as *caught*
(`signal_has_handler_for_pending`) — the first implementation skipped masked
bits, answered "nothing will catch this", and the default action executed
the program in the middle of the very handler it had installed. One accepted
wart rides this: a handler that itself BLOCKS while its own signal is
pending again gets `INTERRUPTED` from every blocking call until it returns —
honest, rare, and strictly better than dying; the cure is a "deliverable
now" vs. "held" split at the checkpoints, booked in DEBTS.

os64 also skips the *unreliable-signal* era entirely, and should say so: V7's
`signal(2)` reset the disposition to the default **before** running the
handler, so a second signal arriving inside the handler killed you — a race
famous enough that 4.2BSD invented `sigvec` to fix it. Handlers here persist
across delivery. There is no reset-to-default and never was.

### 8. Interrupted calls return INTERRUPTED — no restart, no errno

A blocking call whose thread runs a handler **returns a distinct INTERRUPTED
result**, and the caller decides what to do about it.

This is not a new convention: `os64_gui_event_wait` already returns
`INTERRUPTED` when its caller is being killed, and libui already handles it
(`rc != 1` leaves the loop). The change generalizes an idiom that exists in
exactly one place today.

What os64 declines, deliberately: `SA_RESTART` and `EINTR`. POSIX's restart
flag exists because its authors could not decide, so they shipped both
behaviours and made every caller learn which one it had. A call that was
interrupted says so; a program that wants to retry writes a loop, which it can
read.

### 9. What is catchable

Every signal this kernel can SEND, except `SIGKILL`: `SIGHUP`, `SIGINT`,
`SIGSEGV`, `SIGPIPE`, `SIGTERM`. (The design said "everything except
SIGKILL" and meant it; rd14/15 narrowed it, because a number with no
producer — `SIGCONT`, `SIGSTOP`, `SIGIO` — is refused at registration with
`BAD_SIGNAL` rather than accepted for a signal nothing can raise. Each
rejoins the set the day something can send it. `signal_is_known` in
signals.c is the list, one producer named per entry.)

`SIGSEGV` included — that is the acceptance test — with one honest limit: the
frame goes on the user stack, so if the *stack itself* is what faulted, there
is nowhere to put it. Delivery fails and the thread dies exactly as it does
today (exit 139). The cure is an alternate signal stack, and it is a later
slice; naming the limit is not the same as pretending it isn't there.

**BUILT 2026-08-24** (`signal_deliver_segv`, called from the page-fault
handler's user-fatal path). It is a THIRD delivery site, distinct from §5 and
§10 because a fault is *synchronous and thread-local*: the target is the
faulting thread itself (no broadcast pending bit to set or consume — though
it DOES take `signalLock`, since rd8, for the lock's other job: keeping the
user page alive across the frame writes, see task.h), the interrupted state is
the exception frame (`exception_context_t`, built on the stack by
exception_entry.S), and the resume rides the exception's OWN `iretq` — which
restores the GP registers from the context and honours an edited `rip`/`rsp`
(the demand-pager already steers it that way). So delivery edits the context
to run the stub; no new resume path. `sigreturn`'s full-frame path takes the
handler home unchanged. Two properties earned in the building: a fault INSIDE
a SIGSEGV handler finds the signal masked (§7) and dies rather than looping
forever, and a handler that simply RETURNS resumes the faulting instruction
and faults again — so a real SIGSEGV handler exits or longjmps, which is what
`/bin/sigtest`'s finale does (fault on a NULL store, catch, brag, exit). The
default action is untouched for a task with no handler installed: `segv_test`
still dies with the full forensic dump and exit 139, and the OS survives.

## §10 — Delivering to a thread that never makes a syscall (design — BUILT 2026-08-24, see "§10 as built" below)

*Chris, 2026-08-23, after the syscall path shipped: "don't forget that the
APIC timer triggers eventually. So every AP ends up in scheduling. I know you
said you didn't want to do signal handling out of the scheduling ISR, but ...
🤷‍♂️ ... maybe?" He is right, and the reasoning below is why — recorded before
the building, like the rest of this file.*

A program that spins without ever entering the kernel gets nothing from §5:
delivery is armed on the way out of a syscall, and there is no syscall. Today
such a program can be TERMINATED (the forced-syscall push in `scheduler.c`
patches its RIP into the exit trampoline, which is how `echo kill >
/proc/N/ctl` reaches `hog`) but not SIGNALLED.

**The scheduler is the delivery point, not a forced syscall.** Every thread
reaches it: the BSP ticks at 100Hz, and on an AP under tickless the BACKSTOP
LEASE arms per non-idle dispatch — built precisely so a syscall-free hog
cannot hold a core forever. So the visit is guaranteed on every core, and it
is guaranteed at a place that already has everything delivery needs.

**Why that is easier than the syscall path, not harder:**

- `thread->regs` ALREADY holds the full user state, saved by the ISR. Nothing
  has to be captured; it is sitting there.
- Patching `regs.RDI` to the signal number is FREE, because the resume path
  reloads every register. The §5 stub has to fish the number off the stack
  precisely because `sysretq` does not restore RDI; here that problem does not
  exist.
- And `sigreturn` needs no new way home. Restoring a full register set through
  `sysretq` is impossible — but a thread resumed BY THE SCHEDULER already gets
  everything back from `thread->regs` via `iretq`, which is how every
  preempted thread in the system resumes. `sigreturn` writes the saved values
  into `regs` and lets the thread take that ordinary road.

**What it does need: the whole frame.** §5 saves four values (RAX, RIP, RSP,
RFLAGS) and gets away with it because the interruption point is a syscall
RETURN, where the ABI has already declared RCX/R11 dead and a C handler
preserves the callee-saved set itself. A spinning thread is interrupted at an
ARBITRARY instruction, so every register is live and a C handler will clobber
half of them without putting them back. This frame carries the general
register set.

Which is exactly the shape of Chris's os32 stack diagram, and the reason it
was long: he picked the general interruption point from the start. §5 is
short only because it picked the easy one.

**Sketch, to be argued with when it is built:**

1. In the scheduler, where `scheduler_sigint_forced_syscall` already stands:
   if a pending signal is catchable and handled, build a FULL frame on the
   user stack (through the HHDM, per §5) from `thread->regs`.
2. Patch `regs.RIP` to the stub, `regs.RSP` to the frame, `regs.RDI` to the
   signal number. Mark the frame as full-register, so `sigreturn` knows which
   kind it is holding.
3. The thread resumes by `iretq` into the stub, which calls the handler.
4. `sigreturn` sees a full frame, writes every saved register back into
   `thread->regs`, and arranges to resume through the scheduler rather than
   `sysretq`.

**Open questions worth settling before writing code**, rather than during:
the exact resume mechanism in step 4 (yield-and-be-reloaded is the obvious
candidate and wants checking against the reentrancy rules in
SCHEDULER_REENTRANCY.md); whether the forced-syscall push should stay for
TERMINATION once the scheduler can deliver (probably yes — it is simpler and
it is proven); and whether one frame type with a flag beats two, given that
`sigreturn` must never be talked into restoring a register set the kernel did
not write.

### §10 as built (2026-08-24) — every open question answered before the code

**The full frame is PREFIX-COMPATIBLE with the §5 frame, and that is what
makes the stub free.** `signal_frame_full_t` begins with `signal_frame_t`
byte-for-byte (magic, rax, rip, rsp, rflags, signo, handler, pad) and hangs
the fourteen remaining GPRs off the end. The stub reads signo at +40 and the
handler at +48 exactly as before, so ONE stub serves both delivery paths and
task_exit_asm.S does not change at all. The discriminator is a SECOND MAGIC
("SIGRFRM2"), not a flag field — a flag inside user-writable memory is the
attacker's to flip, and flipping it would upgrade a 4-value restore into a
full-file restore. Two magics mean forging the wrong one buys the validation
of the frame you forged, never the other one's.

**No FXSAVE area, and the reason is the build system.** Userland is compiled
`-mno-mmx -mno-sse -mno-sse2` (userland/GNUmakefile CFLAGS), so an
arbitrary interruption point has NO live vector state to preserve — the
question that forced Linux's interrupt-delivered frame to carry an fpstate
simply does not arise. THE FRAME'S COMPLETENESS IS PREDICATED ON THOSE
FLAGS: the day userland grows SSE, this frame grows a 512-byte FXSAVE area
or float code corrupts across delivery, quietly, weekly.

**Delivery lives where the forced push lives**, in
`scheduler_signal_visit` (né `scheduler_sigint_forced_syscall`), called at
both dispatch sites — the continue path and the switch path — behind the
same `(regs.CS & 3) == 3` seatbelt: interrupted in ring 3, holding no
kernel locks. The visit now asks two questions in order: *will something
catch a pending signal?* → build the full frame from `thread->regs`
(through the HHDM, per §5), point regs.RIP at the stub and regs.RSP at the
frame, clear DF in regs.RFLAGS (rd10), and mirror all three into the
per-core isr arrays (both images, exactly as the forced push always did —
RIP, RSP and RFLAGS are the only registers delivery changes, because the
stub takes everything else from the frame; this sentence said "RIP and RSP"
until rd25, and rd11 had already found that a mirror missing the third made
the DF clear a no-op on the continue path); *otherwise, is a terminate
pending?* → the gallows — asked only when delivery did not FAIL (rd23). A
FAILED frame write on a terminating signal, or an orphaned death (rd18),
falls through to the gallows (death must not depend on
the victim's stack — the same reason the push survives at all); on a
non-terminating one the signal is dropped with a log line.

**`sigreturn`'s full path never returns, and that is the resume mechanism.**
It validates (magic2, signo, the running-handler check), sanitizes RFLAGS
through the §6 mask, writes the full register file into `thread->regs` —
selectors from GDT constants, NEVER from the frame — sets
`execDontSaveRegisters` (exec's own crafted-regs seam: the next store pass
skips saving, so the crafted context survives), and parks through the
ordinary SIGSLEEP machinery with a wake tick of NOW. The kernel
continuation is abandoned mid-syscall — its stack is forgotten, exactly as
exec forgets one — the sleep sweep wakes the thread immediately, and the
next dispatch loads the crafted regs through `scheduler_load_thread` and
resumes the interrupted spin by `iretq`, the road every preempted thread
already takes. Not returning is load-bearing twice over: there is no
sysretq that could restore fifteen registers, and a returning sigreturn
would run the dispatcher-exit delivery hook against a continuation the
scheduler is about to discard.

## Failure modes to design against

| Symptom | Cause to look for |
|---|---|
| A handler runs N times for one `SIGTERM` | Handler table left on the thread, or the delivery rule not clearing the bit task-wide |
| A `SIGSEGV` handler faults and the machine spins | The signal not blocked during its own handler (§7) |
| `sigreturn` resumes with impossible register state | The RFLAGS mask (§6) not applied, or the running-handler check bypassed |
| A DIFFERENT program dies or runs wild after a signal is delivered | The syscall return-frame pointer read from somewhere per-CORE. It is per-THREAD (thread.h) because a blocking syscall parks with its frame live — found in review 2026-08-24, one day after the field shipped in CLS, the same disease as the `cls->task` staleness this arc fixed |
| A second Ctrl+C during a handler kills the program | A masked pending signal answered "nothing will catch this" at a kill checkpoint. Masked-with-handler counts as CAUGHT (`signal_has_handler_for_pending`) — the held bit delivers right after `sigreturn` unmasks |
| Every blocking call returns INTERRUPTED forever, handler never runs | Delivery kept failing (unusable stack) with nobody applying the default. `SIGNAL_DELIVER_FAILED` exists so the dispatcher kills instead of shrugging |
| A signal delivered to a thread that then exits | Deliver at the checkpoint, in the victim's own context — the same property `raise_terminating_signal_and_die` depends on |
| Handler never runs on a spinning ring-3 program | The forced-syscall push in `scheduler.c` is its only boundary; delivery must ride that checkpoint like termination does |
| An app "catches" `SIGKILL` | Registration must refuse it (§4) |

## Size and order

M, and it comes apart cleanly. Each step is useful on its own and green before
the next.

1. ~~**Numbers, and handlers to `task_t`.**~~ **DONE 2026-08-23.** No
   behaviour change; the bit-value-indexing landmine is gone. The pending set
   became a struct (`signal_set_t`) so the compiler found all 31 conversion
   sites itself — a renumbering that stayed silent at even one of them would
   have been a bug asleep in the tree. Two neighbours came out of it: a
   pre-existing double-close race in `handle_close` (claim-then-act now), and
   `asm-offsets.h` never depending on the structs it measures.
   `sigaction` was renamed `signal_raise`, which is what it always did.
2. ~~**Registration syscall.**~~ **DONE 2026-08-23** —
   `SYSCALL_SIGNAL_HANDLER` (49), `os64_signal_set_handler` in
   `abi/os64/signal.h`. Install / restore-default / report-previous; `SIGKILL`
   refused by name, out-of-range refused, a higher-half handler address
   refused. The ABI's numbers are static-asserted against the kernel enum
   (klog_format.h's discipline), so the two rings cannot drift.
   `/bin/sigtest` is the fixture, in the ring-3 suite.

   **HISTORICAL — this was the state for a few hours on 2026-08-23, and it
   is kept because the bet it describes was won.** Registration shipped
   BEFORE delivery, deliberately: for that window an installed handler meant
   exactly "do not apply the default action" — which `scheduler.c`'s
   forced-syscall push had honoured for `SIGINT` since before there was any
   way to install one. The bet was that a program written against the
   interface then would start actually running its handler the moment step 3
   landed, without the interface changing under it. It did, the same day.
   TODAY a handler RUNS — by all three delivery paths (§5 syscall exit, §10
   scheduler visit, §9 page fault), and a signal with no producer is refused
   at registration rather than accepted on the same bet (the rd14 ruling in
   `signal_is_known`). (Codex #29 rd16 read this paragraph as current
   behaviour; it was the only way to read it.)
3. ~~**Delivery + the stub + `sigreturn`.**~~ **DONE 2026-08-23 for the
   syscall path**, which is the common case: a handler runs, the program
   resumes, and the interrupted syscall's return value survives.

   Two findings changed the shape from what §5 above describes, both for the
   better:

   - **Delivery happens ONCE, at the syscall dispatcher's EXIT — not at the
     nine checkpoints.** The checkpoints exist so a PARKED thread notices a
     terminate in its own context, and they still do; but every one of them
     returns through the dispatcher, so that is the single place a handler
     needs arming. Arming it on the way IN would have been wrong for a
     different reason: the syscall would be skipped entirely and then resumed
     as though it had happened — a `read` that silently never read.
   - **The saved frame is FOUR values, not a register file.** Because the
     interrupted context is a syscall return, the syscall ABI has already
     declared RCX and R11 clobbered, the entry stub preserves the callee-saved
     set, and a handler obeying the C ABI preserves those itself. What is left
     is RAX (the syscall's own answer), RIP, RSP and RFLAGS. Delivering from
     an interrupt — as os32 did — would have needed all of them.

   The stub CALLs the handler and falls through into `sigreturn`, rather than
   being returned INTO: that puts the signal number in RDI (which the syscall
   return path does not restore) without the kernel having to touch a register
   the frame does not carry.

   **`sleep` reports `OS64_INTERRUPTED`** (§8) — the first blocking call to
   do so, and the one the demo needs: a parked thread must RETURN for the
   dispatcher to arm anything, so a park that kept re-parking would hold the
   signal forever and never reach delivery. The nap is not resumed and the
   remaining time is not slept; a caller who wanted the whole nap loops.

   ~~NOT YET, and named rather than implied: a program that never makes a
   syscall gets no delivery.~~ **PAID 2026-08-24 — §10 is BUILT** (see "§10
   as built" below): the scheduler's visit delivers the full-register frame
   to a thread caught spinning in ring 3, the same stub serves it, and
   `sigreturn`'s full path resumes the spin through `thread->regs` and
   `iretq`. `/bin/sigspin` is the demo — a loop with no syscalls in it that
   catches your Ctrl+C anyway. The forced push survives as the gallows for
   the uncaught, exactly as designed.

   **EVERY OTHER BLOCKING CALL REPORTS `OS64_INTERRUPTED` NOW** (§8, done
   2026-08-23): `console_read`, both pipe ends, the TCP/ICMP/UDP readers and
   `thread_join`. Eight sites, one shape, one helper —
   `current_thread_will_catch()`, which takes the task from the THREAD by
   construction so the staleness below can never reach a decision again.

4b. **The demo.** `/bin/sigdemo` — a countdown you interrupt with your own
   hands, which says it was interrupted and carries on counting. It exists
   because a fixture reports a verdict and a demo shows a behaviour, and
   "sigtest exited 85327872" is the former pretending to be the latter
   (Chris, on being shown the fixture: "pretty anticlimactic!"). Direct
   descendant of his os32 test app.
4. **Teach the nine checkpoints** to look for a handler before defaulting to
   death.
5. ~~**The fixture**: raise `SIGSEGV`, catch it, print, die~~ **DONE
   2026-08-24** — Chris's os32 test app reborn as `/bin/sigtest`'s finale, and
   the SIGSEGV delivery it needed (§9, "BUILT") built with it. Raised by Codex
   #29 round 3: the API advertised SIGSEGV as catchable while every fault
   killed unconditionally — the same lie the SIGPIPE fix closed, and closed
   the same way (deliver, don't reject).

## Explicitly not in scope

- **`sigaltstack`** — an alternate stack for the case where the stack is what
  faulted. Named in §9; nothing here prevents it.
- **`sigprocmask`** and general blocking sets — §7.
- **Queued signals with payloads** (`sigqueue`/`siginfo`). The pending set is
  a bitmask: two `SIGTERM`s before delivery are one `SIGTERM`. That is
  classic Unix behaviour and is not a bug.
- **Job control** — `SIGTSTP`/`SIGCONT`, `fg`/`bg`. Its own DEBTS row, and it
  wants the debugger's `ctl stop`/`start` built with it.
- **`SIGWINCH` itself.** This slice builds the road; the terminal-resize row
  (DEBTS) drives on it, along with gterm resizing its pty grid.
- **Signal-safe library rules.** Which libos64 functions may be called from a
  handler is a real question with a real answer, and it deserves its own pass
  once there is a handler to call them from.
