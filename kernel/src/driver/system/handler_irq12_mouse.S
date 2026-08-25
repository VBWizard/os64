.code64
.intel_syntax noprefix
#include "utility_macros.inc"

# IRQ12 (PS/2 mouse) entry. Identical shape to handler_irq1_keyboard.S — the
# shared C dispatch (ps2_handle_irq) drains the 8042 and routes each byte by
# origin, so keyboard and mouse bytes always reach the right driver no matter
# which of the two IRQs fired.

.extern ps2_handle_irq
.extern kIRQ12UsesLapic
.extern kMPApicBase
.global handler_irq12_asm
.section .text
handler_irq12_asm:
    cld                     # DF clear before any C runs — an interrupt gate does not clear it, and ring 3 may have set it (see exc_common)
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

    # EOI. Routed through the IOAPIC (the normal case) it's one LAPIC write.
    # The legacy-PIC fallback needs TWO: IRQ12 lives on the slave PIC, and a
    # slave interrupt must be acknowledged on BOTH the slave and the master
    # (the master saw it via the cascade line, IRQ2).
    cmp byte ptr [rip + kIRQ12UsesLapic], 0
    je irq12_send_pic_eoi
    mov rax, qword ptr [rip + kMPApicBase]
    mov dword ptr [rax + 0xB0], 0
    jmp irq12_eoi_done

irq12_send_pic_eoi:
    mov al, 0x20
    out 0xA0, al            # EOI to slave PIC
    out 0x20, al            # EOI to master PIC (cascade)

irq12_eoi_done:
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
