# PROC.md — processes as files (design)

*2026-07-25. Recorded before the writing, same discipline as MALLOC.md and
SIGINT.md. Chris ratified the shape at the keyboard: the name is `/proc`, the
format is text, threads are first-class, and **control is a write** — there is
no `kill` syscall in os64 and there is not going to be one.*

## The lineage

`/proc` is Tom Killian's, Bell Labs, **UNIX 8th Edition, 1984**. His USENIX
paper is titled *"Processes as Files"* and the motivation was that `ptrace`
was miserable: a debugger read the inferior's memory one machine word per
system call, through a keyhole. Killian's move was to notice that a process
is a thing with an address space and some state, and UNIX already has a name
for "a thing you can open, seek into, and read." So `/proc/1234` **was** the
process's memory, and debugging became `read()`.

**Plan 9** is the version that got the shape right: a *directory* per process
— `ctl`, `mem`, `status`, `note`, `ns`, `wait` — with control performed by
writing words to `ctl`. **Solaris** adopted it and made the files binary
structs. **Linux** adopted it and then put everything else in it too:
`cpuinfo`, `meminfo`, `sysctl`, `net`, `scsi`, `bus`. By 2002 they had to
invent `sysfs` to start walking it back, and the kernel's own documentation
now says *don't add new things to `/proc`*. That is a filesystem apologizing
for itself, and it is the entire reason this file opens with a constitution.

## The constitution

**If it is not a process, it does not live in `/proc`. Ever.**

Kernel state — memory statistics, CPU inventory, PCI topology, mount tables,
driver knobs — belongs somewhere else, under a name that says what it is.
That "somewhere else" is deliberately not designed here; designing it here is
exactly how Linux's `/proc` happened. When os64 wants to expose the mount
table, it gets its own mount with its own name, and this file stays untouched.

## The namespace

```
/proc/                      one entry per live task, named by decimal taskID
/proc/<id>/                 a task
/proc/<id>/status           state, parent, timing, fault counts
/proc/<id>/cmdline          argv, one argument per line
/proc/<id>/cwd              the task's current directory
/proc/<id>/handles          the handle table, one row per open handle
/proc/<id>/maps             the address space, one row per VMA
/proc/<id>/ctl              WRITE: control. READ: the accepted vocabulary.
/proc/<id>/thread/          one entry per thread of this task
/proc/<id>/thread/<tid>/status
```

**Threads are first-class** (Chris's call). Linux fudged this — threads hide
at `/proc/<pid>/task/<tid>` and the naming pretends a thread is a kind of
process. os64 has a real distinction to honor: a **task** owns the address
space, handles, cwd and environment; a **thread** owns registers, a stack, a
state and a core. So the task directory holds what the task owns, the thread
directory holds what the thread owns, and neither lies about the other.

**Why `maps` and not `mem`.** `maps` is a *description of* the address space,
not the address space. Killian's `/proc/n/mem` was the bytes themselves —
seekable, readable, the thing that killed `ptrace`. That name stays reserved
for the day os64 has a debugger to want it, because a `mem` that hands you a
table of ranges instead of memory would be a name that lies, and the house
doctrine on names that lie is settled (see `DEBUG_TASKSWITCH`).

**No `.` and no `..`** — the same rule the rest of the tree follows: they are
not directory content, and `readdir` never delivers them.

## The format: text, tabular, one record per line

Ratified: **text**, in the Plan 9 tradition rather than the Solaris one. The
argument is not aesthetics, it is that `cat` and `ls` already exist and this
costs userland nothing to consume. `key<TAB>value`, one per line — trivially
splittable by a five-line parser, and the console renderer already honors
`\t` so it reads as a table on the glass.

The deliberate cost: **no ABI is frozen here.** These files are a human- and
tool-readable *report*, not a struct. Fields may be added; a consumer parses
by key and ignores what it does not know. The moment someone wants a stable
binary shape, that is a syscall's job, not a file's.

**`cmdline` is one argument per line**, not NUL-separated. Linux's
NUL-separated `cmdline` is famously hostile to `cat`, and its only virtue —
unambiguity for arguments containing spaces — is delivered just as well by a
newline, which `cat` renders correctly. os64 takes the virtue and leaves the
hostility.

## `ctl` — control is a write

The whole point, and the reason there is no `kill` syscall:

```
echo kill > /proc/7/ctl
```

Nothing about the redirect is special. `echo` writes five bytes to handle 1
and has no idea what is behind it — the same indirection that lets it write
into a pipe. What is special is the **file**: procfs's write handler is not a
block writer (there is no block, no sector, nothing to store into), it is a
**parser**. It reads the word, finds the task, and acts.

What this buys:

- **`kill`, `stop`, `ps` become userland programs**, five lines each: open,
  write, close. Process control leaves the kernel's vocabulary entirely.
- **Permissions come free.** "May you signal task 7?" becomes "may you open
  `/proc/7/ctl` for writing?" — a question the filesystem already answers.
  Linux carries a dedicated permission tangle inside `kill()` for this; Plan 9
  deleted it by making it a file.
- **The control surface is inspectable.** You can `ls` it. A syscall number is
  invisible.

**Reading `ctl` returns the vocabulary this kernel accepts**, one word per
line. A control surface that describes itself cannot drift out of sync with
its documentation, and `cat /proc/7/ctl` is a better manual page than a manual
page.

### v1 vocabulary

| word | effect | exit code |
|---|---|---|
| `kill` | terminate the task, uncatchable | 137 (128+9) |
| `interrupt` | terminate the task; catchable once ring 3 can catch | 130 (128+2) |

Both are *terminate* today, because ring 3 cannot install handlers yet (the
ratified userland-signal-delivery DEBT). They are nonetheless two different
things and get two different exit codes, because a task that died from a
`ctl` write did not die "interrupted from the keyboard" and its exit status
must not claim it did.

### The rule that makes `ctl` safe: a ctl write only sets a bit

**A `ctl` write never touches a scheduler queue.** It ORs a pending-signal bit
into the target's threads and returns. Everything else — the queue surgery,
the reaping — happens where it already happens: at the per-core scheduler
checkpoint, under `kSchedulerSwitchTasksLock`, which is the only context
allowed to move a thread between queues.

This is not a workaround, it is the architecture SIGINT.md already argued for,
inherited whole. The keyboard IRQ cannot call `task_exit` on a task spinning
on another core; neither can a `write` syscall. Both do the same safe thing —
set a word — and the same three roads carry it out:

1. the dispatcher check in `_syscall_dispatch` (the victim's next syscall),
2. the blocking-call sentinels (`console_read`, `pipe_read`, `pipe_write`),
3. the forced-syscall push in `scheduler_run_new_thread` (a syscall-free spin
   loop, redirected into the exit trampoline — Chris's os32 trick).

The pleasing part: **Ctrl+C paid for all of this three commits ago.** `ctl`
adds a parser and inherits a delivery mechanism that is already proven.

### Deferred to v2: `stop` / `start`

`stop` and `start` are the obvious next words and they are *not* in v1, for a
specific reason. Terminating is idempotent and can be enforced anywhere the
victim is observed; **stopping is queue surgery**, and a thread RUNNING on
another core may not be moved between queues while it is executing — the same
caution SIGINT.md raised. Doing it correctly means a new check at the
scheduler's resume checkpoint ("do not resume this thread; park it in
`qStopped` and pick another"), which is a *scheduler* slice with its own
verification burden, not a filesystem one.

`SIGSTOP` and `SIGCONT` already exist as bits. When that slice lands, the two
words join the `ctl` parser and the vocabulary file starts listing them — with
no change required in any program that writes to `ctl`.

### Planned: `note`

Plan 9's other control file. Writing to `/proc/n/note` delivers a **note — a
string, not a number**: you do not send `2`, you send `"interrupt"`, and the
process receives the text. Plan 9 threw out numbered signals entirely, and the
argument (a signal is a message, and `9` is a terrible message) is one os64
already agrees with in every other corner of its design.

`note` waits on userland signal delivery, because a note nobody can catch is
just `kill` wearing a better name.

## The engineering: os64's first synthetic filesystem

Everything above rests on one new thing — a filesystem with **no block
device**. This is the first one in the tree, and it is why `/proc` is a real
kernel slice rather than a formatting exercise.

- **Mounting.** `kRegisterFilesystem` reaches through
  `device->block_device->ops` to copy block operations, so it cannot be used;
  procfs builds its `vfs_filesystem_t` directly and claims the `/proc` prefix
  in `kMountTable` itself. It mounts *after* the secondary-partition sweep, so
  its all-zero `part_guid` can never collide with a real partition's during
  GUID dedupe.
- **Routing is free.** `vfs_resolve_mount` is pure longest-prefix string
  matching, so `/proc/7/status` reaches procfs with tail `/7/status` without
  one line of change anywhere. The mount table was built for this.
- **`readdir` walks `kTaskList`**, not a disk. `ls /proc` is the scheduler's
  task list rendered as directory entries.
- **Reads are snapshots.** A procfs file's content is generated *once, at
  open*, into a kmalloc'd buffer; `read`/`seek`/`tell` then serve that buffer.
  This is a deliberate choice over generating on each `read`: it makes a file
  internally consistent (you cannot read the first half of a task's status and
  the second half of its successor's), it makes `seek` trivially correct, and
  it means the task list is walked at exactly one well-defined instant instead
  of at every 4 KB chunk. The cost is staleness between open and read, which
  for a status report is not a cost at all.
- **The `f_path` lifetime rule applies unchanged** (vfs.h): procfs stores the
  pointer it is handed and the handle closer frees it. Store the base pointer,
  never `path + offset`.

## The un-synchronized snapshot (this slice's known debt)

`handles` and `maps` walk live structures — the handle table and the VMA list —
with **no lock**, because os64 has no per-task lock to take. The target task may
be mutating them on another core. Consequences, honestly stated:

- Reading a handle slot mid-change yields the old or new value; both are valid.
- The real hazard is a pointer: if a task closes a file while its `handles` file
  is being rendered, the `vfs_file_t` and its `f_path` can be freed underneath
  the read — and under the lazy HHDM a freed page is *unmapped*, so the
  dereference panics rather than reading garbage. Same for a VMA being unmapped
  mid-walk.
- Both walks are **bounded** (`PROC_MAX_LIST_WALK`), so a spliced or corrupted
  list can never become an infinite loop in the kernel.
- The common case — a task reading its **own** `/proc` entry — has no race at
  all: it is the one blocked in the syscall.

The fix is a per-task lock (or deferred free), and it is deliberately not in
this slice: it is a task-lifetime change, not a filesystem one. Note that the
same un-synchronized shape already exists elsewhere (`task_reap_eligible_zombies`
walks `kTaskList` and its dead-child chains with no lock) — /proc did not
introduce this class of risk, it made it visible.

Related: **`kTaskList` is append-only.** Nothing has ever unlinked from it, so
it holds every task the system has created. /proc hides reaped ones
(`proc_task_is_visible`) rather than displaying corpses, but the underlying leak
is real and now has a place where it can be seen.

## Failure fingerprints

| Symptom | Cause |
|---|---|
| `sigind` shows a keyboard interrupt nobody typed | The old `SIGKILL = 9` in a bitmask enum: 9 == `SIGHALT\|SIGINT`. Fixed 2026-07-25 to a free bit |
| A `/proc` file reads as an empty file | The snapshot generator returned 0 bytes at open — the task vanished between path resolution and generation. Legal and expected; a task that exits mid-`cat` is not a bug |
| `cat /proc/N/cmdline` prints ANOTHER task's name (`/idle7` for husk) | Dereferencing `task->argv[i]` from kernel context. The pointer ARRAY is a kmalloc'd kernel object but every pointer INSIDE it is a task VA (`TASK_ARGV_VIRT + …`). Translate through the task's own page tables and read via the HHDM (`proc_copy_task_string`) |
| `cmdline` still says `/idle7` for tasks 32–40 AFTER that fix | Not a procfs bug — a KERNEL one, and the real reason the read above "succeeded" instead of faulting. `task_initialize` gives ktask `kKernelPML4v` (task.c:455) and every idle task its parent's (task.c:436-440) — **nine tasks, one address space** — while `task_create` maps each task's argv blob at the FIXED VA `TASK_ARGV_VIRT` (task.c:930). Each creation clobbers the last; idle7 is created last, so `0x6f000000` in the kernel page table really does hold idle7's blob. Latent only because `task_setup_entry` is gated on `loadedElfProgram` and kernel tasks load no ELF. Same shape threatens `TASK_ENV_VIRT` and `TASK_EXIT_TRAMPOLINE_VIRT`. Found by Chris, 2026-07-25, reading `/proc` — the first thing in os64 that ever displayed the argv mapping |
| `ls /proc/N` fails on a task that `ls /proc` just listed | Correct behavior, not a bug: the task WAS the previous command, and it exited and was reaped in between. Target a long-lived task (husk) when testing |
| A thread's core reads `18446744073709551615` | That is `THREAD_NO_AFFINITY`, and `mp_apic` is the thread's *pinned affinity*, not the core it runs on. Rendered as `affinity any`. The current core lives in each core's CLS and would have to be found by scanning them |
| A refused `ctl` write reports `$? = 0` | Not procfs — `echo` ignored every `os64_write` return and exited 0 unconditionally. /proc's ctl is the first thing in os64 that ever answers a write with "no", so it is the first thing that ever caught this. Fixed in echo.c |

## What is deliberately NOT here

- **No `/proc/self`.** It is genuinely useful and it is also the first step
  onto the slope; it arrives when a program demands it, per the
  consumer-driven rule, not because Linux has one.
- **No writable anything except `ctl`.** Not `status`, not `maps`, not ever.
- **No kernel state.** See the constitution. This is the whole point.
