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
// physical console, without touching the console cursor: the WIDGET PLANE.
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
// natively; the future ext2 write driver will be built to it.
//
// Returns 0 on success. Refused, not half-done: a filesystem with no write
// path (os64's ext2 is read-only by design), a path that isn't there, or a
// directory that still has contents — emptying it first is the caller's
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

// --- GUI syscalls: RESERVED, not yet in the dispatch table -----------------
// The GUI client API (kernel gui/gui_client.h) is kernel-direct today; when
// userland GUI apps arrive these numbers go live — see GRAPHICS.md "The
// userland boundary" for the full design (16-21 defined there, 22 =
// gui_event_wait reserved).
#define SYSCALL_GUI_WINDOW_CREATE       16
#define SYSCALL_GUI_WINDOW_DESTROY      17
#define SYSCALL_GUI_WINDOW_GET_SURFACE  18
#define SYSCALL_GUI_WINDOW_PRESENT      19
#define SYSCALL_GUI_EVENT_POLL          20
#define SYSCALL_GUI_SCREEN_INFO         21
#define SYSCALL_GUI_EVENT_WAIT          22

#endif
