.code64
.intel_syntax noprefix
#include "utility_macros.inc"

.global handler_irq1_asm
# extern handler_irq1_c
.section .text
handler_irq1_asm:
    SAVE_ALL                # Save all general-purpose registers
#    call handler_ireq_c     # Call the C handler
    mov al, 0x20            # Send EOI to PIC
    out 0x20, al
    RESTORE_ALL                 # Restore all registers
    iretq                    # Return from interrupt