#ifndef SMP_OFFSETS_H
#define SMP_OFFSETS_H

#define CLS_KERNEL_RSP0_OFFSET 0x50
#define CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET 0x68
#define CLS_SYSCALL_USER_RSP_OFFSET 0x90

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
