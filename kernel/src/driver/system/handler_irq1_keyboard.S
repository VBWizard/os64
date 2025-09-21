.code64
.intel_syntax noprefix
#include "utility_macros.inc"

.extern keyboard_handle_scancode
.global handler_irq1_asm
.section .text
handler_irq1_asm:
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

    in al, 0x60             # Read the keyboard data port (clears IRQ internally)
    movzx edi, al
    sub rsp, 8              # Align stack to 16 bytes before calling C code
    call keyboard_handle_scancode
    add rsp, 8

    mov al, 0x20            # Send EOI to PIC
    out 0x20, al

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
    iretq                    # Return from interrupt
