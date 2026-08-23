# The config search path — design record (2026-08-23, UNBUILT)

*Ruled by Chris the afternoon `/home/desktop.conf` became the sixth file to
carry its own private copy of the same ladder. Written by Fable for Opus to
build from. Nothing here is speculative: every decision below was made in the
conversation, and the reasons are recorded so they are not re-litigated.*

## The problem

Six config files, six hand-rolled "where do I look" sequences:

| Reader | Ring | Ladder today |
|---|---|---|
| `logd` (`logd.conf`) | 3 | `/home` → `/etc` → `LOGFMT=` → `classic` |
| `husk` (`husk.rc`) | 3 | `/home` → `/etc` → `/fat` (lifeboat spelling) |
| `os64get` (`os64get.conf`) | 3 | `/home` → `/etc` → cwd |
| resolver (`hosts`, `net.conf`) | 3 (libos64) | `/etc` only |
| desktop (`desktop.conf`) | 0 (`gui/desktop.c`) | `/home` → `/etc` |

Same idea, five spellings, and the seventh file would add a sixth. Chris's
ask: **one setting says where config files are looked for, first to last,
and every reader obeys it.** He considered the environment and rejected it
— it freezes at spawn, it is per-process, and the kernel's own readers
(desktop, hosts) have no environment at all. A file tells the truth at read
time; that is the `/sys/gui` ruling (GRAPHICS.md) applied to configuration.

## The design

### 1. One root file with a fixed address: `/etc/os64.conf`

Something has to be found before a search path exists. This is the one file
in the system that is never searched for. Grammar is every other conf's —
`key = value`, `#` comments, 8KB cap, no sections:

```
# /etc/os64.conf — the system's root configuration
conf = /home/conf:/etc
```

Colon-separated, first hit wins, exactly PATH's shape (1979, and still the
right one for "a list of places, in order"). Absent file or absent key = the
built-in default `/home:/etc`, which is what every reader does today, so
nothing changes until someone writes the line. Other system-wide knobs that
do not belong in the environment will accumulate here over time — that is
the point of having a root file, and the answer to "the environment would
get big".

**`/home/conf`, not `/home/.config`.** Dotfiles were never designed: an
early Unix `ls` skipped `.` and `..` by testing only the first character, so
every name beginning with `.` vanished, and hiding files that way became a
habit by accident (Rob Pike's account). os64's `readdir` never delivers `.`
or `..` and has no dot magic to hide behind; a visible directory in a curated
tree is the honest shape. The default ladder above keeps `/home` itself as
the first stop so nothing existing moves; a user who wants `/home/conf`
writes the line.

### 2. One walker, two rings, one truth

- **Kernel side** (`kernel/src/conf.c`, new): after the root mounts and the
  secondary partitions auto-mount (so `/home` exists — the order matters and
  is why this cannot run earlier), read `/etc/os64.conf` once, parse the
  `conf` key into an ordered list of up to 8 directories, and expose
  `conf_find(const char *name, char *out_path, size_t cap)` — walks the
  list, returns the first path that `open`s. Kernel readers (desktop.c; the
  resolver's `hosts` reading if it ever moves kernel-side) call it.
  Reads go through `call_in_kernel_context` when the caller is a task
  (desktop.c already does; see its `read_whole_file`).
- **`/sys/conf`** (sysfs.c): publishes the active ladder and, per reader,
  which file it actually took:
  ```
  path: /home/conf:/etc
  source: /etc/os64.conf            (or "built-in default")
  desktop.conf: /home/conf/desktop.conf
  logd.conf: /etc/logd.conf
  ...
  ```
  The per-reader lines come from readers *reporting* (`conf_note_used(name,
  path)` on the kernel side, a sysfs-write or a syscall from ring 3 — Opus's
  choice; the syscall is simpler and `/sys` files are not writable today).
  This is the diagnostic Chris asked for explicitly: "for some time I'll want
  to be able to verify where each conf file is coming from."
- **libos64 side**: `os64_conf_open(const char *name, char *path_out,
  size_t cap)` — reads `/sys/conf`'s `path:` line (one read, cached for the
  process lifetime), walks it, returns the handle of the first hit and the
  path it found. ONE parser for the ladder, and it lives in the kernel; ring
  3 consumes the result through a file, exactly the `/sys/gui` pattern.
  Readers keep their own value-parsers — only the *finding* is shared.

### 3. Every reader migrates

logd, husk (`husk.rc`), os64get, the resolver (`hosts`, `net.conf`),
desktop. Same first-hit semantics, same file names; each loses its private
ladder and gains a `conf_note_used`. husk's lifeboat spelling (`/fat/husk.rc`)
stays as a last-resort fallback *after* the walk — the lifeboat exists for
the day root is broken, and the walker's root file lives on root.

### 4. Logging — DEBUG_BOOT, unconditional

Chris: "DEBUG_BOOT is perfect since I always have that on." Every reader
prints ONE line at `DEBUG_BOOT` when it settles:
```
conf: desktop.conf <- /home/conf/desktop.conf
conf: logd.conf <- /etc/logd.conf (no /home/conf/logd.conf)
```
and the root file itself announces once: `conf: search path /home/conf:/etc
(/etc/os64.conf)`. (desktop.c's existing `desktop:` lines are `DEBUG_GUI`
and were invisible on the P5 for that reason — the "which file won" line is
boot news, not GUI debug. Move it.)

## Size and order

S-to-M. Suggested order, each step green before the next: kernel walker +
`/etc/os64.conf` + `/sys/conf` → desktop.c migrates (the kernel-side proof)
→ `os64_conf_open` in libos64 → logd → os64get → husk → resolver. Ship
`etc/os64.conf` in the build (GNUmakefile's ext2 `write` list AND the
lifeboat's `/etc`, which grew today for `desktop.conf`) and add its
`os64get.conf` routing row.

## Explicitly not in scope

- Re-reading a config without a reboot (a signal or an inotify-shaped
  thing): separate slice, nothing here prevents it.
- Per-user anything: os64 has one user.
- Environment-variable overrides (`CONF=`): rejected above; the file is the
  carrier.
