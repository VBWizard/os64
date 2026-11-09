# The config search path — design and record (BUILT 2026-08-23)

*Ruled by Chris the afternoon `/home/desktop.conf` became the sixth file to
carry its own private copy of the same ladder. Designed by Fable, built by
Opus the same day. Every decision below was made in conversation and the
reasons are recorded so they are not re-litigated. Where the BUILD differs
from the design, the difference is called out and argued — those are the
parts worth reading twice.*

## The problem

Six config files, six hand-rolled "where do I look" sequences:

| Reader | Ring | Ladder before |
|---|---|---|
| `logd` (`logd.conf`) | 3 | `/home` → `/etc` → `LOGFMT=` → `classic` |
| `husk` (`husk.rc`) | 3 | `/home` → `/etc` → `/fat` (lifeboat spelling) |
| `os64get` (`os64get.conf`) | 3 | `/home` → `/etc` → cwd |
| resolver (`hosts`, `net.conf`) | 3 (libos64) | `/home` → `/etc` (both) |
| desktop (`desktop.conf`) | 0 (`gui/desktop.c` — deleted 2026-08-25; the reader is ring-3 `/bin/desktop` now, via `os64_conf_find`) | `/home` → `/etc` |

Same idea, five spellings, and the seventh file would add a sixth. Chris's
ask: **one setting says where config files are looked for, first to last, and
every reader obeys it.** He considered the environment and rejected it — it
freezes at spawn, it is per-process, and a reader with no environment at all
(the kernel's desktop reader, as it was then; the resolve syscall, still) cannot
use it. A file tells the truth at read time; that
is the `/sys/gui` ruling (GRAPHICS.md) applied to configuration.

## What was built

### 1. One root file with a fixed address: `/etc/os64.conf`

Something has to be found before a search path exists. This is the one file in
the system that is never searched for. Grammar is every other conf's — `key =
value`, `#` comments, 8KB cap, no sections:

```
conf = /home/conf:/etc
```

Colon-separated, first hit wins, exactly PATH's shape (1979, and still the
right one for "a list of places, in order"). Absent file or absent key = the
built-in default `/home:/etc`, which is what every reader did before, so
nothing changed until someone wrote the line. **The shipped file has that line
commented out on purpose**: the default IS that line, so an untouched system
behaves exactly as it did, and uncommenting is a deliberate act.

**`/home/conf`, not `/home/.config`.** Dotfiles were never designed: an early
Unix `ls` skipped `.` and `..` by testing only the first character, so every
name beginning with `.` vanished, and hiding files that way became a habit by
accident (Rob Pike's account). os64's `readdir` never delivers `.` or `..` and
has no dot magic to hide behind; a visible directory in a curated tree is the
honest shape.

### 2. One walker, two rings, one truth

- **Kernel side** (`kernel/src/conf.c`, `kernel/include/conf.h`): `conf_init()`
  runs in `kernel_init` after the root mounts AND the secondary sweep (the
  default ladder names `/home`, which is a mount — a walker that ran earlier
  would conclude the user's directory does not exist), and before logd, which
  is the first reader to ask. `conf_find(name, out, cap)` walks the list and
  returns the first path that opens. The probe follows the house pattern:
  `vfs_resolve_mount` outside (pure string matching, safe from any CR3), the
  actual open on the `call_in_kernel_context` trampoline.
- **`/sys/conf`** publishes the ladder, its source, and per reader which file
  actually answered:
  ```
  path: /home:/etc
  source: built-in default (/etc/os64.conf sets no conf)
  logd.conf: /etc/logd.conf
  husk.rc: /home/husk.rc
  ```
  This is the diagnostic Chris asked for by name: *"for some time I'll want to
  be able to verify where each conf file is coming from."*
- **libos64**: `os64_conf_find(name, path_out, cap)` and the convenience
  `os64_conf_find_read(...)`.

### 3. Every reader migrated

logd, husk (`husk.rc`), os64get, the resolver (`hosts`, `net.conf`), desktop.
`bootenv.conf` (2026-08-30, `kernel/src/bootenv.c`) was born on the ladder —
the environment every task inherits, applied by the kernel right after
`conf_init`, merged last-directory-first so `/home` layers over `/etc`.
Each lost its private ladder. husk's lifeboat spellings (`/fat/husk.rc`,
`/husk.rc`) stay hardcoded and stay LAST — the lifeboat is for the day the
ext2 root is broken, and the search path's own root file lives on that root.

### 4. Logging — DEBUG_BOOT, unconditional

Chris: *"DEBUG_BOOT is perfect since I always have that on."* Every resolve
prints one line, and the root file announces once:

```
conf: search path /home:/etc (built-in default (/etc/os64.conf sets no conf))
conf: husk.rc <- /home/husk.rc
conf: desktop.conf <- /etc/desktop.conf (no /home/desktop.conf)
```

The misses are named, because "why is it not reading MY copy" is the question
this line exists to answer.

## Where the build departed from the design — and why

**1. Ring 3 asks the KERNEL to walk, rather than parsing `/sys/conf` itself.**

The design had libos64 read `/sys/conf`'s `path:` line, parse the colon list,
walk it, and then report back through "a sysfs-write or a syscall — Opus's
choice". That is **two parsers of one ladder and two channels to reach one
answer**, and it still needed a syscall for the reporting half.

Built instead as `SYSCALL_CONF_RESOLVE` (47): the kernel walks, and hands back
the path. The ladder is then parsed in exactly one place — which was the
stated point of the slice — and `/sys/conf`'s per-reader lines come free,
because the kernel is the thing that resolved them. It also passes the bar for
entering the syscall table: "where is the config file called X" is an
operation, not a diagnostic. It RESOLVES rather than OPENS, so the caller uses
the handle machinery it already has and the ABI grows no second way to acquire
a handle.

**2. `hosts` MERGES, so the walk is resumable.**

The design said the resolver migrates like everyone else. It cannot, quite:
Chris ruled `hosts` **merged** on 2026-08-22 — `/home/hosts` layers *on top of*
`/etc/hosts` so your machine names sit over the system's list rather than
erasing it — and it is written into `etc/hosts` in those words. First-hit-wins
would have silently overturned that ruling and made a name in `/etc/hosts`
stop resolving the moment `/home/hosts` existed.

So the syscall takes a `from` position and returns the matching index plus one:
feed a call's answer back as the next call's `from` to walk to the following
copy. `hosts` enumerates; `net.conf` (a settings file, where your copy replaces
the system's) takes the ordinary first hit. The distinction is real —
**a settings file is not a database** — and the walker serves both rather than
forcing one reader to keep the private ladder this whole slice exists to
abolish. Only a `from` of 0 is remembered for `/sys/conf`, so an enumeration's
later copies cannot overwrite the row and claim to be the file the reader took.

## Verification (2026-08-23)

- Boot: `conf: search path /home:/etc`, `conf: husk.rc <- /home/husk.rc`,
  `conf: desktop.conf <- /etc/desktop.conf (no /home/desktop.conf)`. 24
  pre-boot + 28 post-boot tests green, zero panics.
- `cat /sys/conf` in husk shows the ladder, the source, and both readers'
  answers — logd taking `/etc`'s copy and husk taking `/home`'s, visibly
  different, which is the whole diagnostic.
- **The proof:** setting `conf = /etc:/home` in `/etc/os64.conf` and rebooting
  moved husk from `/home/husk.rc` to `/etc/husk.rc` and `source:` from
  "built-in default" to `/etc/os64.conf`. One line, one file, every reader
  obeyed. (Reverted after the test.)

## Explicitly not in scope

- Re-reading a config without a reboot (a signal or an inotify-shaped thing):
  separate slice, nothing here prevents it.
- Per-user anything: os64 has one user.
- Environment-variable overrides (`CONF=`): rejected above; the file is the
  carrier.
- Backfilling ABI.md's syscall inventory, whose table stops at 43 — 44/45/46
  (pty_create, pty_snapshot, tty_handle) were never added and 47 was not going
  to be the one row that made it look complete. `syscall_numbers.h` remains
  the authority. Booked.
