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
