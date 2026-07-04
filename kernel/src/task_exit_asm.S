#include "asm-offsets.h"

.code64
.intel_syntax noprefix

.extern task_exit
.extern kKernelPML4

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

// void call_in_kernel_context(void (*func)(void*), void *arg)
//
// Run func(arg) in kernel context — on the core's kernel interrupt stack with
// kKernelPML4 loaded — then restore the caller's (task) CR3/RSP and return.
//
// This is a NAKED trampoline on purpose. The equivalent C ran the switch in a
// normal frame and then touched C locals whose rbp was still the (now-unmapped)
// task stack — UB masked only by the -O0 layout (see the CLAUDE.md
// "Context Switching" notes). Here nothing is stored to a stack local across a
// switch: task CR3/RSP and func/arg live in callee-saved registers (which func
// preserves per the SysV ABI), and the ONE piece of per-core state we need,
// kernel_interrupt_stack_top, is read from CLS via GS (valid under any CR3/RSP).
.globl call_in_kernel_context
.type  call_in_kernel_context, @function
call_in_kernel_context:
    // Preserve the callee-saved regs we borrow (the caller expects them intact).
    push r12
    push r13
    push r14
    push r15
    mov  r12, rdi                                   // r12 = func
    mov  r13, rsi                                   // r13 = arg
    mov  r14, cr3                                   // r14 = task CR3   (survives call)
    mov  r15, rsp                                   // r15 = task RSP   (survives call)

    // kernel_rsp = (cls->kernel_interrupt_stack_top - 16) & ~0xF  (16-byte aligned
    // so the `call` below lands the callee at rsp%16 == 8, per the SysV ABI).
    mov  rax, [gs:CLS_KERNEL_INTERRUPT_STACK_TOP_OFFSET]
    sub  rax, 16
    and  rax, -16

    // Switch to the kernel interrupt stack, then to kKernelPML4.
    mov  rsp, rax
    mov  rax, kKernelPML4                           // value load, per scheduler.S idiom
    mov  cr3, rax

    // func(arg): SysV first argument in RDI.
    mov  rdi, r13
    call r12

    // Restore task CR3 and task RSP, then unwind and return to the caller (whose
    // return address is on the task stack we just restored).
    mov  cr3, r14
    mov  rsp, r15
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    ret
