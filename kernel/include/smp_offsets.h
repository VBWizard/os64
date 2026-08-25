#ifndef SMP_OFFSETS_H
#define SMP_OFFSETS_H

#define CLS_KERNEL_RSP0_OFFSET 0x50
// (CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET moved out 2026-08-24: its only
// consumer, task_exit_asm.S, already reads it from the auto-generated
// asm-offsets.h — and syscall.S now includes BOTH headers, so a hand copy
// here became a -Werror redefinition. One name, one home: generated.)
// 0x90 -> 0x70 on 2026-08-10, when the four dead cikc_* fields (32 bytes) were
// removed from core_local_storage_t. The static assert below is why that was a
// compile error and not a boot-time mystery: syscall.S reads the user RSP at
// this literal offset, and a silently stale constant would have had the syscall
// entry stub restoring garbage into RSP on the way back to ring 3.
#define CLS_SYSCALL_USER_RSP_OFFSET 0x70
// (CLS_SYSCALL_RETURN_FRAME_OFFSET stood here for one day — the syscall
// return frame pointer is the THREAD's now, reached via the auto-generated
// THREAD_SYSCALL_RETURN_FRAME_OFFSET in asm-offsets.h. See thread.h.)

#ifndef __ASSEMBLER__
#include <stddef.h>
#include "smp.h"
_Static_assert(CLS_KERNEL_RSP0_OFFSET == offsetof(core_local_storage_t, kernel_rsp0),
               "CLS_KERNEL_RSP0_OFFSET mismatch");
_Static_assert(CLS_SYSCALL_USER_RSP_OFFSET == offsetof(core_local_storage_t, syscall_user_rsp),
               "CLS_SYSCALL_USER_RSP_OFFSET mismatch");
#endif

#endif
