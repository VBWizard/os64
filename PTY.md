# PTY — a terminal with a program where the glass should be

*The design record for os64's pseudo-terminal: how a ring-3 program comes to
own another task's terminal. Companion to GRAPHICS.md (the compositor the
terminal app will live inside), LIBDRAW.md (the toolkit it will be built
with), and tty.h (the object this design multiplies one more time). Designed
2026-08-19, the pty conversation; rulings Chris's, same day. RATIFY BEFORE
CODE — the VT8 chapter's ritual, kept.*

## Why this exists

Phase E's terminal emulator — husk in a window, the self-hosting moment —
needs something os64 has never had: a userland program standing where the
keyboard and glass usually stand. And a second customer has been waiting
since 2026-08-13: the pipes-into-pagers problem (`ps | less` — less's stdin
is the pipe, but its KEYS must come from the terminal), deferred with "not
now" precisely because it deserved this design instead of a hack. One design
serves both, which is why it waited.

The lineage is 4.2BSD (1983 — the same release that closed the rename()
window, a good year for finishing ideas): the pseudo-terminal, a pair of
ends where the **slave** looks exactly like a terminal to the program using
it, and the **master** is held by whatever pretends to be the human — xterm,
screen, sshd, or os64's coming terminal window. The name stays `pty`
(ruled): os64 diverges on design, not vocabulary — the Unix names carry
meaning and culture, and ~95% of the utility fleet already honors them.

## What os64 already built without meaning to

Three discoveries from the code that shaped everything below:

1. **Echo is already userland's job.** console.c's discipline is "the caller
   echoes" — the kernel delivers bytes, husk paints them. Classic pty design
   drowns in line-discipline modes (echo, cooked, raw); os64 exiled that
   policy to userland years before ptys existed. A pty slave has no echo
   machinery to replicate because there is none.
2. **Console handles are already tty-relative.** A task talks to ITS tty —
   `task->tty`, inherited at creation — and `tty_t` already carries the
   multiplied singletons: per-tty fgTask (Ctrl+C aim), per-tty EOF, waiter,
   pushback, `tty_shell_departed`. The whole controlling-terminal concept
   exists; ptys just mint more terminals.
3. **spawn already resolves parent handles into child slots.** The
   redirection plumbing pipelines use is the plumbing a terminal needs to
   seat a child on a slave.

So a pty is not a new subsystem. It is **a `tty_t` with no keyboard and no
glass**, plus a handle to stand where they stood.

## THE FORK, and the principle it minted (ruled 2026-08-19)

Two shapes were on the table:

- **A — the grid pty (os64-native)**: the slave keeps a character grid like
  every VT; the kernel's ONE terminal interpreter (`tty_write`: \n \t \b \r,
  wrap, scroll, the scrollback ring) does what it always does; the terminal
  app's job is *grid in, keys out* — read cells, blit, forward keystrokes.
  The VT8 chapter promised exactly this shape ("the pty seam swaps the grid
  source from a kernel VT to a slave pty").
- **B — the byte-stream pty (the 1983 classic)**: the slave streams raw
  bytes; the master's holder interprets them itself.

**The ruling: A now, with B designed in as a named MODE from day one.** And
the principle that decided it, recorded because it is bigger than this fork:

> **A future customer you are CERTAIN about counts as a customer.** The ABI
> philosophy's "no compliance without an interop reason" forbids building
> speculatively; it was never license to build something that LOCKS OUT a
> customer already walking up the driveway. For big decisions, buy the SEAM
> cheap now; defer the feature.

The certain customer is remote access — telnet/ssh, wanted "shorter term."
And examining what a remote daemon actually needs is what made the fork
dissolve: telnetd moves BYTES between a socket and a shell; the RENDERING
happens on the remote end (the laptop's xterm holds the grid). So the remote
customer wants the child's output **pre-interpretation** — and would bypass
the interpreter no matter which option won today. Everything expensive about
a pty is flavor-independent: the slave being a `tty_t`, controlling-tty
inheritance, the per-tty Ctrl+C intercept, spawn-with-tty, the master handle,
EOF. The flavor itself is one question at one choke point — `tty_write` is
the single entry for slave output — so:

- **`PTY_MODE_GRID`** (v1, the terminal window): output interprets into the
  grid, master snapshots cells.
- **`PTY_MODE_STREAM`** (deferred, pre-named): output appends to a byte ring
  — a pipe wearing a tty's identity, and os64 already owns pipes — master
  read()s bytes. Its gate is not this design: it is TCP `listen()`, which
  does not exist yet (the RTL8125 arc was dial-out only). When networking
  grows an inbound door, telnet-before-ssh (telnet is 1969 vintage and adds
  a socket glued to a master; ssh drags in a crypto arc).

Picking A misbuilds nothing B's customer needs. B's arrival is a mode bit, a
byte ring, and a read path — additive, at a seam this document names now.

## The design

### Creation

`SYSCALL_PTY_CREATE` (next free row): `pty_create(cols, rows)` → a **master
handle** (new kind, `HANDLE_PTY_MASTER`). The kernel kmallocs the slave
`tty_t` — grid sized cols×rows with the standard scrollback ring, input
ring, singletons zeroed, mode = GRID — and registers it on a pty list
SEPARATE from `kTTY[8]`. The VT fleet's iterators (knock-summon sweep,
Alt-cycling's `% TTY_COUNT`) never see ptys, by construction. `/proc/self/tty`
answers `pty<n>`; rows/cols report what the creator declared.

One handle rules the pair: the terminal never reads the slave directly, so
the slave needs no handle of its own — it is named THROUGH the master where
needed (below), and by `task->tty` everywhere else.

### Seating a child

`spawn` grows one argument beside the redirect slots: a controlling-tty
handle. `HANDLE_NONE` = inherit the parent's tty (today's behavior,
unchanged); a master handle = the child's `task->tty` becomes that master's
SLAVE. The child's default console handles then route to the slave with zero
new code, because console handles were already tty-relative — husk seated on
a slave reads the slave's input ring and tty_writes the slave's grid without
knowing anything happened.

### The master's verbs

- **write(master, bytes)**: keystrokes going in. Bytes become synthesized
  key events (ascii-set, scancode 0 — which is exactly what forwarding
  printable keys produces) into the slave's input ring, waking its waiter
  the way the keyboard does. One byte is intercepted first: 0x03 runs the
  per-tty interrupt path and aims SIGINT at the SLAVE's fgTask — the
  machinery the VT arc scoped per-terminal, doing its job for a terminal
  that happens to be a window. (STREAM mode will want a raw pass-through
  flag; noted, deferred with the mode.)
- **`SYSCALL_PTY_SNAPSHOT`** (GRID mode's read): copies out a small header
  (rows, cols, cursor row/col, a GENERATION counter, flags) and the live
  cells. A grid is not a stream, and pretending read() streams it would be
  dishonest — so the snapshot is its own verb, the klog_read precedent. The
  generation counter makes polling cheap: bump it on every grid mutation;
  the terminal polls header-only at frame cadence (~30Hz, blessed) and
  full-reads only on change. ~16KB copy for 80×25, only when dirty —
  trivial. (A shared read-only mapping of the grid is the someday
  optimization; the snapshot ABI survives it. And the poll retires when the
  client-notification seam lands — THIS POLL is that seam's third customer,
  after publish-ack and theme reload. It used to be counted as the fourth,
  alongside a separate "pty-dirty" entry, which was the same thing twice;
  and the RESIZE notification was miscounted into the same list, which sent
  the gterm DEBTS row down the wrong road for a while. Resize is not a seam
  customer — it is SIGWINCH's, see the Resize row below. The seam is about
  when a client WAKES; a signal is about a fact a process must hear.)
- **read(master)**: reserved for STREAM mode. In GRID mode it returns a
  clean error naming the snapshot syscall — a mode misuse should teach, not
  mislead.

### Lifetime and hangup

The slave holds two reference kinds: attached tasks (`task->tty` pointers,
counted at attach/exit) and the master handle. Child exits → count falls;
snapshot's flags report HUNGUP when no tasks remain seated, which is the
terminal's cue (it also holds the child's pid from spawn and may wait it —
both doors work). Master closes first → the slave is orphaned: in GRID mode
a child writing to an orphaned slave is BENIGN (the grid absorbs it, nobody
watches — no SIGPIPE theater needed; STREAM mode will need a real rule,
deferred with the mode). The slave `tty_t` frees when BOTH the master is
closed and the seat count is zero.

### The /dev/tty door (the pagers' half)

`less` with a pipe on stdin needs terminal keys: the answer is minting fresh
console handles bound to `task->tty` — which ptys make universally correct,
since a pager inside a terminal window resolves to the slave exactly like a
pager on VT3 resolves to VT3. The MECHANISM (a small syscall now vs a /dev
name when a devfs exists) is its own tiny slice, deliberately outside this
design's scope: nothing here blocks it, everything here feeds it.

## v1 scope, and the fixture that proves it

Phase D builds: `pty_create`, the spawn tty argument, master write with the
0x03 intercept, `pty_snapshot` with generation + HUNGUP, lifetime rules.

**Acceptance is `ptyprobe`, not the terminal**: a fixture that needs NO GUI —
it creates a pty, seats a husk on the slave, writes `ps\n` to the master,
polls the snapshot, and prints the slave's grid to its OWN stdout. The whole
round trip — spawn-seating, input injection, interpretation, snapshot —
proven from a text VT, headless-testable in the harness. Ctrl+C acceptance:
write 0x03 at a running `hog`, watch it die by the slave's fgTask aim. Then
Phase E's terminal is ptyprobe wearing libui: grid→window blit, keys→master,
and the self-hosting moment.

## Deferred, each with its trigger

- **STREAM mode** — gate: TCP `listen()`; customer: telnetd.
- ~~**Resize**~~ — **BUILT 2026-08-25**: `pty_resize` (syscall 51) reallocs
  the grid and raises **SIGWINCH** at the seats; gterm's resize arm calls
  it; `/bin/winchtest` proves it headless. The program inside a pty has no
  window and no queue to read — that is the whole reason 4.3BSD invented
  the signal. **Design and what the build taught: § "Resize — the SIGWINCH
  slice" below.**
- **Scrollback exposure** — the slave's ring already holds history;
  a view_offset in the snapshot header exposes it; gate: the terminal
  wanting Shift+PgUp parity.
- **Shared-mapping snapshot** — gate: profiling showing the copy matters.
- **The /dev/tty knob** — gate: the pager slice getting picked up.
- **Raw input pass-through** (0x03 as data) — rides STREAM mode.
- **SIGHUP on master close** — gate: window close buttons (GRAPHICS #5).
  Today a master closing orphans the slave benignly (GRID absorbs the
  writes), which is correct for a probe but wrong for a terminal WINDOW:
  clicking the X with husk alive inside would leave a ghost session typing
  into a grid nobody watches. The classic answer, with the etymology worn
  proudly: SIGHUP is named for a MODEM hanging up mid-session — the master
  closing is the same event fifty years on, the phone line replaced by a
  window's X. Mechanism exists (task_signal_all_threads at the slave's
  seats); the ruling on who exactly gets signalled (the seated shell, or
  every seat) is taken when the X is real. `exit`-closes-the-window needs
  none of this — HUNGUP already delivers it, and ptyprobe's act 3 is its
  standing test.

## Resize — the SIGWINCH slice (designed 2026-08-25, BUILT the same day)

*Written before the building, the way SIGNALS.md and MALLOC.md were. The
Resize row above was the booking; this is the design it pointed at, with
what the building changed marked where it happened. One thing the design
missed outright — see "The wake" below — and it was the piece that made
the signal useful rather than merely deliverable.*

### It is TWO hops, and they are different animals

The single word "notification" in the old booking hid a mechanism choice.
GRAPHICS.md § Event delivery now carries the rule — **a signal tells a
PROCESS something; an event tells a WINDOW something** — and the two hops
land on opposite sides of it:

1. **WM → gterm: an EVENT, and it already arrives.** `window.c` produces
   `INPUT_EVENT_WINDOW_RESIZE`, gterm's event loop already catches it, and
   the resize arm (`gterm.c`, the block whose comment says the grid does not
   follow "not yet") already refreshes its draw context and forces a repaint.
   **This hop needs no new mechanism at all** — only the arm's body.
2. **gterm → the program inside the pty: a SIGNAL.** That program has no
   window and no event queue; it may be `husk`, or `ls` halfway through a
   listing. This is SIGWINCH's entire job description and precisely why Sun
   invented it for 4.3BSD.

### The shape

- **A new syscall, `SYSCALL_PTY_RESIZE(master_handle, cols, rows)`** — 51 is
  the next free number. gterm calls it from the resize arm after computing
  the new grid from its window's content rect. It is the master's verb
  because the master OWNS the geometry: the slave learns, it does not decide.
- **The kernel half does three things, in this order:** realloc the grid
  under the tty's own lock, bump the GENERATION counter (the snapshot poll's
  whole contract — every grid mutation door touches it, per the fingerprint
  list below), then raise SIGWINCH at the SEATED TASKS. Not the window owner
  — gterm is the window owner and is the one doing the telling.
- **No new locking hazard, and this is worth stating because F29 made it a
  reflex:** the kGuiLock → signalLock order does not arise here. The raise
  happens on gterm's own syscall thread, inside the pty layer; the compositor
  is not in the call path and kGuiLock is never held.
- **Reflow policy: NONE, deliberately.** Preserve the top-left origin, clamp
  the cursor into the new bounds, blank the new cells. Every hardware
  terminal and most software ones do exactly this; real reflow (rewrapping
  logical lines) is a scrollback feature and belongs with the scrollback row,
  not here. Say it in the code or someone will file it as a bug.
  **One refinement, found while building:** "clamp the cursor" alone would
  have eaten husk's prompt — a shell sits on the LAST row, so shrinking by
  five rows would have clipped the five rows that contain it and left the
  cursor clamped onto unrelated text. `tty_resize_grid` therefore rolls the
  top rows into scrollback when the cursor would otherwise fall off the
  bottom, which is what xterm does and what a fixed-glass VT100 never had to
  think about. The origin is preserved whenever preserving it is possible.
- **The wake — the piece the design missed.** Every park loop in the kernel
  (console_read, the pipes, sleep, event_wait, thread_join, the three net
  waits) ended only for `SIGNALS_TERMINATING`, and `processSignals` rousted
  a sleeper only for the same mask — correct while every catchable signal
  was also terminating, and useless the day one was not. A handled SIGWINCH
  raised at a shell blocked in `read()` would have been delivered on the
  next KEYSTROKE. The fix is one predicate, `signal_park_must_end(thread)`:
  a terminate pending, or ANY pending signal a handler will catch — because
  the handler is armed only at the dispatcher's exit, and a thread that
  stays parked never reaches it. Twelve sites, one question. The syscall
  boundary's `current_thread_will_catch` gained the matching edge: a park
  that ended for a non-death signal whose handler was uninstalled meanwhile
  answers INTERRUPTED, not death — there is nothing terminating to name.
- **Default action = ignore, and ignore is spelled CONSUME.** An unhandled
  pending WINCH is cleared at the pick (`signal_pick_deliverable`, under
  the same lock both delivery paths hold) rather than sitting pending until
  a handler is installed an hour later and fires for a resize nobody
  remembers.

### Admitting number 28

The number is already RESERVED (`abi/include/os64/signal.h`). Admitting it
means obeying the two rules the gauntlet wrote:

- **`signal_is_known` gains 28 with its PRODUCER NAMED** (the rd14 rule —
  SIGCONT/SIGSTOP were thrown out for having none). Its producer is
  `pty_resize`, and now it is real.
- **SIGWINCH MUST NOT ENTER `SIGNALS_DEFAULT_IS_DEATH`.** Its default action
  is *ignore* — this is the one mistake that would be catastrophic and
  invisible in review, because every program that does NOT handle it (husk,
  ls, cat, all of them) would die the first time a window was dragged. rd9's
  lesson applies directly: each mask answers ONE question, so check this
  against the mask that means "what happens when the handler cannot run",
  not the one that means "would a checkpoint stop this thread".
- Undeliverable-and-non-terminating is already handled: §10 drops it with a
  log line. That is the correct behaviour for a WINCH nobody can receive.
- **Its home is `SIGNALS_DEFAULT_IS_IGNORE`, and ignore means CONSUMED AT
  PUBLICATION** (Codex #32): a task with no SIGWINCH handler gets no
  pending bit at all. "Leave the bit and let the pick drop it" looked the
  same and was not — a thread parked with no handler is deliberately left
  asleep, never reaches the pick, and a handler installed later by a
  sibling would run for a resize from an hour ago. The pick's own
  consumption survives as the backstop for a handler uninstalled between
  publication and delivery.
- **Every park ends for it, `task_wait` included, and every park LEAVES
  through `current_thread_will_catch()`.** The wait was the one that did
  not ask (a shell could not hear a resize until its job finished —
  `winchtest` act 6 is its fixture), and sleep was the one that asked the
  narrower question on the way out (see the fingerprints).

### "It changed" is useless without "to what" — and that half already exists

A signal carries no payload (the pending set is a bitmask, by design). The
program must then ASK. **It already can: `/proc/self/tty` prints `rows` and
`cols`** — built in the VT arc under the file-not-syscall doctrine, and it
reports a pty's geometry today. No new syscall, no `ioctl`, no `TIOCGWINSZ`.
`ls`'s hardcoded 100 columns (DEBTS, item b) becomes a read of that file.

**THE TRAP, and it has already bitten once:** procfs generates its text at
**OPEN**, not at read — this cost a wasted control experiment during the rd10
audit (`proc_gen_maps` runs at open, so a probe kept the file open and saw
stale data). A program must therefore **re-open** `/proc/self/tty` after each
SIGWINCH; an fd held across the signal answers with the old size forever.
Write that where the handler goes, not just here.

### The fixture, and why it needs no GUI

Per rd9's doctrine — **prove the test before trusting its green** — the
fixture must be run against a deliberately broken kernel first (bump the
generation but skip the raise: the child must then report the OLD size).

It is fully HEADLESS, which is the happy part: `ptyprobe`'s shape already
covers it. Create a GRID pty, spawn a child that installs a SIGWINCH handler
and re-reads `/proc/self/tty`, call `pty_resize`, and check the child reports
the NEW geometry. No compositor, no window, no QMP screendump — the whole
hop-2 contract is testable in the ordinary suite. Hop 1 is then a one-line
call in the resize arm, verified on glass by dragging a gterm and watching
`husk` reflow its prompt.

### Order of work (each lands green before the next)

1. ~~`pty_resize` + grid realloc + generation bump~~ — `tty_resize_grid`
   (tty.c) + syscall 51 (syscall.c). **BUILT.**
2. ~~Admit 28, raise it, fixture with its broken-kernel control.~~ **BUILT:
   `/bin/winchtest`** (in testrun's table, exit 0x0A1D0000). Its control
   was run before it was trusted: with the raise deleted it exited
   0x0A1D0006, "child never reported the grown size".
3. ~~gterm's resize arm calls it~~ — **BUILT**, the arm now computes the
   grid from the surface and calls `os64_pty_resize`.
4. *Separately, and NOT part of this slice:* `ls` reading `/proc/self/tty`,
   and the DEFERRED-WRAP bug (DEBTS item a — a `pending_wrap` flag in
   `tty.c`, what every terminal since the VT100 does). That bug is the one
   actually producing the blank lines Chris sees, it is smaller than all of
   the above, and it fixes the symptom at ANY width on gterm AND the text
   console. Do not let it ride this slice's timeline.

## Failure fingerprints (the first four predicted before the build, the rest earned)

- **A dragged gterm resizes but husk only notices at the next keystroke**:
  a park loop is testing `SIGNALS_TERMINATING` instead of asking
  `signal_park_must_end` — every blocking wait must ask the predicate, and
  a new one copied from an old one will carry the mask test with it. (Or
  not asking at all: `task_wait` re-parked on every wake until Codex #32,
  so a shell with a running job was deaf. `winchtest` act 6.)
- **A multi-threaded program dies 130 when its window is dragged, with
  no Ctrl+C anywhere**: a park's EXIT asked `signal_has_handler_for_pending`
  instead of `current_thread_will_catch()`. The WINCH is task-wide; a
  sibling delivered it first and cleared this thread's bit, and "nothing
  pending" was read as "nothing will catch it" — a death with no name,
  which the ladder tags SIGINT. Sleep did this (Codex #32).
- **A program installs a SIGWINCH handler and it fires at once for a
  resize that already happened**: the ignored WINCH was left pending
  instead of consumed at publication (`task_signal_is_ignored`, task.c).
- **Dragging a window kills everything in it (exit 130/141)**: SIGWINCH has
  crept into `SIGNALS_DEFAULT_IS_DEATH` or `SIGNALS_TERMINATING`. Its
  default is ignore; `winchtest` act 2 catches this (the child dies instead
  of reporting).
- **A program's SIGWINCH handler reports the OLD size**: it is reading a
  `/proc/self/tty` handle it opened earlier — procfs renders at OPEN; re-open
  after every signal.
- **The prompt vanishes when a window is made shorter**: the shrink is
  clamping the cursor instead of rolling the top rows into history
  (`shift` in `tty_resize_grid`).

- **Terminal shows a frozen grid, child alive**: generation not bumping on
  some grid mutation path — every `tty_write` door must touch it.
- **Ctrl+C kills the TERMINAL, not the child**: the 0x03 went through the
  terminal's own tty instead of the master write intercept — check which
  side consumed it.
- **Child's reads never return**: the synthesized events missed the ring's
  waiter wake — same wake discipline as the keyboard producer, no shortcuts.
- **`ps` shows the child seated on the wrong tty**: the spawn argument
  resolved after inheritance instead of overriding it.
