.code64
.intel_syntax noprefix
#include "utility_macros.inc"

# IRQ1 (keyboard) entry. The actual 8042 read/dispatch lives in C
# (ps2_handle_irq in keyboard.c) and is SHARED with the IRQ12 mouse handler:
# it drains the controller and routes each byte by origin, so it doesn't
# matter which of the two IRQs fired for a given byte.

.extern ps2_handle_irq
.extern kIRQ1UsesLapic
.extern kMPApicBase
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

    sub rsp, 8              # Align stack to 16 bytes before calling C code
    call ps2_handle_irq
    add rsp, 8

    # EOI on whichever controller is delivering IRQ1: once
    # ioapic_adopt_isa_irq(1, ...) has run, that's the LAPIC; the PIC path
    # remains as the fallback for a system with no IOAPIC.
    cmp byte ptr [rip + kIRQ1UsesLapic], 0
    je irq1_send_pic_eoi
    mov rax, qword ptr [rip + kMPApicBase]
    mov dword ptr [rax + 0xB0], 0
    jmp irq1_eoi_done

irq1_send_pic_eoi:
    mov al, 0x20            # Send EOI to master PIC
    out 0x20, al

irq1_eoi_done:
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
