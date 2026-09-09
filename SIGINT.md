# SIGINT.md — Ctrl+C, the foreground task, and async termination (design)

*2026-07-19. The design conversation, recorded before the writing — same
discipline as MALLOC.md. NOT built yet. Chris ratifies at the keyboard; this
file exists so the understanding survives the night it was had. Prompted by a
real event: `cat pattern.bin` (a 1.5 MB file) scrolled the P5's console for an
hour with no way to stop it. The scrolling was welcome proof a core had been
faithfully working for an hour — but a user wants an off switch.*

## The problem, precisely

There is no way to interrupt a running foreground program. `cat` writing a
huge file spins in a `write` loop; nothing short of reboot ends it. Ctrl+C is
the fix, and Ctrl+C is routed through **signals** — which requires a concept
os64 does not yet have: **the foreground task** (the one task the console
belongs to *right now*). Get "which task is foreground" wrong and you signal
the wrong program — in os32, with 7–8 virtual consoles, that meant killing the
app in someone else's terminal. We build the concept correctly now so virtual
terminals (NOT tonight) inherit a sound foundation instead of a retrofit.

## The concept: the foreground task is one pointer (v1)

`kForegroundTask` — a single `task_t *`, because v1 has one console. It becomes
a per-`tty_t` field the day virtual terminals arrive, exactly as `kConsoleWaiter`
and `kConsoleEOFPending` in console.c are already commented to become.

**How it moves — and the point is it needs ZERO husk changes.** The foreground
task is, by definition, *"the task the controlling shell is currently blocked
waiting on."* So the transfer rides `task_wait`, which husk already calls:

- Kernel launches husk (kernel.c) → `kForegroundTask = husk`, and husk is
  tagged the **controlling shell** (see shell protection below).
- husk calls `task_wait(child)` → `kForegroundTask = child`.
- child exits, husk's wait returns → `kForegroundTask = husk`.

husk learns no new trick. Keying the transfer on **wait**, not **spawn**, is
also future-proof: when husk grows `&`, a backgrounded child is spawned but not
waited, so it correctly never becomes foreground. (`&` reached the Thompson
shell in 1973; the seam is already shaped for it.)

## The three moving parts

### 1. Intercept the interrupt character (the line-discipline job)

Ctrl+C already translates to `0x03` = **ETX, "End of Text"** — and that is not
a coincidence to route around but a lineage to honor: it is the same well
`Ctrl+D → 0x04 → EOT` drinks from (see console.c's CONSOLE_EOT comment). The
tty's *interrupt* character has been ETX since the DEC line disciplines; Unix's
`ISIG`/`VINTR` machinery is what turns that keystroke into a signal.

So instead of letting `0x03` fall into the console ring as a data byte, a small
hook in **console.c** (called from `keyboard_deliver_event`'s key-down path in
keyboard.c — the "delivery choke" the comment there already names) catches ETX
and raises `SIGINT` on `kForegroundTask`, at **input time**.

This is the crux of *why* it must be a signal and not a console byte: **cat is
not reading the console** — it is *writing a file to it* — so nothing is
draining that ring. A `0x03` buffered there would sit unread forever. The
interrupt has to be delivered asynchronously, at the keystroke, not at a read
that never comes.

Layering note: keyboard.c is the device layer and must not include task/signal
headers; the intr-character *policy* lives in console.c (the tty seed), which
keyboard.c calls. Setting a pending signal bit (`sigind |= SIGINT`) is a single
word-OR — safe to do from the PS/2 IRQ path; the actual kill happens later
(part 3), not in the IRQ.

### 2. SIGINT default action = terminate

Exactly the `SIGPIPE` model already in the tree (`raise_sigpipe_and_die`,
syscall.c): ring 3 cannot install signal handlers yet — that is the ratified
"userland signal delivery" DEBT — so the **kernel enforces the default action**.
For SIGINT the default is terminate. The dead task's `retVal = 130` (128 +
SIGINT), the classic "died by signal" encoding, matching SIGPIPE's 141.

### 3. Shell protection

`kForegroundTask == controlling shell` (husk sitting at its prompt) → SIGINT is
**ignored** in v1. Ctrl+C at an idle prompt must never kill your shell. When
real userland signal delivery lands (the DEBT), husk will instead *catch*
SIGINT to abort a half-typed input line and redraw its prompt — the way a real
terminal does. This design hands off to that future cleanly: it does not
foreclose it, it just supplies the safe default until then.

## The requirement that is NOT obvious — and is the interesting one

`raise_sigpipe_and_die` had it easy: the dying task is the one that *called*
`write`, so it is "current" and calls `task_exit()` on its own stack. **Ctrl+C
cannot do that.** When you hit it, `cat` is spinning **on another core**, and
the keyboard IRQ cannot reach across and call `task_exit` on it. `task_exit`'s
entire complexity (the RSP/CR3 switch, the `noinline` continuation — see
task.c and CLAUDE.md's context-switch section) exists because it is sawing off
the branch it stands on. You cannot perform that suicide dance *for* a task
running elsewhere.

So the kill cannot happen at keystroke time. It happens at a **checkpoint every
core already hits ~100×/second**: the per-core point where the scheduler is
about to *resume* a chosen thread (`scheduler_run_new_thread`, scheduler.c:707
— confirm the exact insertion point at build time; the resume/return-to-user
edge is the target). At that instant the doomed thread is *not* executing — its
state is saved, the core is in its tick handler — so the scheduler can check
"does the thread I am about to resume have a pending terminate?" and, if so,
reap it and pick idle instead. `cat` never runs again. Latency ≤ 10 ms after
your thumb — which is exactly what Ctrl+C should feel like.

**The pleasing inversion:** killing another task this way is *easier* than
`task_exit` kills itself. We stand on the scheduler's stack, not cat's — there
is no self-immolation problem, no RSP/CR3 gymnastics. We simply run the
bookkeeping `task_exit_finish` already does — `handle_close_all` (hands back
cat's pipe/file ends so no reader hangs on an EOF that never comes),
`task_enqueue_dead_child` (which wakes husk's `task_wait`), mark zombie — *for*
a thread instead of *as* it. `task_exit` is hard precisely because it is
suicide; an async reap is not.

**The one caution this raises:** a task may currently be RUNNING on another core
the instant Ctrl+C is pressed. You may NOT reap a thread mid-execution on
another core — it is using its own stack/state there. The reap is safe only at
the moment that core is about to resume it (its own next tick). That is why the
checkpoint is per-core and rides each core's timer, not a single global sweep.

## OPEN questions (Chris, at the keyboard)

- **Where exactly is the resume checkpoint?** `scheduler_run_new_thread` is the
  candidate; verify it is the point reached on EVERY resume-to-user on EVERY
  core, and that inserting a "reap instead of resume" branch there is clean.
- **How is the controlling shell tagged?** A `task_t` flag
  (`TASK_FLAG_CONTROLLING_SHELL` / `TASK_FLAG_SIGINT_IMMUNE`), set by the kernel
  when it launches husk (kernel.c)? A dedicated `kControllingShell` pointer? The
  flag generalizes better to per-tty later.
- **Should `task_wait` set/restore `kForegroundTask`, or should a tiny explicit
  hook do it?** Wait-follows needs no husk change and is correct for v1;
  confirm husk's wait shape matches the assumption (kernel.c calls it "husk
  loops forever on read/spawn/wait").
- **What does Ctrl+C at the husk prompt DO in v1** beyond "not kill husk"?
  Nothing? Discard queued console input + echo `^C\n`? (Real terminals echo and
  redraw.) The richer behavior wants the userland-signal DEBT resolved first.
- **Multi-thread tasks:** SIGINT targets `kForegroundTask->threads` (first
  thread) for now. When a foreground task has several threads, which get the
  terminate — all of them? (Deferred until a multi-threaded foreground app
  exists; note it so nobody assumes.)

## Ties and precedents

- Mirrors `SIGPIPE`/`raise_sigpipe_and_die` (syscall.c) — same "kernel enforces
  the default because ring 3 can't catch it" pattern, same 128+signo retVal.
- Hands off to the **userland signal delivery DEBT** (DEBTS.md): once ring 3 can
  install handlers, husk catches SIGINT itself and the kernel default steps back.
- The intr-character interception is the seed of a tty **line discipline**
  (`ISIG`/`VINTR`); virtual terminals will multiply the single `kForegroundTask`
  / controlling-shell state into per-`tty_t` fields, not rewrite it.

## Prior art — os32 (an example to learn from, NOT the roadmap)

os64's ancestor (`~/src/os`: `kproj/chrisOSKernel` kernel, `aproj/libChrisOS`
library, `aproj/kshell` shell) shipped this exact feature — virtual consoles
with a foreground app — and shipped it *well*. Read it for ideas, not orders.
Chris's framing, explicit: this is a side street os64 may take, or a road it may
leave entirely; the divergence is open on purpose. "I really liked the way I did
virtual terminals — but it's an example, not necessarily the roadmap." We keep
what was right and are free to do better where os64 can.

**What os32 actually did:**

- **`foreground` was a per-process bool** (`process.h`), not a global. os64's
  single `kForegroundTask` pointer is that model *collapsed to one console*. The
  "becomes a per-`tty_t` field later" note above is therefore not speculation —
  it is os64 re-expanding back toward the shape os32 already had. The ancestor
  is the reference for the multi-console end state.
- **Virtual consoles via `Ctrl+Alt+#`**, kernel on system console 0
  (`drivers/terminal/termdrv.c`).
- **The idea worth stealing — the terminal IS a pipe.** The terminal driver
  gates output by `device->stdOutWritePipe == activeTTY->stdOutWritePipe`: only
  the pipe belonging to the console that currently has focus is drained to the
  screen. Background consoles keep running, writing into pipes that simply
  aren't shown. This dovetails cleanly with os64's one-handle-type model
  (a tty is a handle; focus decides which handle's output reaches glass) — a
  strong candidate to carry forward, if virtual terminals are ever built.
- **The signal struct os64 still uses is os32's** — `sighandler[]`, `sigdata[]`,
  `sigmask`, `sigind`; SIGINT delivered by `sigind |= SIGINT`. os64 inherited it
  wholesale.
- **CORRECTION (2026-07-24, after actually reading the ancestor): os32 had the
  ENTIRE signal machine, working.** An earlier revision of this section claimed
  the delivery half "was never built" based on one vestigial stub
  (`executeSigHandler`) — wrong. The real path: `modifySignal()` →
  `SYSCALL_SETSIGACTION` (userland registration), `signalTask()` →
  `SYSCALL_SIGNAL` (a kill(2) equivalent os64 does not have yet), the
  `sigProcAddress` resume trampoline in `_scheduler.s` (per-APIC-indexed —
  SMP-aware delivery state), and `_sigJumpPoint`: a COMPLETE hand-rolled
  sigreturn — pusha, call the handler as a plain function, restore the
  original CR3 the trampoline stashed on the stack, popa, iretd BACK TO THE
  INTERRUPTED INSTRUCTION. Deliver, run, resume. No sigreturn syscall needed;
  the return rides the IRET frame the scheduler pre-built. PROVEN by
  `aproj/testMainProgramEntry` (May 2016): registers HandleSEGV, deliberately
  writes to unowned memory, and the ring-3 handler catches the fault and
  exits with its own code. os64's "userland signal delivery" DEBT is therefore
  a 64-bit SMP-hardened PORT of working family machinery, not an invention —
  modifySignal → registration syscall, sigProcAddress → per-thread pending
  redirect, _sigJumpPoint → the sigreturn stub. 64-bit amendments the port
  must add: the ring-3-only redirect guard (never abandon an in-flight
  syscall's locks), per-thread (not per-core-global) delivery state, and the
  SysV x86-64 RED ZONE — the kernel must build the signal frame at least 128
  bytes below the interrupted RSP or it corrupts leaf-function locals (a
  hazard 32-bit os32 never had; its ABI has no red zone).

**Where os64 may deliberately diverge:** the no-Linux-cosplay ethos may reshape
what a tty even is; async delivery gets built fresh against real SMP (the
per-core resume checkpoint above has no os32 equivalent to copy); and the
one-handle-type model may absorb the "terminal is a pipe" idea into something
cleaner than a `stdOutWritePipe` field comparison. The ancestor is cited so the
descendant *knows its parent* and chooses on purpose — converge where os32 was
right, take a different road where os64 can do better.

## Anticipated failure fingerprints (fill in as they are earned)

- *Ctrl+C kills husk / the whole session dies* → shell protection missing or the
  controlling-shell tag not set at launch.
- *Ctrl+C does nothing, `^C` shows up as a character in the next line* → the ETX
  interception never happened; `0x03` reached the console ring as data.
- *Ctrl+C only works while a program is reading input, not while cat spins* →
  the signal was raised at read time (console_read) instead of input time
  (delivery hook); a non-reading foreground app can't be interrupted.
- *cat dies but husk never wakes / prompt never returns* → the reap skipped
  `task_enqueue_dead_child`, so husk's `task_wait` backstop-sleeps a full second
  or hangs; async reap must run the SAME bookkeeping as `task_exit_finish`.
- *Intermittent corruption after a Ctrl+C under -O2* → a thread was reaped while
  still running on another core; the reap must only fire at that core's own
  resume checkpoint.

*Recorded 2026-07-19 for a successor to Fable 5, on the first evening in the
chair. The design is understood; the writing waits for daylight. 🍩*

---

## AS BUILT — 2026-07-24 (v1 shipped; where it kept faith and where it diverged)

Chris ratified at the keyboard (7/24): visible feedback at the prompt is
REQUIRED ("a keystroke that does nothing erodes faith"), the controlling-shell
tag is a `task_t` flag, and ~10ms latency is fine. Built and QEMU-verified the
same evening. Two deliberate deviations from the design above, both found
during build-time recon — recorded here so the doc stops describing a road not
taken.

### Deviation 1: the kill moved from the scheduler checkpoint to the SYSCALL BOUNDARY

The resume-checkpoint reap ("run task_exit_finish's bookkeeping FOR the
victim") has a hazard this doc missed: `scheduler_do` holds
`kSchedulerSwitchTasksLock` across the whole pass, and `handle_close_all`
bottoms out in filesystem closes — FatFs reentrancy locks that may be HELD by
a thread which needs the scheduler to run. Closing a victim's handles from
scheduler context is a deadlock waiting for its day. (Deferring the closes to
collection time was designed, then rejected for v1: more moving parts than the
gap it covers.)

What shipped instead — the SIGPIPE rail, generalized. The raise is still one
word-OR at the keystroke (`console_intr_intercept`, console.c, called from
keyboard.c's delivery choke). The KILL is `raise_terminating_signal_and_die`
(syscall.c — born `raise_sigint_and_die`, twin of `raise_sigpipe_and_die`,
renamed when it grew the HUP/TERM/PIPE ladder): the victim dies in its OWN context, through
the normal `task_exit` path — free to sleep, safe to close handles, retVal
130. Three roads lead there:

1. **The dispatcher check** (`_syscall_dispatch`): any task DOING anything
   crosses it constantly — a spinning `cat` dies at its next write call, in
   MICROSECONDS, beating the 10ms promise.
2. **`console_read` returns `CONSOLE_READ_INTERRUPTED`** at its loop top —
   how a task blocked on stdin dies.
3. **`pipe_read`/`pipe_write` return `PIPE_ERR_INTERRUPTED`** at their loop
   tops — how blocked pipeline stages die.

Sleepers reach their loop-top checks because `processSignals` now wakes any
ISLEEP thread with SIGINT pending (an interrupt outranks the nap). The wake
walk also captures `->next` BEFORE requeueing — the old walk followed the
pointer after the node had been relinked into qRunnable.

**The gap that lived for three hours — CLOSED the same evening (Slice A,
7/24 late):** a syscall-free ring-3 spin (`while(1);`) never crosses the
syscall boundary, so the pull half couldn't touch it. It dies now, by
Chris's os32 trick, named by him in review: **"I *forced* the task to make a
syscall."** os32's `_scheduler.s` rewrote the resume IRET frame to land in
`defaultSIGINTHandler` — a stub whose only job is
`call sysEnter_Vector(SYSCALL_ENDPROCESS)`. os64's port is
`scheduler_sigint_forced_syscall()` (scheduler.c), called at BOTH resume
flavors in scheduler_run_new_thread (the switch path after
scheduler_load_thread, and the continue-with-same-thread path, which resumes
from the mp_isrSaved arrays without a reload — both frame images get
patched): pending SIGINT + no handler + **saved CS is ring 3** (the seatbelt:
never abandon an in-flight syscall's locks; ring-0 frames are left to the
pull path) → saved RIP := TASK_EXIT_TRAMPOLINE_VIRT. The victim resumes,
executes the trampoline's `syscall`, and the dispatcher check already
shipped does the honors. No new machinery — the redirect target is the exit
trampoline every ring-3 task already carries, and the executioner is the
dispatcher check from the pull half. When userland delivery lands, the
handler-installed branch grows beside it exactly as os32 had two branches.
Verified: fixture `spin_test` (prints once, then loops with zero syscalls) —
Ctrl+C → `^C`, `$?` = 130, serial shows the SIGINT terminate line; pull-path
regression (cat + Ctrl+C → 130) still green. Also corrected from the
prior-art section: os32 WAS SMP (per-APIC mp_isrSaved* arrays) — its
shoot-on-sight kill was safe not because of one core but because its kernel
had almost no locks to leak. os64's executioner is careful because os64's
kingdom is richer.

### Deviation 2: shell Ctrl+C is a DATA BYTE, not an ignored signal

Better than "ignored in v1": when the foreground task IS the controlling
shell, the intercept declines and ETX flows to husk as data. husk's line
editor treats 0x03 as line-kill — echo `^C`, discard the half-typed line,
fresh prompt. The elevator button always lights up, and no userland signal
delivery was needed. husk also echoes `^C` once after collecting a pipeline
in which any stage died 130 (the echo at the funeral, ~10ms after the
keystroke — reads the same to a human).

### As designed, unchanged

`kForegroundTask` (defined task.c, doctrine in task.h) rides `task_wait`,
keyed on wait not spawn, restored to the shell on every return path; husk
learned zero new tricks for it. `controllingShell` flag set at launch
(kernel.c). Layering held: keyboard.c includes console.h only — no task or
signal headers in the device layer.

### Verified on the glass (QEMU, 7/24)

- `ech` + Ctrl+C at prompt → `^C`, fresh prompt, shell alive, line not run
- `cat` (blocked on stdin) + Ctrl+C → `^C`, `$?` = 130
- `cat | cat` + Ctrl+C → waited stage dies 130, downstream EOFs clean,
  `$?` = 0 (last stage wins — Bourne's answer), one `^C` echoed
- `cat /bin/husk` mid-scroll + Ctrl+C → dies in its write loop, `$?` = 130
- Ctrl+D EOF, backspace, ordinary commands: unchanged
- Serial log: zero panics, zero scheduler errors; husk reports
  `exited code 130` for every interrupted child

*The 2012 signal struct finally received the signal it was built for,
fourteen years and one word-OR later.*

## Raw mode (2026-09-08) — the one bit this terminal has

Everything above made two keystrokes mean something: Ctrl+C is SIGINT at
the foreground task, Ctrl+D is end-of-input. Those two interpretations are
the console's whole line discipline. There is no line buffering and no kernel echo —
husk edits its own line, arrow keys already arrive as VT100 bytes, and
Ctrl+~ and the VT chords live outside the byte stream altogether, the way
SysRq does. So where V7's `stty raw` flipped a dozen bits at once (and
`stty sane` exists because programs died with them flipped), os64's raw
mode is ONE bit: while it is set, the terminal interprets nothing and the
program reads 0x03 and 0x04 like any other byte.

The consumer was telnet (TELNET.md's booked gap: a Unix login you could not
log out of with Ctrl+D). In raw mode telnet reads 0x03 and sends `IAC IP`
itself, one path, bytes in and bytes out, which is what character-mode
telnet has done since 4.2BSD — the SIGINT-handler workaround it would have
needed on a cooked terminal is gone. ssh will want exactly the same bit.

**The rulings (Chris, 2026-09-08):**

1. **Both bytes, or neither.** A half-mode ("Ctrl+D is data, Ctrl+C still
   kills") would be a third state no other system has, and ssh needs the
   whole thing anyway. (Ruled by Fable on Chris's deferral.)
2. **A file, not a syscall.** `/proc/self/tty` already answers "what is my
   terminal"; a write of `raw` or `cooked` to the same file is how a program
   asks. The file's `mode` line reports it and `raw_task` names the holder —
   the same self-describing shape as `ctl`. Only `/proc/self` takes the
   write: a terminal mode is the sitter's own, so another task's tty file
   refuses at the open.
3. **The kernel resets it.** Raw mode is held in the asking task's name
   (`tty->rawHolder`) and `tty_task_departed` clears it at that task's death,
   so a crashed raw program can never leave a seat deaf to Ctrl+C, and there
   is no `stty sane` because nothing ever needs one. A fresh shell seating
   also starts cooked.

**Three rules beyond those** (the second and third from Codex's round on
PR #80):

- Only the terminal's FOREGROUND task may ask. A background job flipping the
  seat would steal Ctrl+C from the program the person is looking at.
  `tty_set_raw` refuses anyone else. And the foreground changes hands AT THE
  SPAWN, not first at the wait: a child is runnable the moment it is
  submitted and can reach a syscall before its parent reaches `wait`, so a
  telnet going raw as its first act would otherwise be refused by a race.
  `task_wait` re-affirms the hand-off; it no longer originates it.
- ONE HOLDER. A foreground child of the holder asking for raw is granted —
  the terminal is raw — but the mode stays in the holder's name, so the
  child's exit cannot cook a terminal its parent asked to have raw. Cooked
  from anyone but the holder is refused.
- A task that is TEARING DOWN cannot become the holder. A sibling thread
  can be in `task_exit` while another thread writes `raw`; the exit sets
  `tearingDown` (an exchange, a full barrier) before `tty_task_departed`'s
  clearing CAS, so the setter stores, fences, and re-reads the flag: if the
  CAS came after the store it cleared us, if before, the flag is already up
  and we clear ourselves. A freed task can never remain the holder.
- THE WRITER SPEAKS, never the opener (Codex round 2). `echo raw >
  /proc/self/tty &` has the shell open the file and a background child
  write through the inherited handle; attributed to the opener, that made
  the shell the holder of a mode nobody it seated asked for. A tty command
  is authorized and attributed to the task doing the write, and a handle
  that changed hands is refused. `os64_tty_set_raw` opens its own.
- A DEAD CHILD HANDS THE CONSOLE TO ITS PARENT when the parent is a
  foreground program on the same terminal, and to the shell otherwise
  (Codex round 2). With the console moving at the spawn, a middleman whose
  child died before it reached `wait` would otherwise find the shell in
  the pointer at its wait's entry — neither itself nor a live child — and
  never take the console back. Orphans are re-parented before their parent
  is freed, so the parent pointer never dangles; `task_is_live` is the belt.

**Where it acts:** both mode-controlled keys are classified WHEN THEY ENTER
THE RING, in one epoch. `console_intr_intercept_tty` returns "data" for
Ctrl+C when the terminal is raw, and `console_classify_tty` marks an EOT as
end-of-input (`keyboard_event_t.eof`) only when the terminal is cooked —
every producer that pushes into a tty ring (the keyboard router, a pty
master's write, a clipboard paste) calls them right before the push, and a
producer that skips the classification delivers a cooked terminal's Ctrl+D
as a byte. `console_read` reads the mark, never the current mode, so a
mode change lands on the next key and never re-reads one already queued.
Nothing else changes: the ring, the waiter, the focus, the pushback slot
are all as they were.

**Ring 3:** `os64_tty_set_raw(bool)` and `os64_tty_mode(&raw, &holder)`.
The mode is NOT a new field on `os64_tty_info_t`: that struct is filled
through a pointer, the kernel is the linker, and a binary built against the
older shape keeps running against a newer `libos64.so` — a grown struct
overruns its object. A new fact about the terminal is a new call.

**Proof:** `/tests/rawtty` (in the ring-3 suite): on a pty whose seated
shell spawns an ordinary WORKER (a shell's Ctrl+C is data by the prompt rule
and would prove nothing): cooked by default (a typed 0x04 is EOF), the worker
raw on request (0x03 0x04 'x' arrive as three bytes — on a cooked terminal
the 0x03 would have been SIGINT at the master's write), a helper child
granted raw without taking the holder's name, cooked again on request, reset
at the worker's exit, and a stranger's write to the child's tty file refused.
Plus the escape hatch Unix never had: a stuck raw program is killed from
another VT, and its seat is cooked the moment it dies.
