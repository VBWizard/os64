.code64
.intel_syntax noprefix
#include "utility_macros.inc"

.global handler_irq1_asm
# extern handler_irq1_c
.section .text
handler_irq1_asm:
	push rax
    in al, 0x60             # <--- READ the keyboard data port (clears IRQ internally)
    mov al, 0x20            # Send EOI to PIC
    out 0x20, al
	pop rax
    iretq                    # Return from interrupt