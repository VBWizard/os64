.code64
.intel_syntax noprefix
#include "utility_macros.inc"

.global handler_irq0_asm
.section .text
handler_irq0_asm:
	cli
	IRQ_PROLOGUE
	# call handler_irq0_c     # Call the C handler
    mov al, 0x20            # Send EOI to PIC
    out 0x20, al
	IRQ_EPILOGUE
	sti
    iretq                    # Return from interrupt