.code64
.intel_syntax noprefix
#include "utility_macros.inc"
.section .text
.global divide_by_zero_handler
divide_by_zero_handler:
    # call handler_divide_by_zero     # Call the C handler
loop_of_death_zero:
	jmp loop_of_death_zero
    SAVE_ALL                        # Save all general-purpose registers
    RESTORE_ALL                     # Restore all registers
    iretq                    # Return from interrupt

.global general_protection_fault_handler
.section .text
general_protection_fault_handler:
    # call handler_divide_by_zero     # Call the C handler
loop_of_death_gpf:
	nop
	jmp loop_of_death_gpf
    SAVE_ALL                        # Save all general-purpose registers
    RESTORE_ALL                     # Restore all registers
    iretq                    # Return from interrupt


.global page_fault_handler
.section .text
page_fault_handler:
loop_of_death_pf:
	nop
	jmp loop_of_death_pf
    SAVE_ALL                        # Save all general-purpose registers
    # call handler_divide_by_zero     # Call the C handler
    RESTORE_ALL                     # Restore all registers
    iretq                    # Return from interrupt
