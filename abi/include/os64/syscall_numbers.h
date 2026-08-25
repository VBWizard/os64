#ifndef OS64_ABI_SYSCALL_NUMBERS_H
#define OS64_ABI_SYSCALL_NUMBERS_H

// os64 syscall numbers — THE kernel↔userland contract, shared by the kernel
// (dispatch table, exit-trampoline asm) and every userland binary (via
// libos64 or direct stubs). This lives in abi/, deliberately OUTSIDE kernel
// source: userland must never reach into kernel headers (the old OS proved
// how ugly that gets), and the kernel must never depend on userland. Both
// depend on THIS.
//
// This header must stay PREPROCESSOR-ONLY (bare #defines, no typedefs, no
// prototypes): it is included from assembly on both sides (the kernel's
// ring-3 exit trampoline template, userland's launch.S).
//
// os64 syscall convention (ours, not Linux's — numbers and semantics are
// free to diverge; see ABI.md for the full register contract):
//   RAX = syscall number, args in RDI, RSI, RDX, R10, R8, R9, result in RAX.
//   (R10 stands in for RCX, which the CPU burns for the return RIP.)

#define SYSCALL_YIELD      0
#define SYSCALL_DEBUG_LOG  1
#define SYSCALL_EXIT       2
#define SYSCALL_WRITE      3
#define SYSCALL_READ       4
#define SYSCALL_SPAWN      5
#define SYSCALL_WAIT       6
#define SYSCALL_PIPE       7
#define SYSCALL_CLOSE      8
#define SYSCALL_OPEN       9
#define SYSCALL_SEEK       10
#define SYSCALL_READDIR    11
#define SYSCALL_MAP        12
#define SYSCALL_UNMAP      13
#define SYSCALL_GETCWD     14
#define SYSCALL_CHDIR      15
// 16-22 are RESERVED for the GUI block (see below) — defined before stat
// arrived, and reserved means reserved. File syscalls resume at 23.
#define SYSCALL_STAT       23
#define SYSCALL_REAP       24

// ── read's patience: the 4th argument (ruled 2026-08-05) ────────────────────
// read(handle, buf, len, timeout_ms) — arg3 says how long the call may WAIT
// for a byte to exist, in milliseconds, and it means what it says:
//
//   0                 — wait ZERO ms: bytes if any are ready, OS64_ERR_TIMEOUT
//                       if none, never a block. (The poll gait — what lets
//                       top watch for 'q' between refreshes.)
//   N                 — wait up to N ms, rounded UP to the tick like sleep();
//                       OS64_ERR_TIMEOUT if the deadline passes byteless.
//   OS64_WAIT_FOREVER — classic blocking read. libos64's plain os64_read
//                       says this out loud; os64_read_for exposes the dial.
//
// Two pieces of history shaped this, one honored and one refused. Refused:
// V7's O_NDELAY (1979) made an empty non-blocking read return 0 — the same 0
// that means EOF — and that in-band lie festered for a decade until POSIX
// invented O_NONBLOCK/EAGAIN as the apology. Here an empty wait returns
// OS64_ERR_TIMEOUT, a verdict no other outcome shares, so "nothing yet" and
// "nothing ever again" cannot be confused by construction. Also refused:
// SO_RCVTIMEO's 0-means-forever, the wart tradition where the one honest
// meaning of zero is unsayable. Honored: the poll()/select() tradition
// (0 = now, forever spelled out), which is the family this call belongs to.
//
// The timeout is REFUSED (not silently ignored) on handles that don't honor
// it — a pipe read that accepts a patience it won't keep is a lie with a
// delay. The console honors it today; pipes and the net's conn handles grow
// or carry it the day their consumers demand (the net branch already
// speaks this contract on udp/tcp/icmp).
#define OS64_WAIT_FOREVER  UINT64_C(0xFFFFFFFFFFFFFFFF)   // ((uint64_t)-1)

// The empty-wait verdict: the deadline expired with nothing to show. Shares
// its value with the net branch's OS64_NET_ERR_TIMEOUT (-10) on purpose —
// when the branches merge, the net code aliases to THIS name, because a
// timeout stopped being a network concept the day the console learned one.
#define OS64_ERR_TIMEOUT   (-10)

// sleep(ms) — park the calling thread for AT LEAST `ms` milliseconds.
// The ABI speaks TIME; the kernel speaks ticks — the conversion happens at
// the boundary, rounding UP to the ACTIVE scheduler interval (minimum one
// tick), so granularity tracks the kernel's tick rate automatically and no
// tick constant ever leaks into a compiled binary. Unix needed four
// generations of this call (sleep/usleep/nanosleep/clock_nanosleep) because
// the units kept being wrong; one syscall, milliseconds, honest floor.
// sleep(0) is the documented free yield — no time, but the CPU goes back.
// Returns 0 always: the only interruption that exists today is death, and
// the dead read no return values (remaining-time semantics deliberately
// wait for the SIGNALS.md EINTR-vs-restart ruling).
#define SYSCALL_SLEEP      25

// ticks(out) — fill an os64_ticks_t (os64/ticks.h) with the monotonic tick
// count since boot AND the active tick rate. The stopwatch, not the
// calendar — see the header for the doctrine and the everyday arithmetic.
#define SYSCALL_TICKS      26

// memory(out) — fill an os64_memory_t (os64/memory.h) with the physical
// memory picture: total/usable/free/reclaimable/available + largest free
// extent + live page size. One atomic snapshot under the allocator lock.
// Every field keeps its meaning forever — see the header for why that
// sentence took Linux 22 years (linuxatemyram.com, MemAvailable, 2014).
#define SYSCALL_MEMORY     27

// printat(x, y, str) — park a string at an absolute character CELL on the
// physical console, without touching the console cursor: the WIDGET PLANE
// (known since 2026-08-19 as the SCREEN LAYER — libui claimed the word
// "widget" for ring-3 UI, and the ptys proved this doctrine from the other
// side the same week: a clock in a gterm session landed on the text VTs,
// exactly as the "it does not travel" paragraph below always said. The
// userland identifier is os64_screen_printat now; this syscall's number and
// table name are wire-stable and keep their birth name).
//
// This is deliberately NOT a console write and NOT cursor addressing. A
// status widget (the uptime clock in the top-right corner) parks glyphs at
// fixed coordinates; it has no business borrowing the console's shared
// cursor, which another core is using to echo somebody's keystrokes. No
// cursor motion, no wrap, no scroll — the string clips at the screen edge,
// because a widget that overflows its corner should be truncated, never
// allowed to reflow the console. (The kernel's own clock learned all of
// this the hard way; see print_at() in BasicRenderer.c.)
//
// LAYERING DOCTRINE, decided before virtual terminals exist so they don't
// have to rediscover it: the widget plane lives OUTSIDE the terminal stack.
// When VTs arrive (F1-F9), switching terminals swaps console content UNDER
// the widgets and never disturbs them — the clock survives an F3 the same
// way it survives a scroll. And it does not travel: an SSH user is not
// looking at this machine's glass, so the widget plane correctly does not
// exist for them. Anything that IS terminal content — top's repaints, a
// future vi — belongs to the in-band escape-sequence slice instead, because
// bytes-in-the-stream survive pipes, VT buffers, and wires; syscalls don't.
#define SYSCALL_PRINTAT    28

// time(out) — fill an os64_time_t (os64/time.h) with the wall clock's raw
// truth: UTC epoch seconds, the configured timezone offset, and the
// sub-second tick phase, one consistent snapshot. The CALENDAR is not here
// on purpose — the kernel keeps a counter; what a "March" is belongs to
// libos64 (<os64/date.h>), exactly the split Unix picked and kept. (It took
// them three tries: First Edition time() returned SIXTIETHS of a second in
// 32 bits and wrapped every 2.26 years. Epoch seconds, UTC, 64 bits — we
// start where they landed.)
#define SYSCALL_TIME       29

// setenv(key, value) — set (or remove) one variable in the CALLING task's
// environment. value = NULL removes the key (idempotent: unsetting the
// absent succeeds, as `unset` has since Bourne). The env block is the same
// physical page the task sees read-only at its env mapping, so the change
// is visible to the caller's own getenv immediately — and env_inherit
// hands it to every child spawned AFTER this call. Children spawned BEFORE
// keep their snapshot: environments flow down at spawn time, never
// sideways (the one-way valve that makes export a shell BUILTIN — an
// external `export` program would set its own copy and take it to the
// grave). Fails only when the env block is full.
#define SYSCALL_SETENV     30

// klog_read(entries, max) — take up to `max` kernel log entries, oldest
// first across every core, into an os64_logent_t array (os64/klog.h).
// Returns the count taken (0 = nothing waiting right now, so the caller
// sleeps; negative = refused). Entries are REMOVED from the kernel rings
// by this call — reading is consuming, because the reader has taken
// responsibility for them.
//
// Calling this CLAIMS the log: the kernel's own logd stops draining to
// serial while a reader is live, which is the whole point (steady-state
// logging moves off a 115200-baud wire and onto a file). The claim is a
// heartbeat — a reader that dies or hangs loses it within seconds and the
// kernel resumes serial by itself. Mechanism here, policy (where the
// bytes go) in the daemon; see os64/klog.h for the argument.
#define SYSCALL_KLOG_READ  31

// sync(handle) — commit a written file to the device: the bytes AND the
// directory entry that says how long the file now is. Returns 0, or
// negative if the handle isn't a file or the filesystem can't sync.
//
// This exists because "I wrote it" and "anyone else can read it" are two
// different claims on a FAT volume. FatFs holds the new length in memory
// and writes the directory entry on sync or close, so a file being
// appended to by a live process reads as EMPTY to every other program
// until one of those happens — which is precisely how a log file looked
// like nothing was being logged while the daemon wrote to it steadily.
// Consumer-driven, like every syscall here: logd is the program that
// needed durability, so durability got a name.
#define SYSCALL_SYNC       32

// thread(entry, arg, exit_stub) — start a second line of execution inside
// THIS task, sharing everything: the same address space, the same heap,
// the same open handles. Returns a HANDLE, or negative.
//
// Reading that handle blocks until the thread finishes and yields its
// return value (an int64_t); closing it means "I don't care what you
// return." That is the whole API — no wait verb, no detach verb, no
// thread-id-reuse hazard, because the handle model already means all
// three (the same move the network listener makes: read IS the wait).
//
// exit_stub is a USERLAND address, supplied by libos64: the kernel seeds
// it as the return address on the new thread's stack, so a thread
// function that simply returns lands there and the stub calls
// thread_exit with the value in RAX. It cannot be a kernel address —
// ring 3 would fault the instant it tried to return into one.
//
// Threads share an address space, so they need NO copy-on-write; that is
// fork's problem, and fork is the next customer for this same plumbing.
#define SYSCALL_THREAD      33

// thread_exit(retval) — end the CALLING thread only, recording retval for
// whoever reads its handle. The task lives on while other threads run.
// A program's main thread returning still ends the whole task (exit means
// exit — Chris's ruling, 2026-08-02): threads do not keep a dead process
// breathing.
#define SYSCALL_THREAD_EXIT 34

// unlink(path) — remove a file OR an empty directory: os64's ONE removal
// verb. Named unlink because that is what the operation honestly is — the
// DIRECTORY ENTRY goes away and the storage follows — and that sentence is
// just as true for a directory as for a file. The program that calls it is
// free to be called rm.
//
// There is deliberately NO rmdir and never will be (ratified 2026-08-04,
// the day rm -r shipped and proved the contract). This is Plan 9's shape —
// one remove() for both — not POSIX's, whose unlink/rmdir split is a scar,
// not a design: before 4.2BSD, rmdir(1) was a setuid-root program that
// unlink()ed ".", then "..", then the entry itself, three raw steps a crash
// could leave half-done, and when 4.2BSD made directory removal atomic it
// kept the two verbs it had inherited. os64 declines the scar. Every
// filesystem's rm op owes this same contract — FatFs's f_unlink grants it
// natively, and the ext2 write driver was built to it the same afternoon
// the contract was ratified (2026-08-04: ext2_rm, promise kept).
//
// Returns 0 on success. Refused, not half-done: a read-only filesystem
// (os64's ext2 ROOT mount stays read-only until ratified writable — the
// driver itself writes since 2026-08-04), a path that isn't there, a file
// or directory another handle holds OPEN (ext2 refuses rather than racing
// the reader), or a directory that still has contents — emptying it first
// is the caller's
// job, which is what rm -r's depth-first walk is.
#define SYSCALL_UNLINK 35

// mkdir(path) — create a directory. Returns 0, or negative: read-only
// filesystem (ext2 by design), a parent that doesn't exist, or a name
// already taken. One call, atomic, done.
//
// That last word is the whole history: Unix went its FIRST DECADE without
// this syscall. V6/V7's mkdir(1) was a SETUID-ROOT PROGRAM that built a
// directory out of three separate privileged steps — mknod(), then link()
// for ".", then link() for ".." — and a crash (or a well-timed signal)
// between them left a half-wired directory for fsck to untangle. 4.2BSD
// (1983) finally made it one atomic kernel operation. os64 starts where
// they landed. Removal is not mkdir's mirror here: unlink (above) is the
// one removal verb for files AND empty directories, so mkdir needs no
// rmdir twin — and will never get one.
#define SYSCALL_MKDIR 36

// debug_log() FLAGS — arg1. Zero is the everyday call: the message goes to
// the kernel log rings, tagged "[user]", and travels wherever the log
// travels (serial drainer, or a claimed logd's file).
//
// SERIAL puts the line on the serial WIRE too, immediately and directly —
// the same door panic() uses — regardless of who has claimed the log. This
// exists because the log pipeline WORKING is what broke watching a live
// boot from outside: once logd claims the log, every marker lands in a file
// on a disk image the host can't read until shutdown. A verification
// harness (or a human tailing the wire) needs a handful of beacons that
// cannot be redirected. Use it for beacons, not for logging — every byte
// here costs a VM exit, which is the exact bill logd exists to avoid.
#define OS64_DEBUG_LOG_SERIAL  0x1

// net_dial(dest) — open a network conversation. arg0 = const os64_netdest_t*
// (os64/net.h): WHERE (ip, host order — ruling #2: the kernel owns the wire),
// WHICH DOOR (port), HOW (protocol). Returns a handle you read() and write()
// like any other, or negative. The Plan 9 dial STRING ("udp!10.0.2.2!53")
// never crosses this boundary — libos64's os64_dial() parses it into this
// struct (ruling #1: kernel speaks structs, the library speaks strings).
//
// THE SCAR, now with two rings (2026-08-05). This call was 28 for a few
// uncommitted hours, then 31 for the whole life of the net branch — and
// BOTH times the userland branch had already minted those numbers in
// parallel (printat/time/setenv at 28-30; klog_read/sync/thread at 31-33).
// The rule both reconciliations followed, stated once so the third time is
// cheap: TWO BRANCHES, ONE REGISTRY, and the merge cedes the numbers to the
// ELDER commits. A syscall number is not a name — it is an index into a
// table that ring 3 compiles against, so whoever shipped it first keeps it
// and the newcomer moves. Renumbering the newcomer costs one rebuild;
// renumbering the incumbent costs every binary ever built.
#define SYSCALL_NET_DIAL   37

// sync(1)'s engine (2026-08-06): walk the kernel's open-file registry and
// run every open file's fops->sync. No arguments — the broom sweeps the
// whole floor. Returns the count of files synced (0 is a legal, honest
// answer on an idle system), negative if any individual sync failed after
// the sweep still visited everyone. Exists because FAT defers a file's
// directory-entry size until sync/close — a still-open ping.log reads as
// empty to every fresh open until SOMEBODY syncs the writer's handle, and
// only the kernel can reach another task's handles. sync(8) has meant
// exactly this since First Edition Unix (1971); the operator's liturgy
// ("sync; sync; sync") predates most filesystems it saved.
#define SYSCALL_SYNC_ALL   38

// shutdown(8) (2026-08-08 — the day the P5's writable root made the power
// button a filesystem event). arg0 = the VERB: 0 = power off, 1 = REBOOT
// (BSD's shutdown -r; System V spelled the same idea `shutdown -i6`, which
// is all the argument anyone needs for not taking design cues from System
// V). Verb 1 went from reserved to real on 2026-08-21 — and the day it did,
// every binary already built passed the right register, because the wrapper
// has passed 0 EXPLICITLY since day one instead of leaving ring-3 garbage
// where the verb goes. AN UNKNOWN VERB IS REFUSED, not rounded down: this
// call never returns, which makes it the worst possible moment to be
// charitable about input. Does not return. The kernel runs the whole
// descent: asks every task to stop (SIGTERM, a grace period, then SIGKILL
// for whoever ignored it), retires the log daemon (final drain, file
// closed), sync_all's every open file, FLUSH CACHEs the storage devices
// (the drive's volatile cache is the one thing no fs-level sync reaches),
// then powers off or resets — or, on hardware whose ACPI we don't speak
// yet, prints the 1995 liturgy ("It is now safe to turn off your computer")
// and parks. The termination ladder runs FIRST, before the daemon retires,
// so the exits it causes are in the log it leaves behind.
// This call retires the operator ritual sync(1) existed to serve: the
// "sync; sync; sync" incantation was a human delay loop for the platters,
// and this is the machine doing its own counting.
#define SYSCALL_SHUTDOWN   39

// The verbs, defined ONCE and here — this header is the ABI, and the kernel's
// descent, the library wrapper and the utility all read these same names
// rather than three copies of "0 means off". (Chris asked for exactly this
// the day the reboot verb landed: a shared enum, not a duplicated one.)
//
// Wrapped in the assembler guard because THIS HEADER IS INCLUDED FROM .S
// FILES — syscall.S, task_exit_asm.S and launch.S all pull it in for the
// numbers, and GAS's preprocessor would choke on a typedef. Plain #defines
// would have been safe unguarded; the guard is what buys C a real TYPE
// without taking that away from assembly. (dirent.h's precedent, same trick.)
#ifndef __ASSEMBLER__
typedef enum os64_shutdown_mode
{
    OS64_SHUTDOWN_POWEROFF = 0,   // stop the machine
    OS64_SHUTDOWN_REBOOT   = 1,   // stop it and start it again
} os64_shutdown_mode_t;
#endif

// taskid() (2026-08-09 — the night after the terminals, because "which tty
// am I on?" starts with "who am I?"). Returns the calling task's ID in RAX;
// takes nothing, cannot fail. One of the oldest questions in Unix — V1 had
// getpid in 1971, before pipes, before /tmp — and the answer belongs in a
// register because the asker is already standing in the kernel's doorway.
//
// SPELLED taskid, NOT getpid (2026-08-24). Same question, older than most of
// this OS; a different noun, because os64 runs TASKS and this returns
// task->taskID. The Unix name was doubly wrong here — "pid" names a thing
// os64 does not have, and worse, it PROMISED PER-PROCESS when a reader needed
// per-thread: libos64's config writer built its temporary file name out of
// "the pid", every thread of a program got the same one, and two threads
// saving one file raced to publish each other's half-written temp. The name
// is what misled it. ("get" is gone too: it earns its keep opposite a `set`,
// and nothing sets its own identity — os64_ticks and os64_memory read
// properties the same way.) The NUMBER is untouched: 40 is the contract, the
// spelling is ours.
//
// Identity has TWO spellings on os64, on purpose, answering at two different
// moments: this syscall is the PRIMITIVE (and what husk's $$ freezes into a
// command line at expansion time, before any child exists), while
// /proc/self is the NAMESPACE spelling, resolved at open time to whoever
// does the opening — which is why `echo $$` names your shell and
// `cat /proc/self/status` names cat. Both honest; different clocks.
#define SYSCALL_TASKID     40

// set_time(epoch) — replace the running kernel's UTC wall-clock counter.
// The monotonic ticks clock remains untouched, so intervals and uptime never
// jump when an operator corrects the calendar. This sets the in-memory clock,
// not the battery-backed RTC; persistence belongs to a future hwclock tool.
// Returns 0. The int64 epoch rides in arg0 as its two's-complement bits.
#define SYSCALL_SET_TIME   41

// heap_report(ptr) — "my heap's report card lives HERE, read it whenever you
// like." arg0 is a user VA holding an os64_heap_report_t (abi/os64/heap.h);
// 0 withdraws the registration. Returns 0, or negative for a pointer the
// kernel won't take.
//
// The kernel stores the address and does NOTHING with it until somebody opens
// /proc/<pid>/heap — then procfs walks the TASK's page tables and reads the
// struct through the HHDM, the same technique that reads a task's argv. It is
// the only place in os64 where a ring-3 subsystem contributes the CONTENT of
// a kernel-rendered file, and it exists because a heap's shape is known only
// to the allocator that owns it (see abi/os64/heap.h for the three routes
// considered and why this one won).
//
// One call, once, from libos64's own init — no application ever calls this.
#define SYSCALL_HEAP_REPORT 42

// rename(oldpath, newpath) — give a file a different name, possibly in a
// different directory. Returns 0, or negative on refusal.
//
// THE GUARANTEE, and the only reason this call exists as a call: if
// `newpath` already names a regular file, it is REPLACED, and there is no
// instant at which `newpath` fails to resolve. That is the whole point.
// Unix went its first decade without rename(2) — you wrote link(new, old)
// then unlink(old), two steps, and a crash between them left you with two
// names or none. 4.2BSD (1983) added rename precisely to close that window,
// and every safe "publish a new version of this file" recipe since is built
// on it: write to a temporary name, verify what you wrote, then put it in
// place in one motion. os64get is this call's first customer, and the
// reason it was built: a transfer that fails must leave the previous file
// exactly where it was, not a truncated impostor wearing its name.
//
// os64 keeps the atomicity and declines the rest of POSIX's rename, which
// accumulated a great deal (EXDEV, directory-onto-empty-directory
// replacement, the ".."-and-link-count corner cases). The rules here are
// four, and each one refuses rather than surprising you:
//
//   - CROSS-FILESYSTEM renames are refused. Moving bytes between two
//     filesystems is a COPY, and a copy that fails halfway needs cleaning
//     up — which is userland's job, where the policy (retry? keep the
//     partial? prompt?) can actually be decided. Unix draws this same line
//     and calls it EXDEV; ours is the same line for the same reason.
//   - Replacement is FILE-ONTO-FILE only. A directory is never replaced,
//     and a directory is never renamed onto an existing name. A silent
//     rmdir hiding inside a rename is a surprise with no upside.
//   - An open DIRECTORY on either side is refused (its reader is mid-walk
//     through the very blocks a move re-parents). Open FILES are fine in
//     both roles, which is the whole point of the orphan work of
//     2026-08-16: a reader holds an INODE, not a name, so it neither
//     notices nor cares that its file was renamed — and when its file is
//     the one being REPLACED, that displaced inode survives, nameless,
//     until the last handle closes. This is what lets a refresh put a new
//     /bin/husk in place while husk is running.
//   - A directory may not be renamed into its own descendant. `mv a a/b/c`
//     detaches the subtree into a ring nothing points at — one of the few
//     things a rename can do that fsck cannot quietly repair.
//
// ATOMICITY IS THE FILESYSTEM'S TO GRANT. ext2 keeps the promise honestly
// (one directory-block write swings the name onto the new inode). FAT
// cannot — it has no file identity separate from the directory entry, which
// is exactly the idea the inode was and MS-DOS's 1981 filesystem was not —
// so on FAT an existing destination is removed first and a real window
// exists. Booked in DEBTS; root is ext2 precisely so the guarantee lives
// where it can be kept.
#define SYSCALL_RENAME 43

// 44 and 45 belong to the pty pair (PTY_CREATE / PTY_SNAPSHOT, defined with
// the GUI block below) — they were claimed on the gui branch first and this
// file RESERVED them sight-unseen, which is why the 2026-08-20 merge was a
// text join and not an ABI renumbering. The reservation trick is worth
// keeping: a branch that claims numbers writes them down in BOTH worlds.

// tty_handle() -> a handle on the CALLER'S controlling terminal, whatever
// handle 0 has become. No arguments: the answer is a property of who is
// asking. Returns the new handle (>= 3), or negative if the table is full.
//
// THE PROBLEM IT SOLVES, which every Unix has had since pipes existed:
// `ps | less` gives the pager a pipe on handle 0, and a pager needs its
// DOCUMENT from that pipe and its KEYS from the terminal — two sources, one
// slot. Unix answers by opening the magic path /dev/tty. os64 answers with a
// verb, because a /dev name with no devfs behind it is a filesystem's promise
// made by something that isn't one; when a devfs exists, /dev/tty becomes a
// NAME for this call and not a replacement of it.
//
// WHY IT IS THREE LINES OF KERNEL: a console handle carries no object
// (handle.h). The read path resolves task_tty(caller) at every read, so the
// handle means "my terminal" and never "terminal number four" — which is why
// one tag serves the VT fleet and a pty slave identically, and why a pager
// inside gterm reaches the slave with no special case anywhere. Minting a
// second one is handle_alloc(); everything else was already true.
//
// WHAT YOU GET is a second reference to ONE shared input ring, not a copy of
// it: a byte read through this handle is gone from handle 0 if handle 0 is
// also the terminal. Reads are ordinary reads — blocking, short (terminal
// semantics), 0 at Ctrl+D, and eligible for read()'s timeout. Ctrl+C never
// arrives as a byte; it is a signal aimed at the terminal's foreground task.
// A background job (`ps | less &`) reads EOF here exactly as it does on
// handle 0 — the console's background rule is keyed on the TASK, not the
// slot, so backgrounding cannot be used to steal the shell's keystrokes.
// Close it with close() like any handle.
#define SYSCALL_TTY_HANDLE 46

// conf_resolve — WHERE IS THE CONFIG FILE CALLED <name>?
//
//   arg0 = const char *name   a FILE name ("logd.conf"), never a path
//   arg1 = char *out          buffer for the winning path
//   arg2 = size_t cap         its size
//   arg3 = size_t from        ladder position to start at; 0 = the ordinary
//                             "find it" call
//   arg4 = int any            non-zero: do NOT probe, just build the path this
//                             name would have at position `from`. The WRITER's
//                             question — a program saving its settings needs
//                             the path of a file that does not exist yet, and
//                             it needs position 0 (the user's directory,
//                             /home by default) rather than wherever it READ
//                             from, because /etc is the system's and every
//                             build rewrites it
//   returns the matching ladder index PLUS ONE (so success is always >= 1
//   and can never be read as SYSCALL_RESULT_INVALID), or SYSCALL_RESULT_* on
//   failure — including "no directory in the search path has it", which is
//   not an error worth its own code, because the caller's next move is the
//   same either way: use defaults.
//
// `from` EXISTS FOR ONE READER, and it is not gold-plating. Almost every
// config file is a SETTINGS file, where first-hit-wins is the entire point:
// your /home/logd.conf replaces the system's. `hosts` is not one — Chris
// ruled it MERGED on 2026-08-22, /home/hosts layering ON TOP of /etc/hosts so
// your machine names sit over the system's list rather than erasing it. It is
// a database, not a setting. Feed a call's return value back as the next
// call's `from` to walk to the following copy; that lets the resolver read
// every hosts file on the ladder without keeping the private ladder this
// syscall exists to abolish.
//
// THE WALK IS THE KERNEL'S, and that is the whole point of the call. Six
// programs each carried a private copy of the same "/home then /etc" ladder
// until 2026-08-23; the cure is one setting (/etc/os64.conf's `conf =`) that
// every reader obeys, and a ladder obeyed by everyone must be PARSED by
// exactly one thing or it is not one ladder. The kernel already has to walk
// it for its own readers (desktop.c), so ring 3 asks the same walker rather
// than growing a second one over a /sys file — which would have meant two
// parsers to keep in agreement AND a separate channel for reporting what
// each reader took.
//
// Reporting comes free this way: because the kernel resolved it, /sys/conf
// can say which file every reader actually got, which is the diagnostic
// Chris asked for by name ("for some time I'll want to be able to verify
// where each conf file is coming from"). Each resolve also prints one line
// at DEBUG_BOOT.
//
// This RESOLVES rather than OPENS: the caller then open()s the path with the
// handle machinery it already has, and libos64's os64_conf_read takes a path.
// Handing back an open handle would buy nothing and would put a second way
// of acquiring handles into the ABI.
#define SYSCALL_CONF_RESOLVE 47

// spawn() FLAGS — arg5. Zero is the everyday spawn, so every caller written
// before this existed keeps working unchanged.
//
// BACKGROUND marks a job the shell launched with `&` and will NOT wait on. The
// kernel needs to know because of the keyboard: a background job that reads
// handle 0 would silently compete with the shell for your keystrokes (os32 had
// exactly this hole — you simply never backgrounded anything that read stdin).
// A background job's console read returns EOF instead, so `cmd &` behaves like
// `cmd < /dev/null &`. Output is untouched: background jobs still write to the
// screen, which was always the useful half.
#define OS64_SPAWN_BACKGROUND  0x1

// SET_TTY seats the child on a pty slave (PTY.md, 2026-08-19): the child's
// controlling terminal becomes the slave of the master handle carried in the
// HIGH 32 BITS of this same flags word. Packed rather than given its own
// argument because spawn's six registers were already spoken for — and a
// handle is a small int, the flags word had 62 idle bits, and one register
// carrying "how to spawn" is honest about what both values are. The child is
// seated as the slave's SHELL (tty_seat_shell: controlling shell, foreground,
// lights on), so seat a shell — that is what sessions are. Low-bit flags and
// the handle never collide: bits 1..31 stay flags, 32..63 stay the handle.
#define OS64_SPAWN_SET_TTY     0x2
#define OS64_SPAWN_TTY_SHIFT   32

// The pty family (PTY.md — ratified 2026-08-19). pty_create(cols, rows)
// returns a MASTER handle; the slave is a kernel tty the master names at
// spawn (above) and tasks name as their controlling terminal. GRID mode:
// write(master) injects keystrokes (0x03 runs the slave's Ctrl+C intercept),
// pty_snapshot copies the interpreted screen out; read(master) is reserved
// for the STREAM mode whose customer (telnetd) waits on TCP listen().
#define SYSCALL_PTY_CREATE   44
#define SYSCALL_PTY_SNAPSHOT 45

// seek() whence values — where `offset` is measured FROM. Part of the ABI
// because both sides must agree on the numbers; they intentionally match the
// kernel VFS's internal SEEK_* so no translation layer is needed.
#define OS64_SEEK_SET  0   // from the start of the file
#define OS64_SEEK_CUR  1   // from the current position
#define OS64_SEEK_END  2   // from the end (offset 0 = the size; negative backs up)

// Well-known handles until a real per-task handle table exists. 0/1/2 are the
// stdin/stdout/stderr convention (a genuinely good one): READ(0) blocks on the
// console keyboard, WRITE(1)/WRITE(2) reach the console. TTYs later make these
// per-task redirectable without changing the numbers.
#define SYSCALL_HANDLE_CONSOLE_IN   0
#define SYSCALL_HANDLE_CONSOLE_OUT  1
#define SYSCALL_HANDLE_CONSOLE_ERR  2

// --- GUI syscalls (16-22, ALL LIVE since 2026-08-17) -----------------------
// The userland boundary of GRAPHICS.md, dispatch rows in syscall.c. The
// kernel's own GUI clients (guicomp, the console, the demos) keep calling
// gui_client.h functions directly — these rows exist for ring 3, where every
// argument is a register a task filled and every handle is checked against
// its OWNER (a task can never touch a window it did not create;
// GUI_ERR_NOT_OWNER). Until the surface pivot (migration step 3),
// GET_SURFACE reports geometry but a NULL pixel pointer to ring-3 callers —
// the canvas VA only becomes a task VA at the pivot, and handing out a
// kernel address in the meantime would be a truthless answer AND a layout
// leak. PUBLISH was born PRESENT; renamed at design review ("present"
// doubles as an adjective, and is swapchain jargon besides).
#define SYSCALL_GUI_WINDOW_CREATE       16
#define SYSCALL_GUI_WINDOW_DESTROY      17
#define SYSCALL_GUI_WINDOW_GET_SURFACE  18
#define SYSCALL_GUI_WINDOW_PUBLISH      19
#define SYSCALL_GUI_EVENT_POLL          20
#define SYSCALL_GUI_SCREEN_INFO         21
#define SYSCALL_GUI_EVENT_WAIT          22   // blocking poll — shipped LAST, as planned (step 5)

// The readback half of create (2026-08-23): where is my window, and what
// state is it in? Frame rect + live flags, in create's own units.
//
// It lives at 48 rather than inside the 16..22 GUI block because that block
// is FULL — the seven reserved numbers were all spent by the surface pivot.
// Consumer-driven, as ever: gclock wanted to remember where you dragged it
// and what you pinned, and discovered that nothing in the client ABI could
// tell an app anything the WINDOW SYSTEM knew about its own window.
#define SYSCALL_GUI_WINDOW_GET_STATE    48

// signal_handler — install a handler for a signal, and answer with the one it
// replaced.
//
//   arg0 = int signo             the signal NUMBER (os64/signal.h)
//   arg1 = os64_signal_fn hand   the handler, or 0 for the kernel's default
//   returns the PREVIOUS handler (possibly 0), or a negative
//   OS64_SIG_ERR_* — in which case nothing was changed
//
// The handler belongs to the TASK, not the calling thread: a signal aimed at
// a program is broadcast to every one of its threads, so a per-thread handler
// would fire once per thread for a single SIGTERM. Install once, covers all.
//
// SIGKILL is refused (OS64_SIG_ERR_UNCATCHABLE). It is the answer to a
// program that has stopped answering; a kernel that let a program decline to
// die would have no last resort.
//
// An installed handler RUNS. Delivery arrives by three paths (SIGNALS.md):
// at the exit of whatever syscall the thread was in (§5 — the common case,
// and why a blocking call answers OS64_INTERRUPTED afterwards), from the
// scheduler's visit to a thread spinning in ring 3 that makes no syscalls
// (§10), and from the page-fault handler for a caught SIGSEGV (§9). The
// handler returns into a kernel stub that calls sigreturn (below) and the
// program resumes where it was.
//
// (HISTORICAL: for a few hours on 2026-08-23 this call shipped before
// delivery did, and an installed handler meant only "do not apply the
// default action". That paragraph stood here as if current until Codex #29
// rd20.) A signal nothing can send is REFUSED here rather than accepted on
// that same bet — see the numbered-but-not-real note in os64/signal.h.
#define SYSCALL_SIGNAL_HANDLER          49

// sigreturn — resume the context a signal handler interrupted.
//
// A PROGRAM NEVER CALLS THIS DELIBERATELY. It exists because the kernel sets
// a handler's return address to a stub that calls it (TASK_SIGRETURN_VIRT, in
// the same page as the exit trampoline). A handler is an ordinary C function
// and ends with `ret`; this is what is waiting at the other end of that `ret`.
//
//   arg0 = the frame the kernel wrote on the user stack
//   does not return — it resumes the interrupted context
//
// The frame is VALIDATED, not trusted: it arrives from ring 3 and this call
// restores register state, which makes it the most attackable thing in the
// signal path. It is refused unless it carries the kernel's magic, names a
// handler that is actually running on this thread, and carries a canonical
// lower-half RIP and RSP (a noncanonical one would #GP in ring 0 at the
// sysretq/iretq — the CVE-2012-0217 family). Nothing privileged is ever
// taken from it: CS and SS come from GDT constants, and RFLAGS passes
// through a mask that keeps only the bits a user program owns.
//
// WHERE the frame sits is deliberately NOT checked (this note used to say
// "sits on the calling thread's own stack" — Codex #29 rd20 caught the claim
// against kernel/src/syscall.c, which explains the omission). A range check
// on the pointer would prove nothing: every word of the frame is
// user-writable wherever it is, so the CONTENTS are what is defended, and
// they are, above. A program that hands sigreturn a frame it forged
// elsewhere gets exactly the resume it asked for, in ring 3, with ring 3's
// privileges — its own problem, and nobody else's.
#define SYSCALL_SIGRETURN               50

#endif
