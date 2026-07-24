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
- **The unfinished half is inherited too.** os32 registered handlers into
  `sighandler[SIGINT] = sigAction`, but `executeSigHandler()` was an **empty
  stub** — the async *delivery* to a userland handler was never built. os64's
  `sigaction()` carries a `sigAction` param marked "reserved for real handler
  registration; unused until then," and the "userland signal delivery" DEBT is
  the SAME open TODO, fourteen years later. This design (kernel-enforced default
  action) is the honest interim on both OSes; the DEBT is where they converge.

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
