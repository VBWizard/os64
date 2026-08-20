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
| Small tools composed by pipes | The whole userland doctrine |
| `\|` `<` `>` `&` shell notation | `<` predates `\|` by two years; `&` is Thompson 1973 |
| `$?`, exit code 0 = success | Bourne 1977; last pipeline stage speaks for the job |
| 128+signo death codes (130, 137) | Human-legible corpse tags, kept even though signal *bits* diverged |
| `cd` as a builtin | Inherited cwd at spawn makes it structural, same as every shell since V6 |
| The `-NUM` count form (`tail -40`, `head -20`, `grep -3`) | POSIX marks it OBSOLESCENT and it stays anyway, as an opt-in `numeric_alias` row in os64_args. The ambiguity POSIX cites — collisions with negative operands and with option bundling — does not exist in this grammar (bundles are letters; no program declares a digit as an option letter), so the objection is inherited, not earned. Decades of muscle memory is a real interface requirement in an OS with one daily user (args.h, 2026-08-01) |
| cwd-first, then PATH walk; argv[0] stays as typed | V6 behavior, colon-separated PATH (V7's gift) |
| Ctrl+letter = control codes; Ctrl+D = EOT = EOF | 1963 semantics, done properly (one-shot EOF, then normal reads) |
| ELF, SysV x86-64 calling convention | Interop with the toolchain — a *specific reason*, per the philosophy |
| Syscall args in RDI/RSI/RDX/R10/R8/R9 | Hardware forces R10-for-RCX; happens to match Linux |
| ext2 on-disk format | FFS re-expressed; FULL read/write since 2026-08-04, judged by the host's own e2fsck (`make fsck-ext2`); root stays mounted read-only until ratified (FAT keeps boot/interop forever) |

---

## Syscalls

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| `errno` (global, action at a distance) | In-band errors only; designed convention = RAX:RDX value:status pair (interim: two high sentinels) | The error check belongs at the call site | ABI § register contract |
| `lseek` (whence + offset dance, return needs care) | `seek` returns the NEW absolute position; `SEEK_END+0` = file size, no fstat needed | The useful answer, directly | ABI inventory #10 |
| `getdents` + per-entry `stat` dance; `struct dirent`'s poverty | `readdir` yields name+size+kind in ONE call; returns 1/0/<0; EOF is sticky | One call, whole answer | dirent.h, ABI #11 |
| Separate `struct stat` | `stat` fills the SAME `os64_dirent_t` readdir yields — "stat is readdir for exactly one name" | One struct, no fork | ABI #23 |
| `readdir` delivers `.` and `..` | Never. They are not directory content | Plan 9 was right | SUCCESSION #7 |
| `open` flag ints (O_RDONLY\|O_CREAT\|...) | Mode strings "r"/"w"/"a"/"c", "d" for directories; unknown modes REFUSED at the boundary | Tripwires over silence | ABI #9 |
| `O_DIRECTORY` / opendir as separate machinery | Directories open with mode "d", same open, same close, one handle type | One door | ABI #9 |
| `brk`/`sbrk` | NEVER. `map(len)`/`unmap(base)` regions: demand-paged, zeroed, guard page after, VAs never reused | brk frees only from the end; stale pointers must fault | DEBTS not-debts, MALLOC.md, ABI #12-13 |
| `waitpid(WNOHANG)` | `reap` is its OWN syscall; returns 0 = "nobody died" as an ordinary answer | A wait that doesn't wait is a name that lies | syscall.c reap, task.h |
| `/proc/meminfo` (text file, parsed with string code; "free" changed meaning when the page cache arrived; MemAvailable = 2014 errata for 1992 fields; linuxatemyram.com) | `memory(out)` syscall fills a typed struct: every field's meaning fixed FOREVER — `reclaimable` is the future page cache's seat (0 today, honestly), `available` = free+reclaimable summed BY THE KERNEL, `free + used == usable` is a live audit any program can check | Numbers may change; meanings never do. Userland never does column arithmetic | os64/memory.h, ABI |
| `kill(2)` | Does not exist and never will. `echo kill > /proc/N/ctl` | kill(pid, SIGCONT) resumes a process — the name has lied since 1971; ctl is self-describing | PROC.md § ctl |
| Unknown flags silently ignored (much of POSIX) | Unknown spawn flags / open modes / ctl verbs REFUSED | A silently dropped request "succeeded" and didn't | syscall.c boundaries |
| `unlink(2)` + `rmdir(2)`, two removal verbs | ONE verb: `unlink` removes files AND empty directories; rmdir will never exist (ratified 2026-08-04) | POSIX's split is a pre-4.2BSD scar (setuid rmdir(1) hand-unlinking `.`, `..`, then the entry); Plan 9's `remove()` got it right | abi syscall_numbers.h #35, ABI #35 |
| `unlink` of an open file succeeds (blocks freed at last close) | **CONVERGED 2026-08-16** — os64 now does the same for regular FILES, and `rename` over an open file likewise. An open DIRECTORY still refuses | Refused from 2026-08-04, explicitly consumer-driven ("built the day a consumer needs it"). The consumer arrived: replacing `/bin/husk` over the network while husk is running — a task holds its own ELF open for demand paging. Converged on MERIT, not inheritance: the inode outliving its name is what makes a file a file rather than a directory entry. Kept honest by the CRASH story rather than the happy path — the orphan list lives on disk in `s_last_orphan`, a mount replays it out loud, and e2fsck already knows that field | ext2_write.c orphan chain / VERIFICATION.md crash procedure |
| `rename(2)` replaces almost anything: file over file, empty dir over empty dir; some systems even swap | REPLACEMENT IS FILE-ONTO-FILE ONLY. A directory is never replaced, and a directory never lands on an existing name. The atomicity is KEPT (ratified 2026-08-16) — that half of rename is the good idea | The window rename(2) was invented to close (4.2BSD, 1983, because link-then-unlink couldn't be atomic) is worth every line; the directory-replacement corners are where POSIX's rename grew its `..`-and-link-count folklore, and no os64 consumer has asked for them | abi syscall_numbers.h #43, ABI #43 |
| `rename(2)` across filesystems fails with EXDEV and the caller copies | Same line, same reason — refused at the syscall boundary, and `mv` does copy-then-unlink in userland | A convergence kept on MERIT, not inheritance: a half-finished copy needs a cleanup policy (retry? keep the partial? prompt?), and policy belongs where a human can be asked | syscall.c syscall_rename |
| `rename(2)` of an open file succeeds | **CONVERGED, same day it was written (2026-08-16)** — open FILES rename freely on both sides; an open DIRECTORY refuses | Shipped that morning REFUSING both sides, inheriting rm's 2026-08-04 ruling. By evening the orphan chain had shown the inheritance was wrong on the source side (an ext2 handle holds an inode number, not a path, so a reader cannot even perceive a rename) and unnecessary on the destination side (the displaced inode simply survives, nameless, until last close). A directory handle IS mid-walk through what a move re-parents, so that one stands | ext2_write.c ext2_rename |
| `fsync(2)` matters (write-back caches defer everything) | ext2 writes are FULL WRITE-THROUGH: data + inode committed before write() returns; `sync` is an already-kept promise | No block cache to flush, and a file being appended reads at true length to everyone immediately — honestly better than FAT's looks-empty-until-sync (which is why sync exists at all). A future block cache re-earns sync | ext2_write.c durability doctrine |
| `O_NONBLOCK` open flag + `EAGAIN` errno; `SO_RCVTIMEO` where 0 = block forever | read() carries its patience as an argument: 0 = wait zero ms (poll), N = deadline, `OS64_WAIT_FOREVER` = block; empty wait returns `OS64_ERR_TIMEOUT`, its own verdict | Ruled 2026-08-05. V7's O_NDELAY let an empty read return EOF's 0 — a decade-long in-band lie POSIX had to apologize for with O_NONBLOCK/EAGAIN; and SO_RCVTIMEO's 0-means-forever makes zero's one honest meaning unsayable. os64 follows the poll()/select() tradition: zero means zero, forever is spelled out. Refused (not ignored) on handles that don't honor it | abi syscall_numbers.h #4 |

---

## Processes & jobs

| Unix/Linux | os64 | Why | Recorded |
|---|---|---|---|
| fork+exec as the only birth | `spawn(path, argv, in/out/err, flags)` first-class beside fork | The everyday launch deserves one syscall | ABI #5 |
| Child inherits the WHOLE fd table | NO blanket inheritance: console + exactly the handles asked for | Kills the classic held-open-pipe-end pipeline hang | DEBTS not-debts |
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
| fstab + mount(2) | Mount table auto-mounts recognized partitions at "/<fstype>", longest-prefix routed, GUID-deduped, foreign partitions allowlisted OUT | Boots self-describing; Windows-safety by construction | vfs.c, CLAUDE.md |
| Path hygiene scattered everywhere | ONE canonicalization point (`vfs_canonicalize_path`, at chdir/open); cwd stored canonical, kernel-owned | Path hygiene happens in exactly one place | ABI #15 |

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
| libc as the world | Freestanding libos64, `<os64/...>` headers, planned .so with hidden-alias internal calls | os64 owns the whole world here | LIBOS64.md |
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
