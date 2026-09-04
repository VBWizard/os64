# DIVERGENCES.md — where os64 deliberately parts ways with Unix/Linux

*The ledger of every place this OS looked at what Unix does and chose
differently — on purpose, with a reason. Breadcrumbs, not essays: each row
points at the doc or header that carries the full argument. Started
2026-07-25, when the divergences still fit in living memory.*

**Why this file exists twice over:** (1) Chris mentally tracks every
divergence and forgets them just as fast; paper doesn't. (2) The day os64
considers running Linux executables, this file IS the requirements list —
every row below is a spot where a compat layer has to translate, and every
row in "Kept on merit" is one it doesn't.

**Absence is not divergence.** Things os64 simply hasn't built yet (quoting
in husk, symlinks, users/permissions, TTY objects) don't belong here. This
file records *decisions*, not gaps — gaps live in DEBTS.md.

---

## Kept on merit (the convergences — just as deliberate)

| Kept | Note |
|---|---|
| `fork` AND `exec`, both first-class | Alongside spawn. Good names, good ideas (LIBOS64 § process model) |
| Handles 0/1/2 = in/out/err | The contract every filter is written against |
| The single-rooted tree | Without the sediment — see the tree section below |
| Linux's 200ms retransmit-timer floor, against RFC 6298's "SHOULD 1 second" | Kept from Linux on MERIT, not inheritance (2026-09-04): the floor exists so a peer's delayed ack is not read as loss, 200ms clears that, and one second — the fixed timer os64 had — made every lost segment on a millisecond wire cost a second (rig: 11.8s → 3.3s for 100KB at 10% loss). tcp.h's timer comment carries the argument |
| `#!` on line one, handled by the KERNEL's exec — and `$1`..`$9`, `$*`, `$#` in husk | Ritchie's V8 (1980) put `#!` in exec() so the loader answers "what runs this file" and every program, not just the shell, can run a script. os64 does it in task_create (elf_loader.h): one level, an absolute interpreter, one optional argument, V7/BSD semantics. No filename extensions, ever — the file says what it is on its first line (Chris, 2026-08-22). The positional sigil is `$` because husk already expands `$CWD`/`$NAME` with it; `%1` was DOS's and a second sigil would be the wart |
| Small tools composed by pipes | The whole userland doctrine |
| `/etc/environment` — the first environment as a key=value DATA file, not a script | `bootenv.conf` (2026-08-30), on the config ladder, `/home` layered over `/etc`, read by the kernel into the first task's block. V7's `/etc/profile` (1979) was a script the login shell ran — husk.rc's ancestor, with the same blind spot: a program that is not a shell's child never sees it (the desktop's clock showed UTC while husk's showed Eastern). PAM's `/etc/environment` closed the same gap in the 90s, as data. `kernel/include/bootenv.h` |
| `\|` `<` `>` `&` shell notation | `<` predates `\|` by two years; `&` is Thompson 1973 |
| `$?`, exit code 0 = success | Bourne 1977; last pipeline stage speaks for the job |
| 128+signo death codes (130, 137) | Human-legible corpse tags, kept even though signal *bits* diverged |
| **200+vector for a CPU exception with no signal** (200 = `#DE`, 206 = `#UD`, 213 = `#GP`, 216 = `#MF`, 219 = `#XM`) — a DIVERGENCE inside the convergence | Unix would say 136 (SIGFPE) or 132 (SIGILL); os64 sends no such signals, and a `$?` that names a signal that never existed is a lie. The band sits above 128+31 so the two can never be confused, and the number names the vector without the log (`user_exception_kill`, 2026-08-27) |
| `cd` as a builtin | Inherited cwd at spawn makes it structural, same as every shell since V6 |
| The `-NUM` count form (`tail -40`, `head -20`, `grep -3`) | POSIX marks it OBSOLESCENT and it stays anyway, as an opt-in `numeric_alias` row in os64_args. The ambiguity POSIX cites — collisions with negative operands and with option bundling — does not exist in this grammar (bundles are letters; no program declares a digit as an option letter), so the objection is inherited, not earned. Decades of muscle memory is a real interface requirement in an OS with one daily user (args.h, 2026-08-01) |
| cwd-first, then PATH walk; argv[0] stays as typed | V6 behavior, colon-separated PATH (V7's gift) |
| Ctrl+letter = control codes; Ctrl+D = EOT = EOF | 1963 semantics, done properly (one-shot EOF, then normal reads) |
| ELF, SysV x86-64 calling convention | Interop with the toolchain — a *specific reason*, per the philosophy |
| Syscall args in RDI/RSI/RDX/R10/R8/R9 | Hardware forces R10-for-RCX; happens to match Linux |
| ext2 on-disk format | FFS re-expressed; full read/write, including writable root, judged by the host's own e2fsck (`make fsck-ext2`); FAT keeps boot/interop forever |

---

## Syscalls

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| `errno` (global, action at a distance) | In-band errors only; designed convention = RAX:RDX value:status pair (interim: two high sentinels) | The error check belongs at the call site | ABI § register contract |
| `lseek` (whence + offset dance, return needs care) | `seek` returns the NEW absolute position; `SEEK_END+0` = file size, no fstat needed | The useful answer, directly | ABI inventory #10 |
| `getdents` + per-entry `stat` dance; `struct dirent`'s poverty | `readdir` yields name+size+kind in ONE call; returns 1/0/<0; EOF is sticky | One call, whole answer | dirent.h, ABI #11 |
| Separate `struct stat` | `stat` fills the SAME `os64_dirent_t` readdir yields — "stat is readdir for exactly one name" | One struct, no fork | ABI #23 |
| `readdir` delivers `.` and `..` | Never. They are not directory content | Plan 9 was right | SUCCESSION #7 |
| `open` flag ints (O_RDONLY\|O_CREAT\|...) | Mode strings "r"/"w"/"a"/"c", "u" for update, "x" for atomic create-new, "d" for directories; unknown modes REFUSED at the boundary | The common operations get names; unsupported combinations cannot slip through as flag arithmetic | ABI #9 |
| `O_DIRECTORY` / opendir as separate machinery | Directories open with mode "d", same open, same close, one handle type | One door | ABI #9 |
| `brk`/`sbrk` | NEVER. `map(len)`/`unmap(base)` regions: demand-paged, zeroed, guard page after, VAs never reused | brk frees only from the end; stale pointers must fault | DEBTS not-debts, MALLOC.md, ABI #12-13 |
| `waitpid(WNOHANG)` | `reap` is its OWN syscall; returns 0 = "nobody died" as an ordinary answer | A wait that doesn't wait is a name that lies | syscall.c reap, task.h |
| `/proc/meminfo` (text file, parsed with string code; "free" changed meaning when the page cache arrived; MemAvailable = 2014 errata for 1992 fields; linuxatemyram.com) | `memory(out)` syscall fills a typed struct: every field's meaning fixed FOREVER — `reclaimable` is the future page cache's seat (0 today, honestly), `available` = free+reclaimable summed BY THE KERNEL, `free + used == usable` is a live audit any program can check | Numbers may change; meanings never do. Userland never does column arithmetic | os64/memory.h, ABI |
| `kill(2)` | Does not exist and never will. `echo kill > /proc/N/ctl` | kill(pid, SIGCONT) resumes a process — the name has lied since 1971; ctl is self-describing | PROC.md § ctl |
| `getpid(2)` | `taskid()` — same syscall (40), same answer, os64's noun. The `get` is gone too: it earns its keep opposite a `set`, and nothing sets its own identity | Renamed 2026-08-24, and NOT for tidiness. os64 runs tasks, so "pid" names a thing it doesn't have — but the cost was concrete: "pid" reads as per-*process*, libos64's config writer believed it and built its temporary file name out of "the pid", and every thread of a program collided on that one name (Codex #29 rd8). The V1-1971 lineage is kept in the comment; only the noun changed. Still PER-TASK — there is no thread id yet, and the header says so out loud so the next reader doesn't repeat the mistake | abi syscall_numbers.h #40, os64/proc.h |
| Unknown flags silently ignored (much of POSIX) | Unknown spawn flags / open modes / ctl verbs REFUSED | A silently dropped request "succeeded" and didn't | syscall.c boundaries |
| `unlink(2)` + `rmdir(2)`, two removal verbs | ONE verb: `unlink` removes files AND empty directories; rmdir will never exist (ratified 2026-08-04) | POSIX's split is a pre-4.2BSD scar (setuid rmdir(1) hand-unlinking `.`, `..`, then the entry); Plan 9's `remove()` got it right | abi syscall_numbers.h #35, ABI #35 |
| `unlink` of an open file succeeds (blocks freed at last close) | **CONVERGED 2026-08-16** — os64 now does the same for regular FILES, and `rename` over an open file likewise. An open DIRECTORY still refuses | Refused from 2026-08-04, explicitly consumer-driven ("built the day a consumer needs it"). The consumer arrived: replacing `/bin/husk` over the network while husk is running — a task holds its own ELF open for demand paging. Converged on MERIT, not inheritance: the inode outliving its name is what makes a file a file rather than a directory entry. Kept honest by the CRASH story rather than the happy path — the orphan list lives on disk in `s_last_orphan`, a mount replays it out loud, and e2fsck already knows that field | ext2_write.c orphan chain / VERIFICATION.md crash procedure |
| `rename(2)` replaces almost anything: file over file, empty dir over empty dir; some systems even swap | REPLACEMENT IS FILE-ONTO-FILE ONLY. A directory is never replaced, and a directory never lands on an existing name. Legacy replacement is atomic on ext2; FAT retains its remove-first behavior. Callers that require safety use `rename_with_flags`: atomic no-replace everywhere, or atomic replacement where supported and refusal otherwise | The window rename(2) was invented to close (4.2BSD, 1983, because link-then-unlink couldn't be atomic) is worth every line; the directory-replacement corners are where POSIX's rename grew its `..`-and-link-count folklore, and no os64 consumer has asked for them | `abi/include/os64/syscall_numbers.h` #43/#54, ABI #43/#54 |
| `rename(2)` across filesystems fails with EXDEV and the caller copies | Same line, same reason — refused at the syscall boundary, and `mv` does copy-then-unlink in userland | A convergence kept on MERIT, not inheritance: a half-finished copy needs a cleanup policy (retry? keep the partial? prompt?), and policy belongs where a human can be asked | syscall.c syscall_rename |
| `rename(2)` of an open file succeeds | **CONVERGED, same day it was written (2026-08-16)** — open FILES rename freely on both sides; an open DIRECTORY refuses | Shipped that morning REFUSING both sides, inheriting rm's 2026-08-04 ruling. By evening the orphan chain had shown the inheritance was wrong on the source side (an ext2 handle holds an inode number, not a path, so a reader cannot even perceive a rename) and unnecessary on the destination side (the displaced inode simply survives, nameless, until last close). A directory handle IS mid-walk through what a move re-parents, so that one stands | ext2_write.c ext2_rename |
| `fsync(2)` matters (write-back caches defer everything) | ext2 writes are FULL WRITE-THROUGH: data + inode committed before write() returns; `sync` is an already-kept promise | No block cache to flush, and a file being appended reads at true length to everyone immediately — honestly better than FAT's looks-empty-until-sync (which is why sync exists at all). A future block cache re-earns sync | ext2_write.c durability doctrine |
| `O_NONBLOCK` open flag + `EAGAIN` errno; `SO_RCVTIMEO` where 0 = block forever | read() carries its patience as an argument: 0 = wait zero ms (poll), N = deadline, `OS64_WAIT_FOREVER` = block; empty wait returns `OS64_ERR_TIMEOUT`, its own verdict | Ruled 2026-08-05. V7's O_NDELAY let an empty read return EOF's 0 — a decade-long in-band lie POSIX had to apologize for with O_NONBLOCK/EAGAIN; and SO_RCVTIMEO's 0-means-forever makes zero's one honest meaning unsayable. os64 follows the poll()/select() tradition: zero means zero, forever is spelled out. Refused (not ignored) on handles that don't honor it | abi syscall_numbers.h #4 |

---

## Processes & jobs

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| fork+exec as the only birth | `spawn(path, argv, in/out/err, flags)` first-class beside fork | The everyday launch deserves one syscall | ABI #5 |
| Child inherits the WHOLE fd table | NO blanket inheritance: the parent's 0/1/2 + exactly the handles asked for | Kills the classic held-open-pipe-end pipeline hang | DEBTS not-debts |
| Background job reads tty → SIGTTIN stop (sessions, pgrps, tcsetpgrp) | `OS64_SPAWN_BACKGROUND` is kernel-known; a background read of handle 0 returns EOF (`cmd &` ≡ `cmd < /dev/null &`) | v1 shape; ctl stop/start later makes it SIGTTIN-like without the session machinery | task.h backgroundJob, syscall_numbers.h |
| Sessions / process groups / controlling terminal | `controllingShell` flag + `kForegroundTask`, riding `wait` not spawn | The 10% that does the job; generalizes to per-tty later | SIGINT.md |
| Shells may leak zombies; init adopts orphans | Reporting a finished job and reaping it are the same act — `&` cannot leak | The shell buries its own dead | jobs commit 6167aa3, husk.c |

---

## Signals

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| Signals are small integers (SIGKILL = 9) | Signals are BITS in a mask (SIGKILL = 1<<8) | Pending-set arithmetic is one OR; 9 in a bitmask reads as SIGHALT\|SIGINT — found the hard way | signals.h |
| `kill(2)` delivers | A ctl write ORs a pending bit; delivery rides the existing checkpoints | See syscalls table; the ONLY safe async raise | PROC.md § ctl safety rule |
| SIGPIPE default: terminate | Kept — but enforced in-kernel until ring-3 delivery exists | It's what makes `yes \| head` exit | DEBTS § userland signal delivery |

---

## Pipes & handles

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| fds are ints into a table of many object kinds with per-kind APIs | ONE handle type: tagged reference; read/write/close dispatch on the tag (files, pipes, dirs, someday windows) | Routing changes, the contract doesn't | LIBOS64 § handle model |
| `PIPE_BUF` (4096) atomicity | One sentence: a write ≤ PIPE_CAPACITY lands WHOLE or waits; reads return SHORT | No magic constant to memorize; the asymmetry is intentional | pipe.h, DEBTS not-debts |
| Pipe buffer can be grown (`F_SETPIPE_SZ`) | The bound IS the flow control; never grows | An unbounded pipe is a memory leak with a friendly API | DEBTS not-debts |
| Zero-copy ambitions in-band | Two copies through a kernel ring, always; a future `splice` hides BEHIND the handle API | Userland must never own the ring's head/tail | DEBTS not-debts |
| One refcount per fd | TWO refcounts per pipe: EOF = absence of writers, EPIPE = absence of readers | Each end's absence means a different thing | pipe.h |
| `dup2` fights redirection | Redirect beats pipe when both claim a slot | One rule, no ordering puzzle | SUCCESSION #6 |

---

## Networking (rulings ratified 2026-07-28; NETWORK.md carries the arguments)

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| `sockaddr` cast circus, address families, `connect(fd, (struct sockaddr*)&sa, ...)` | Kernel ABI: typed structs ONLY, no text at the syscall boundary; libos64 speaks Plan 9 dial strings — `os64_dial("tcp!10.0.2.2!80")`, protocol segment ALWAYS explicit (it's the verb: stream vs datagram), `net!` wildcard refused | The struct is the contract; the string is user-input syntax and user input is a library problem. The `!` is UUCP bang-path lineage via Plan 9 — ruled fitting by the resident hippie | NETWORK.md ruling #1 |
| `htons()`/`ntohs()`/`htonl()`/`ntohl()` at every call site, forever | ABI is HOST-ORDER everywhere; the kernel owns the wire. The entire swap surface of the OS is four helpers in net_wire.h; htons never exists in libos64 | 1981's big-endian coin toss is not every application's problem; forgetting htons compiles clean and connects to port 20480 | NETWORK.md ruling #2, net_wire.h |
| `listen()`/`accept()` trio: vestigial backlog int, sockaddr out-param, then `getpeername()` | A listener HANDLE whose read() yields one `os64_netconn_t {handle, peer_ip, peer_port}` — accept IS read; peer identity arrives with the connection | Berkeley's own poll() reports a pending connection as READABLE — its event API spent 40 years admitting accept is a read | NETWORK.md ruling #3 |
| `sendto`/`recvfrom` per-packet addressing as UDP's front door | Dial a UDP peer once, then plain read/write on the handle; a recvfrom-equivalent arrives only when a real consumer demands it | Covers DHCP/DNS/ping cleanly; consumer-driven growth (the args-parser precedent) | NETWORK.md ruling #4 |
| `/proc/net/*` text files (network state squatting in the process filesystem) | Syscall first (memory(out) pattern: typed struct, meanings fixed forever) for fixed-shape state; a real `/net` mount later (procfs template) for browsable tables. /proc stays processes-only, constitutionally | Numbers may change; meanings never do — and /proc's scope creep is exactly how Linux's happened | NETWORK.md ruling #5 |

---

## The tree & filesystems

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| /usr split, /sbin, /usr/local sediment | /bin holds ALL executables; /lib, /etc, /home, /tmp, /dev later; NO /usr ever | The /usr split was Ken and Dennis's 1971 disk shortage (a full RK05), not a design | SUCCESSION #7 |
| fstab + mount(2) | fstab's one row conflated WHAT mounts (local policy) with WHERE (now the disk's own business — a partition mounts at its GPT label). os64 splits them: `/etc/mounts.conf` lists only WHAT joins the namespace at boot (`mount = <name> [/where] [ro]`, gui.conf's whole-list rule; absent file = the authored-GUIDs default sweep — the old Windows-safety allowlist demoted from gate to default). Runtime mount(52)/unmount(53) since 2026-08-30: `mount` names a PARTITION (GPT name or GUID — PARTLABEL-first, where Linux ended up), never a device path, and any partition it can name is mountable — naming is the deliberateness | Boots self-describing; the not-fstab is a list of names, no columns to drift; Windows-safety kept where the accident lived (the uninvited sweep) | vfs.c, os64/mount.h, CLAUDE.md |
| A mount point must be an existing directory | The mount table IS the namespace at that level (Plan 9's view, ratified 2026-08-30): the point itself need not exist, only its PARENT must be a real directory — so `ls` never invents one. Unix mounts OVER a directory because a second RK05 needed a name inside the first one's tree (1971) | No empty stub directories to curate | vfs.c vfs_mount_explicit |
| lsof duplicates a shared hold per process (`txt`/`mem` rows for every instance running one binary) | One kernel-held row per ACTUAL open: a dyn-linked program's image is held once by the shared-object registry however many instances run, and `/sys/openfiles` says so — lsof prints it under `kernel`, not once per task. Linux duplicates because its lsof's *source* is per-process (`/proc/pid/maps`); the shared `struct file` under its hood is just as singular, never shown. Ratified 2026-08-31: "duplication would just be for the sake of parity, which we don't do." Per-task attribution stays available in `/proc/<pid>/maps` for any tool that wants the other view | The ledger answers "how many holds keep this busy?" without counting ghosts | /sys/openfiles, lsof(1) |
| df(1) reads statfs(2) | df reads `/sys/mounts` — a FILE (the /proc/self/tty doctrine): one line per mount with fstype, device, GUID, mode, totals from each fs's `space` fop. lsblk reads `/sys/block` the same way | One `cat` answers what a syscall would; the tools are trivially HIS to write | sysfs.c |
| Path hygiene scattered everywhere | ONE canonicalization point (`vfs_canonicalize_path`, at chdir/open); cwd stored canonical, kernel-owned | Path hygiene happens in exactly one place | ABI #15 |
| Mouse on a text console is a USERLAND DAEMON (gpm, 1994) reading /dev/mouse and handing the selection back through `ioctl(TIOCLINUX)` | In the kernel, beside the pieces it needs: `vt_select.c` reads the events the compositor already drains, the console's own cell grid, and the kernel clipboard | gpm was a daemon because of where Linux's pieces LIVED — the mouse was a character device with no in-kernel consumer, while the highlight and the paste were always kernel code. os64 holds all three on one side already; a daemon would mean inventing a /dev/mouse to export events and a control path to feed them straight back in. The seam stays open if selection POLICY (word/line modes — gpm's real value-add) ever wants userland | CLIPBOARD.md § slice 4, vt_select.h |
| A clipboard is a windowing-system API (X11 selections, Win32 `OpenClipboard`) with TWO buffers — PRIMARY and CLIPBOARD | ONE buffer, and it is a FILE: `/sys/clipboard`. `cmd > /sys/clipboard` copies, `cat /sys/clipboard` pastes. No new syscall, no GUI dependency — a headless text VT has the same clipboard the desktop does | Content belongs in the byte-stream world, where pipes and redirection already work; forty years of X11 users pasting the wrong buffer is the argument against two. **Convergence with Plan 9**, which served snarf as `/dev/snarf` for the same reason — os64 files it under /sys because its /dev is the narrower Unix-shaped one (stateless devices), not Plan 9's service namespace | CLIPBOARD.md, sysfs.c |

---

## /proc

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| /proc = processes + cpuinfo + meminfo + sys + everything | Processes ONLY. Ever. Kernel state will get its own mount with its own name | "Designing it here is exactly how Linux's /proc happened" | PROC.md § constitution |
| Threads fudged under `task/` | Threads first-class: `/proc/N/thread/T/` | They ARE first-class in the kernel | PROC.md § namespace |
| Mixed formats (positional stat, key: value status) | TEXT, `key<TAB>value`, one record per line, everywhere | One parsing contract for ps/top/free | PROC.md § format |
| Writable knobs sprinkled around | NOTHING writable except `ctl`; reading ctl prints its own vocabulary | Self-describing control surface | PROC.md § ctl |
| `/proc/self` | Absent until a program demands it | Consumer-driven, like everything | PROC.md § not here |
| `/proc/N/mem` | `maps` for the listing; `mem` name RESERVED for byte-readable address space when a debugger wants it | Names mean things | PROC.md |

---

## The library (libos64 vs libc)

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| `strcpy`/`strncpy`/`strlcpy`/`strcpy_s`/... (50+ ways to copy a string) | ONE: `os64_strcopy(dst, cap, src)` — always terminates, returns the length src wanted | strlcpy's semantics adopted on merit, renamed (the 'l' tells a reader nothing); strcpy and strncpy deliberately absent | str.h |
| `strcmp` three-way for everything | `os64_streq` returns bool; ordering arrives when something needs to sort | "strcmp()==0 means equal" has confused people for fifty years | str.h |
| `getopt` (optind/optarg globals, argv permutation) | `os64_args`: caller-owned struct, table-driven, argv NEVER touched, `--help` falls out free | A global cursor makes two loops eat each other | args.h |
| `environ` as `char**` of "KEY=VALUE" strings | Env block: packed `key\0value\0` pairs behind a header, read-only in the address space; `os64_env_next` walks it with a CALLER-owned cursor, handing back copies | Real keys, no re-splitting, no writable shared state | env.h (abi) |
| `crt0` | `launch` | Names mean things | SUCCESSION #3, launch.S |
| libc as the world | Freestanding libos64, `<os64/...>` headers, shipped as `/lib/libos64.so` since 2026-08-22 | os64 owns the whole world here | LIBOS64.md |
| `ld.so` — a userspace dynamic linker named in every dynamic binary's `PT_INTERP`, exec'd by the kernel, which then loads the real program | **THE KERNEL IS THE DYNAMIC LINKER.** No ld.so, no PT_INTERP (app.ld discards `.interp` outright), no auxv rendezvous. `shared_object.c` reads, relocates and caches each page on first touch | SunOS 4.0 put the linker in userspace in 1988 so it could be *replaced*; os64 wants the opposite — ONE loader that can hand the same physical page to every task that maps the library. A per-process userspace linker relocates per process and shares nothing | shared_object.h, app.ld |
| Lazy PLT binding through `_dl_runtime_resolve` | Every relocation on a page is applied eagerly the first time that page is touched. There is no resolver trampoline to enter | Lazy binding buys startup time by adding the single most fragile mechanism in dynamic linking. Per-page laziness already gets the win — an untouched page costs nothing — without a runtime resolver at all. (It is also where libChrisOS's internal calls used to crash) | LIBOS64 § fingerprints |
| Libraries export variables freely; non-PIE executables reference them through `R_X86_64_COPY` relocations | **libos64 exports functions, not variables.** Zero COPY relocations exist anywhere in os64, and the one global that would have needed one (`__os64_env`) became a function argument instead | A COPY relocation gives the executable its own copy of a library's variable and quietly redirects the library's references to it. It is the ugliest corner of the format, and os64 got to skip it entirely for the price of one parameter | env.c, launch.S |
| Every executable at the same address (`0x400000` non-PIE, random bias for PIE) | Every app at its OWN fixed base, hashed from its name | Only because the debugger sits *outside* the MMU: host GDB on QEMU sees one flat address space and cannot tell two processes apart. A no-MMU technique paid for by a machine that has an MMU. Retires the day an in-OS debug server exists | app_bases.py |
| `LD_LIBRARY_PATH`, `RPATH`, `RUNPATH`, a search-path algorithm with cache | `DT_NEEDED` name → `/lib/<name>`. One directory, no search, no cache, no environment override | Nothing has asked for a second answer, and every mechanism above exists to resolve a conflict os64 does not have | shared_object.c |
| Buffered stdio as the default | Raw handles first; buffered layer is a future second phase | Consumer-driven growth | LIBOS64 § gaps |
| `char`/`short`/`int`/`long` where width matters | stdint names everywhere in new code; no u8-style aliases either | The width IS the contract | house convention |
| snprintf's (dst, size) vs strcpy's (dst, src) argument chaos | (destination, its cap, source) everywhere — buffer and bound always adjacent | One shape to remember | str.h, fmt.h |

---

## The shell (husk)

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| Fat shells (builtins that duplicate utilities) | husk does NOTHING a real program can do | Zero feature duplication, thin forever | husk.c header |
| `$PWD` env var (drifts from reality) | `$CWD` fetched LIVE at expansion; the env seed that lied was deleted | A cache that can lie is worse than a syscall | husk expand_line |
| `kill %1` job notation | Job reports print job number AND task number — the task number IS the handle you type into `/proc/N/ctl` | No second process-naming scheme | husk job_announce |
| Shells glue newlines onto ragged output | Shell adds NOTHING; GENERATORS terminate their own last line, FILTERS are byte-faithful (`cat < a > b` must equal `a`) | State it the two-clause way — the short form caused a real bug | roadmap memory 7/19 |
| `2>&1` binds POSITIONALLY — stderr gets whatever stdout was bound to at the moment the token was read, so `cmd 2>&1 > f` leaves stderr on the terminal | `2>&1` (and `>&2`, `&>`) bind LAST, to the other stream's final destination — file, pipe or console — wherever on the line they were written | Forty-five years of tutorials carry a paragraph explaining the positional rule; nobody has ever *meant* it. Chris, 2026-08-28: "the other way just sounds annoying." The rest of the vocabulary is Bourne's verbatim: the digit is the handle number, `2>>` appends, `&>` is bash's 1989 both-streams shorthand | husk extract_redirections |
| Two kinds of variable: shell variables are private until `export`ed into the environment (Bourne 1977; csh's `set` vs `setenv`) | ONE kind. `NAME=value` sets an environment variable; `$NAME` reads it; children inherit it; `export NAME=value` is the same act under its older name | The split was a PDP-11 economy (the environment is copied at every exec) that became a silent trap — a child that cannot see a value says nothing, so everyone exports everything and pays for a distinction they never use (Chris: "95% of the time I used export"). One namespace means a variable means the same thing at the prompt, in a script, in a `for`, and in the child. Costs, accepted: a loop counter leaks into children's environments; nothing is kept from a child by default — `env -u NAME command` withholds one variable from one program, and the day os64 has secrets in scripts, the answer is a `local`-shaped verb, not a private tier. Refused on purpose: a case-of-the-name rule (uppercase flows, lowercase stays) — magic hiding in spelling. Ruled 2026-08-28 | husk run_assignment |
| `` `cmd` `` backquote substitution, and `$(cmd)` forks a SUBSHELL | `$(cmd)` only (Korn 1983 — nests, quotes sanely); the inner line runs IN THIS SHELL, one expansion depth down | No second spelling for one idea. In-process because the inner text must reach exactly one parser — expanding it in the outer shell and again in a `husk -c` child would hand substituted values to the second parser as syntax (PR #28's rule). Consequences, deliberate: a builtin inside `$()` acts on THIS shell (`$(cd /x)` moves you) and its output is not captured (builtins write to the console, not through the capture pipe — `$(pwd)` works because pwd is a program); `$?` after the line is the LINE's — with Bourne's one exception, adopted 2026-08-29 when the first `x=$(cmd)` needed its verdict: a bare assignment answers with the substitution's status (the value IS the command there), while `echo $(false)` stays echo's 0 | husk run_substitution / run_assignment |
| Bracketed paste (`ESC[200~`/`ESC[201~`, xterm 2002) so the shell can refuse to run a pasted newline | NOT IMPLEMENTED, deliberately. gterm's right-click writes the snarf to the pty master as if typed; a pasted newline RUNS the line | Chris's ruling, 2026-08-21: "I am a smart guy. If I want to run multiple lines I copy multiple lines. If I want to review first, I review before copy or copy one line at a time, no newline." The wrapper exists so a shell can second-guess its user; os64 has one user and he is not asking for that. (The copy side helps: a one-row copy carries no trailing newline, so it lands on the prompt un-run) | CLIPBOARD.md § slice 3, gterm.c |

---

## Scheduled work (cron, 2026-08-29)

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| Five compressed fields — `0 3 * * 1` — where day-of-month and day-of-week are OR'd, not AND'd | Words, left to right: `@reboot`, `every 5m`, `every 2h`, `daily 03:00`, `weekly sun 04:30` | The five fields are compression for a file parsed by a tiny C program and typed on a teletype. Nobody reads `*/5 2-4 * * 1-5` right the first time, and the dom/dow OR has surprised people for forty-five years. What os64 gives up is range and step expressiveness; it buys that back the day something asks for it | cron.c header |
| cron runs in LOCAL time, so DST gives it an hour that never happens and an hour that happens twice — a real source of jobs that run twice or never, which Vixie documented and worked around rather than solved | UTC, always, read straight from the kernel epoch rather than through anybody's `TZ` | The system clock is UTC by ruling (the sysfs arc); a UTC cron simply cannot have the bug. Cost, stated in `/etc/crontab` rather than hidden: `daily 03:00` is 03:00 UTC | cron.c `utc_now` |
| `*/5` is a calendar match; interval schedulers elsewhere measure from last run | `every 5m` is a CALENDAR MATCH too — minute % 5 — which makes every schedule STATELESS | Due-ness becomes a question about the clock alone, so nothing survives a re-read and a restarted cron is exactly where a running one would be. The wart, stated: `every 7m` is lumpy at the hour (60 ∤ 7). No cron has ever been otherwise | cron.c `job_is_due` |
| Per-user spools plus `crontab(1)` to manage them | One file, found on the config search path, and no command | `crontab(1)` exists to enforce per-user permissions on a spool directory. os64 has neither users nor a spool, so the command would be a wrapper around `scribe` | cron.c header |
| `/etc/crontab` is one file; user tables live elsewhere | `/home/crontab` MERGES on top of `/etc/crontab` | A crontab is a LIST, not a setting — the `hosts` treatment (Chris, 2026-08-22), for the same reason: your jobs should join the system's, not erase them. Consequence, stated in the file: adding a crontab cannot turn a system job OFF, only add to it | cron.c `load_crontabs` |
| cron MAILS a job's output to its owner | Output goes to the console and the log; the exit status is logged for every job, not just failures | There is no mail. The log is where you go looking for "what happened while nobody was watching", and "nothing was printed" is not an answer you can act on | cron.c `reap_finished` |
| `@reboot` is a scheduling verb because Unix already had rc scripts and Vixie needed a hook an unprivileged user could reach | `@reboot` is os64's ONLY boot hook, because os64 has no init at all | Kept knowingly, not inherited: `husk.rc` runs in EVERY shell (VT1 and VT2 both), so a system service started there starts twice — the bug that moved the desktop's startup list out of it. The day os64 grows an init, cron becomes its tenant and this row gets revisited | kernel.c cron launch |

---

## Kernel-internal doctrine (not ABI, but porters should know)

| Convention elsewhere | os64 | Why | Recorded |
|---|---|---|---|
| `free(NULL)` is a no-op | `kfree(NULL)` PANICS **in the kernel**; userland `os64_free(NULL)` IS a no-op, as it has been since V7 | Kernel-side, NULL is a wild pointer's favorite disguise; ring-3 side, refusing it would only make every caller write the same `if` | CLAUDE.md fingerprints / MALLOC.md |
| malloc returns dirty memory | **Kernel:** EVERY allocation zeroed at the allocator choke point. **Userland (2026-08-15):** a fresh block is kernel-zeroed and a RECYCLED one holds 0xA5 poison — `os64_malloc` promises neither, `os64_calloc` promises zero and skips the memset when the memory is provably virgin | One kernel choke point kills the "uninitialized" bug class; at ring 3, poison catches use-after-free (a tripwire beats a favor), and the zeroed-region guarantee makes calloc free on first touch | CLAUDE.md / MALLOC.md |
| Heap statistics need LD_PRELOAD or a debug build | `cat /proc/<pid>/heap` — malloc publishes one struct, the kernel renders the file, `watch` makes it a live profiler | The allocator is the only thing that knows its own shape; a report is cheaper than an interposition mechanism | abi/os64/heap.h, PROC.md |
| Eager full direct map (Linux) | Lazy HHDM: mapped exactly while allocated; freed memory FAULTS on touch | A designed use-after-free tripwire | CLAUDE.md § HHDM |
| KPTI (unshared kernel half) | Shared U/S-protected kernel upper half | Deliberate for os64's threat model: we run our own binaries | DEBTS not-debts |
| Log rings drop-and-count | NEVER drop a byte — buffers grow (16MB/core), the drainer hands off to a ring-3 daemon rather than paying 115200 baud, and panics write serial direct + emergency flush. **REFINED, not reversed, 2026-08-18:** at the WALL — ring full, nothing draining, growth impossible — the rings now keep the NEWEST entry, overwrite the oldest, and COUNT it (`buffer->lost`, reported by /sys/log). What that replaced was a PANIC after two billion spins, so the honest framing is Chris's own: *"never drop a byte is aspirational."* The choice at that moment is not whether to lose lines but which, and a panic loses all of them plus the machine. Drop-and-count remains rejected as an operating MODE; this is the behavior at the end of the road, and it is loud | Chris's motto, load-bearing — and the corollary decides the tie: the byte that matters most is the last one | SUCCESSION #4; log.c `LOG_FULL_PATIENCE_SPINS` |

---

## If a Linux-compat layer ever happens (the shim's shopping list)

- **Syscall personality**: Linux numbers/semantics → os64 table at the
  boundary (the FreeBSD Linuxulator / WSL1 shape). Biggest fights: errno
  synthesis from the RAX:RDX pair, `struct stat`/`dirent` translation,
  brk emulation over map() (a fake heap region), kill → ctl writes.
- **Name-shim library** for source/link-level ports: the strcpyXYZ family →
  `os64_strcopy` et al. (glibc itself finally adopted strlcpy in 2.38/2023 —
  the direction of travel is ours.)
- **The rows above are the spec.** Anything in "Kept on merit" is free;
  everything else is a translation entry.
