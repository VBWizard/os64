# The command line's length — the chain, the history, and the plan

*Written 2026-09-14 by Fable, from a conversation with Chris while PR #102
waited on its last review round. Built in two slices, kernel first: the
spawn half (agreement 4 below) is in the tree; husk's line, history and
arena, and sshd's cap (agreements 1–3 and 5), are the second slice.*

## Why this came up

An `ssh host 'command'` into the P5 with five greps in it bounced with
"exec request failed": the daemon caps an exec command at 255 bytes. That
cap is not the daemon's opinion — it is husk's line, seen from the wire.
Chris: "I don't think there's a good reason to limit husk at 256."

## The chain as it stands

A command line passes through a row of walls, and each one is a separate
number in a separate file. Raise one alone and nothing visible changes.

| Wall | Where | When this was written | Now |
|---|---|---|---|
| The line husk reads and edits | `LINE_MAX`, `userland/apps/husk/husk.c` | 256 | 256 |
| One argument across spawn | `OS64_SPAWN_ARG_MAX` (`abi/include/os64/syscall_numbers.h`) | 256, asserted equal to `TASK_MAX_PATH_LEN` | 128 KiB, its own number |
| The whole argv block in the child | `TASK_ARGV_MAX_BYTES`, `kernel/include/task.h` | 1 MiB, checked in `task_create` | 1 MiB, checked before spawn copies a byte |
| The environment block | `TASK_ENV_MAX_BYTES` | 64 KiB | 64 KiB |
| The ssh exec command | `SSH_COMMAND_MAX`, `userland/apps/sshd/ssh_engine.h` | 255 | 255 |

Two of these bind each other directly: `husk -c` hands the WHOLE line across
spawn as ONE argument, so the line cap and the per-argument cap must move
together, and the daemon's cap trails both.

## Lineage

- **bash has no line limit.** readline grows its buffer as you type; the
  only wall it ever meets is the kernel's at exec.
- **V7's whole argv budget was `NCARGS` = 5120 bytes** — the origin of the
  "about 4K" instinct.
- **POSIX sets the floor for `ARG_MAX` at 4096.**
- **Linux today:** about 2 MiB for the block, 128 KiB for any single string
  (`MAX_ARG_STRLEN`).

256 is not a Unix number anywhere. It is a "fits in a small static array"
number, which is what os32 chose and what husk inherited.

## What we agreed

1. **husk's line goes to 4096** — the POSIX floor, and V7's whole budget.
   The line buffer itself stays a plain array: one buffer, process
   lifetime, an arena for one thing is ceremony.

2. **History becomes a byte ring, not a slot matrix.** Today it is 32 slots
   of `LINE_MAX`; at 4 KiB lines that is 128 KiB reserved for the worst case
   on every entry, and most entries are `ls`. A ring of bytes holding
   variable-length records gives the memory to the lines that exist: a
   16 KiB ring holds hundreds of typical lines or four enormous ones. One
   BSS array, no allocator. (bash does this with malloc'd strings; we do it
   without malloc.)

3. **Per-line scratch lives on an arena.** Words, substitution captures,
   expansions: many buffers, all born while a line is parsed, all dead when
   it finishes. A mark-and-release arena reset at each prompt replaces the
   pile of fixed stack arrays with sizes chosen at run time and never frees
   anything individually. This is os32's marker idea with an even cleaner
   lifetime rule: ONE COMMAND LINE. It lives INSIDE husk first (a bump
   pointer over a BSS region, mark, release — twenty lines) and moves to
   libos64 the day a second program asks; the browser's page parse is the
   likely second consumer.

4. **The kernel sizes the spawn block by real lengths.** The block was
   `argc × 256`, kmalloc'd and zeroed per spawn, which is what kept the
   per-argument cap small: at 4 KiB per slot, `ls a b c` would have zeroed
   16 KiB. Spawn now measures every string before it copies any, so the
   per-string cap is Linux-sized (128 KiB) and the 1 MiB block ceiling does
   the real bounding — checked before a byte is copied, and answered with
   `OS64_SPAWN_TOO_LONG` (−3), so a shell can say "argument list too long"
   rather than "cannot run". The constant was already named for an argument;
   the second job it had been doing — telling a caller how big a path buffer
   must be — went to a new `OS64_PATH_MAX` (256), and the assert that tied
   the argument cap to `TASK_MAX_PATH_LEN` now ties the path cap to it.

5. **sshd's cap follows husk**, as it does now (`LINE_MAX - 1`). Its
   `command[]` sits in the engine struct, where 4 KiB is noise. The guest
   probe's "overlong command refused" stage (256 x's) must move past the
   new cap or it will start passing for the wrong reason.

## BSS, because it decides the memory argument

Chris's first instinct was malloc, to avoid "a 160 KB executable that is
128 KB of static buffer". The premise does not hold, and it matters:

| husk today | bytes |
|---|---|
| file on disk | 93,256 |
| text | 40,969 |
| data | 608 |
| bss | 271,304 |

An uninitialised static array is `.bss`: zero bytes in the ELF, and under
os64's demand paging zero RAM until a page is first touched. The history
matrix is already there beside 260 KB of other static buffers, and husk's
file is 93 KB. Static is the CHEAPEST option, not the most expensive;
malloc would add the allocator's bookkeeping to get the same laziness. The
byte ring and the arena are the right shapes for the DATA, not for the
memory — Chris: "I kinda forgot that BSS existed."

## Practical limits that do NOT bind

- husk's stack: per-level word and directory buffers on a 1 MiB user stack,
  block depth 8, substitution depth 3 — 4 KiB lines are comfortable, 64 KiB
  would still fit.
- The environment's 64 KiB bounds `x=$(cmd)` VALUES, not lines.
- The pipe and redirection parser already takes 512 words (`ARGS_MAX`,
  matching the kernel's `SPAWN_MAX_ARGS`).

## The wart it would have exposed, paid first

The console's backspace clamped at column 0 and never climbed to the
previous row, so a line longer than the screen could not be edited back
across the wrap; a 256 cap hid it and a 4 KiB cap would have made everyone
meet it. It was its own slice and went first (PR #106): husk's caret moves by
row and column now, on every terminal. One of that slice's booked limits
comes back with the longer line — a line taller than the screen cannot be
climbed back into (DEBTS.md, "husk's screen model trusts its inputs") — and
is the second slice's to settle or keep.

## Verification plan

- Host: whatever `tools/test_husk_host*` covers of the parser gets a 4 KiB
  line and a 4 KiB word; the arena gets a mark/release unit test.
- Guest: a 1,000-byte `ssh host '<command>'` through `tools/sshd_probe.py`,
  a 1,000-byte line typed by sendkey and read back with `echo`, and the
  moved overlong-refusal stage.
- Kernel: `/tests/argsize`, in `testrun` — a 100 KiB argument arrives whole
  in argv and in `/proc/self/cmdline`, one at the cap in argv, and 512
  arguments are all carried; one byte over the cap, nine arguments at the
  cap, and a 513th argument are each refused with `OS64_SPAWN_TOO_LONG`; an
  unreadable argument is still a bad address. And on the host,
  `python3 tools/test_spawn_argv_host.py` compiles the kernel's own measure
  and copy passes and races them the way a second thread could: exact at
  the terminator, the 512th argument and the block's last byte, never
  reading an unreadable page, and refusing a string that grows, dodges the
  cap, or moves its terminator between the passes.

## Settled

The three questions this section held were settled on 2026-09-15, on the
picks Opus offered, when Chris said to proceed:

- The line is 4096 bytes. Nothing in the analysis needs it larger.
- The history ring is 16 KiB.
- No new name for the argument cap — `OS64_SPAWN_ARG_MAX` always meant one.
  Paths got `OS64_PATH_MAX` instead (agreement 4).
