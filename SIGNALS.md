# SIGNALS.md — delivering signals to ring 3 (design)

*2026-08-23. The design conversation, recorded before the writing — the
discipline MALLOC.md and SIGINT.md set. NOT BUILT YET. Chris ruled the shape
at the keyboard; this file exists so the understanding survives the night it
was had, and so the decisions below are not re-litigated by whoever builds the
next piece.*

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

## The problem

**Ring 3 cannot install a handler.** `signals_t` has a `sighandler[32]` array
and a `sigaction()` function, but nothing in the syscall table reaches them,
so every signal's behaviour is the kernel's default and the default for the
interesting ones is death.

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
calls it. It takes the frame's address, validates that it lies within the
calling thread's own stack, restores the saved registers, unblocks the signal
that was being handled, and returns to the interrupted context.

**The validation is not optional.** `sigreturn` is a "restore arbitrary
register state" primitive, and it is reached from ring 3. It restores only a
frame it can prove is the one it wrote, and it never restores a privileged
selector or flags field from user memory.

### 7. Masking, kept to the minimum that works

While a handler runs, **its own signal is blocked**. Nothing else is.

That single rule is what stops a `SIGSEGV` handler that itself faults from
re-entering forever — the failure mode Chris's os32 test app could have hit and
never did, which is the usual way one learns this exists. There is no
`sigprocmask` in v1: DEBTS ratified "os64's answer to sigaction, without
POSIX's restart/mask warts", and a general mask API is a separate slice with
its own consumer.

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

Everything except `SIGKILL`.

`SIGSEGV` included — that is the acceptance test — with one honest limit: the
frame goes on the user stack, so if the *stack itself* is what faulted, there
is nowhere to put it. Delivery fails and the thread dies exactly as it does
today (exit 139). The cure is an alternate signal stack, and it is a later
slice; naming the limit is not the same as pretending it isn't there.

## Failure modes to design against

| Symptom | Cause to look for |
|---|---|
| A handler runs N times for one `SIGTERM` | Handler table left on the thread, or the delivery rule not clearing the bit task-wide |
| A `SIGSEGV` handler faults and the machine spins | The signal not blocked during its own handler (§7) |
| `sigreturn` resumes with impossible register state | The frame not validated as inside the caller's own stack (§6) |
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

   **Registration works; delivery does not exist yet, and that is deliberate.**
   Today an installed handler means exactly "do not apply the default action"
   — which `scheduler.c`'s forced-syscall push has honoured for `SIGINT` since
   before there was any way to install one. A program can be written against
   this interface now and will start actually running its handler when step 3
   lands, without the interface changing under it.
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

   NOT YET, and named rather than implied: a program that never makes a
   syscall gets no delivery. `scheduler.c`'s forced-syscall push already
   solves that shape for termination and is the obvious home for it — until
   then a spinning program with a handler installed is reachable only by
   SIGKILL. Blocking calls also do not yet return INTERRUPTED (§8); they
   deliver on the way out of whatever they return, which is right, but they
   still take the default action rather than reporting the interruption.
4. **Teach the nine checkpoints** to look for a handler before defaulting to
   death.
5. **The fixture**: raise `SIGSEGV`, catch it, print, die — Chris's os32 test
   app reborn as `/bin/sigtest`, in the ring-3 suite.

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
