#ifndef SYSCALL_NUMBERS_H
#define SYSCALL_NUMBERS_H

// os64 syscall numbers — shared between C (syscall table) and assembly (the
// ring-3 exit trampoline template in task_exit_asm.S), so this header must
// stay preprocessor-only: bare #defines, no typedefs, no prototypes.
//
// os64 syscall convention (ours, not Linux's — numbers and semantics are free
// to diverge; see syscall.S for the register contract):
//   RAX = syscall number, args in RDI, RSI, RDX, R10, R8, R9, result in RAX.

#define SYSCALL_YIELD      0
#define SYSCALL_DEBUG_LOG  1
#define SYSCALL_EXIT       2
#define SYSCALL_WRITE      3
#define SYSCALL_READ       4

// Well-known handles until a real per-task handle table exists. 0/1/2 are the
// stdin/stdout/stderr convention (a genuinely good one): READ(0) blocks on the
// console keyboard, WRITE(1)/WRITE(2) reach the console. TTYs later make these
// per-task redirectable without changing the numbers.
#define SYSCALL_HANDLE_CONSOLE_IN   0
#define SYSCALL_HANDLE_CONSOLE_OUT  1
#define SYSCALL_HANDLE_CONSOLE_ERR  2

// --- GUI syscalls: RESERVED, not yet in the dispatch table -----------------
// The GUI client API (gui/gui_client.h) is kernel-direct today; each function
// is already syscall-shaped and annotated in gui/gui_client.c with the
// user_ptr_mask it needs. When userland GUI apps arrive, add SYSCALL_DEFINE
// rows for these numbers — see GRAPHICS.md "Userland migration" for the
// recipe (gui_window_get_surface is the one call whose implementation must
// change: kernel pixel pointer -> shared-memory mapping).
#define SYSCALL_GUI_WINDOW_CREATE       16
#define SYSCALL_GUI_WINDOW_DESTROY      17
#define SYSCALL_GUI_WINDOW_GET_SURFACE  18
#define SYSCALL_GUI_WINDOW_PRESENT      19
#define SYSCALL_GUI_EVENT_POLL          20
#define SYSCALL_GUI_SCREEN_INFO         21

#endif
