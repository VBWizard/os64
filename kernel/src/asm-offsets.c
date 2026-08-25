/*
 * asm-offsets.c — emit struct field offsets for use in .S files.
 *
 * The Makefile compiles this file with -S and extracts lines matching
 * "=> NAME VALUE" to auto-generate include/asm-offsets.h.  Never link
 * this file into the kernel; it exists solely to produce that header.
 *
 * To add a new offset: add a DEFINE() call here and use the constant
 * by name in your .S file.  The value is always up to date.
 */
#include <stddef.h>
#include "smp.h"
#include "thread.h"
#include "task.h"

#define DEFINE(name, val) \
    __asm__ volatile ("\n=> " #name " %0" : : "i" ((unsigned long)(val)))

void asm_offsets(void)
{
    /* core_local_storage_t (smp.h) — used in task_exit_asm.S, scheduler.S */
    DEFINE(CLS_APIC_ID_OFFSET,       offsetof(core_local_storage_t, apic_id));
    DEFINE(CLS_CURRENTTHREAD_OFFSET, offsetof(core_local_storage_t, currentThread));
    /* used by call_in_kernel_context (task_exit_asm.S) */
    DEFINE(CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET, offsetof(core_local_storage_t, kernel_interrupt_stack_top));

    /* thread_t (thread.h) — used in task_exit_asm.S */
    DEFINE(THREAD_OWNERTASK_OFFSET, offsetof(thread_t, ownerTask));
    /* used by syscall.S: the in-flight syscall's return frame, published for
     * signal delivery. On the THREAD because the frame is on the thread's
     * kernel stack and must park and migrate with it — see thread.h. */
    DEFINE(THREAD_SYSCALL_RETURN_FRAME_OFFSET, offsetof(thread_t, syscall_return_frame));

    /* task_t (task.h) — used in task_exit_asm.S */
    DEFINE(TASK_RETVAL_OFFSET, offsetof(task_t, retVal));
}
