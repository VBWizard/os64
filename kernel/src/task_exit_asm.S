#include "asm-offsets.h"

.code64
.intel_syntax noprefix

.extern task_exit

// Called when a task's thread returns via 'ret'.  The x86-64 ABI leaves the
// return value in RAX.  We walk CLS -> currentThread -> ownerTask and store
// RAX directly into task->retVal before any C function call can clobber it,
// then fall through into the normal task_exit path.
.globl task_exit_with_retval
.type  task_exit_with_retval, @function
task_exit_with_retval:
    mov  rcx, rax                                   // rcx = ELF's return value
    mov  rax, gs:[0]                                // rax = CLS self-pointer
    mov  rax, [rax + CLS_CURRENTTHREAD_OFFSET]      // rax = currentThread
    mov  rax, [rax + THREAD_OWNERTASK_OFFSET]       // rax = ownerTask (task_t*)
    mov  [rax + TASK_RETVAL_OFFSET], rcx            // task->retVal = return value
    jmp  task_exit
