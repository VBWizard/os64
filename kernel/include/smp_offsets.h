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

#ifndef __ASSEMBLER__
#include <stddef.h>
#include "smp.h"
_Static_assert(CLS_KERNEL_RSP0_OFFSET == offsetof(core_local_storage_t, kernel_rsp0),
               "CLS_KERNEL_RSP0_OFFSET mismatch");
_Static_assert(CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET == offsetof(core_local_storage_t, kernel_interrupt_stack_top),
               "CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET mismatch");
_Static_assert(CLS_SYSCALL_USER_RSP_OFFSET == offsetof(core_local_storage_t, syscall_user_rsp),
               "CLS_SYSCALL_USER_RSP_OFFSET mismatch");
#endif

#endif
