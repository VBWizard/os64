#ifndef SMP_OFFSETS_H
#define SMP_OFFSETS_H

#define CLS_KERNEL_RSP0_OFFSET 0x50
#define CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET 0x68
// 0x90 -> 0x70 on 2026-08-10, when the four dead cikc_* fields (32 bytes) were
// removed from core_local_storage_t. The static assert below is why that was a
// compile error and not a boot-time mystery: syscall.S reads the user RSP at
// this literal offset, and a silently stale constant would have had the syscall
// entry stub restoring garbage into RSP on the way back to ring 3.
#define CLS_SYSCALL_USER_RSP_OFFSET 0x70
// The syscall return frame's address on the kernel stack (2026-08-23, signal
// delivery). Same discipline as the line above: syscall.S writes it at this
// literal offset, and the assert below is what turns a stale constant into a
// compile error instead of a signal handler returning to the wrong address.
#define CLS_SYSCALL_RETURN_FRAME_OFFSET 0x78

#ifndef __ASSEMBLER__
#include <stddef.h>
#include "smp.h"
_Static_assert(CLS_KERNEL_RSP0_OFFSET == offsetof(core_local_storage_t, kernel_rsp0),
               "CLS_KERNEL_RSP0_OFFSET mismatch");
_Static_assert(CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET == offsetof(core_local_storage_t, kernel_interrupt_stack_top),
               "CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET mismatch");
_Static_assert(CLS_SYSCALL_USER_RSP_OFFSET == offsetof(core_local_storage_t, syscall_user_rsp),
               "CLS_SYSCALL_USER_RSP_OFFSET mismatch");
_Static_assert(CLS_SYSCALL_RETURN_FRAME_OFFSET == offsetof(core_local_storage_t, syscall_return_frame),
               "CLS_SYSCALL_RETURN_FRAME_OFFSET mismatch");
#endif

#endif
