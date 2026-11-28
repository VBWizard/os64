.code64
.intel_syntax noprefix
#include "utility_macros.inc"
#include "irq_accounting.inc"

.extern ax210_isr
.extern kMPApicBase
.global handler_ax210_msix_asm
.section .text
handler_ax210_msix_asm:
    cld
    push rax
    push rcx
    push rdx
    push rbx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    IRQ_ACCOUNT_ENTER 88

    sub rsp, 8
    call ax210_isr
    add rsp, 8

    mov rax, qword ptr [rip + kMPApicBase]
    mov dword ptr [rax + 0xB0], 0
    IRQ_ACCOUNT_EXIT

    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbx
    pop rdx
    pop rcx
    pop rax
    iretq

.section .note.GNU-stack,"",@progbits
