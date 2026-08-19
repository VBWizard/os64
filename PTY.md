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
  client-notification seam lands — this is that seam's FOURTH customer,
  after publish-ack, theme reload, and pty-dirty was already counted.)
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
- **Resize** — grid realloc + child notification; gate: window resize
  existing at all (GRAPHICS #5).
- **Scrollback exposure** — the slave's ring already holds history;
  a view_offset in the snapshot header exposes it; gate: the terminal
  wanting Shift+PgUp parity.
- **Shared-mapping snapshot** — gate: profiling showing the copy matters.
- **The /dev/tty knob** — gate: the pager slice getting picked up.
- **Raw input pass-through** (0x03 as data) — rides STREAM mode.

## Failure fingerprints (predicted; verify against reality when built)

- **Terminal shows a frozen grid, child alive**: generation not bumping on
  some grid mutation path — every `tty_write` door must touch it.
- **Ctrl+C kills the TERMINAL, not the child**: the 0x03 went through the
  terminal's own tty instead of the master write intercept — check which
  side consumed it.
- **Child's reads never return**: the synthesized events missed the ring's
  waiter wake — same wake discipline as the keyboard producer, no shortcuts.
- **`ps` shows the child seated on the wrong tty**: the spawn argument
  resolved after inheritance instead of overriding it.
