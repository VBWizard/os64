#include "smp_offsets.h"

.code64
.section .text
.intel_syntax noprefix

.extern _syscall

.globl syscall_Enter
.type syscall_Enter, @function
syscall_Enter:
    # Save user return state on the user stack so we can reclaim the registers
    push r11                         # user RFLAGS
    push rcx                         # user RIP
    lea r11, [rsp + 16]              # r11 now holds original user RSP

    # Switch to the current kernel stack (stored in the per-core TSS)
    mov rcx, qword ptr [gs:0]
    mov rsp, qword ptr [rcx + CLS_KERNEL_RSP0_OFFSET]

    # Preserve callee-saved registers used by the kernel dispatcher
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    # Reserve space for the saved user context (rflags, rip, rsp) and padding for alignment
    sub rsp, 32

    # Capture the user-space context for the eventual sysret
    mov r12, qword ptr [r11 - 8]     # user RFLAGS that were pushed earlier
    mov r13, qword ptr [r11 - 16]    # user RIP
    mov [rsp], r12
    mov [rsp + 8], r13
    mov [rsp + 16], r11              # original user RSP

    # Dispatch the syscall; arguments remain in their ABI registers
    call _syscall

    # Prepare the return context
    mov rdx, [rsp]                   # user RFLAGS
    mov rcx, [rsp + 8]               # user RIP
    mov rsi, [rsp + 16]              # user RSP
    add rsp, 32

    # Restore callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    # Restore the user return state and drop back to ring3
    mov r11, rdx
    mov rsp, rsi
    sysretq

.size syscall_Enter, .-syscall_Enter
