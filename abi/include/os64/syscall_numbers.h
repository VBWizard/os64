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

// net_dial(dest) — open a network conversation. arg0 = const os64_netdest_t*
// (os64/net.h): WHERE (ip, host order — ruling #2: the kernel owns the wire),
// WHICH DOOR (port), HOW (protocol). Returns a handle you read() and write()
// like any other, or negative. The Plan 9 dial STRING ("udp!10.0.2.2!53")
// never crosses this boundary — libos64's os64_dial() parses it into this
// struct (ruling #1: kernel speaks structs, the library speaks strings).
// v1 speaks UDP; TCP takes the same struct in Phase 4.
// (Historically 28 for a few uncommitted hours on the net branch — then the
// userland branch minted printat/time/setenv at 28-30 in parallel, and the
// merge ceded the numbers to the elder commits. Two branches, one registry:
// the merge is where the registry gets reconciled, and this comment is the
// scar that says so.)
#define SYSCALL_NET_DIAL   31

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
