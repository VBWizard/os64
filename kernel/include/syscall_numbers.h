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

// Well-known handles accepted by SYSCALL_WRITE until a real per-task handle
// table exists.  1/2 mirror the "stdout/stderr" convention because it is a
// genuinely good one — both currently reach the console.
#define SYSCALL_HANDLE_CONSOLE_OUT  1
#define SYSCALL_HANDLE_CONSOLE_ERR  2

#endif
