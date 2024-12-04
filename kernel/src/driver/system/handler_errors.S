.code64
.intel_syntax noprefix
#include "utility_macros.inc"
.section .text
.global divide_by_zero_handler
divide_by_zero_handler:
    # call handler_divide_by_zero     # Call the C handler
	call beep_speaker
loop_of_death_zero:
	jmp loop_of_death_zero
    SAVE_ALL                        # Save all general-purpose registers
    RESTORE_ALL                     # Restore all registers
    iretq                    # Return from interrupt

.global general_protection_fault_handler
.section .text
general_protection_fault_handler:
    # call handler_divide_by_zero     # Call the C handler
	call beep_speaker
	call beep_speaker
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
	call beep_speaker
	call beep_speaker
	call beep_speaker
	nop
	jmp loop_of_death_pf
    SAVE_ALL                        # Save all general-purpose registers
    # call handler_divide_by_zero     # Call the C handler
    RESTORE_ALL                     # Restore all registers
    iretq                    # Return from interrupt

.global beep_speaker
.section .text
beep_speaker:
    # Set up PIT Channel 2 for square wave generation (e.g., ~1 kHz)
    mov al, 0xb6       # Select Channel 2, low/high byte access, square wave
    out 0x43, al
    mov al, 0xa5       # Divisor low byte (e.g., 0x04A5 for ~1 kHz)
    out 0x42, al
    mov al, 0x04       # Divisor high byte
    out 0x42, al

    # Enable the PC speaker
    in al, 0x61
    or al, 0x03        # Set bits 0 and 1 to enable speaker and PIT Channel 2
    out 0x61, al

    # Delay for ~some time (adjust based on system needs)
    mov rcx, 500000000
waitloop:
    dec rcx
    jnz waitloop

    # Disable the PC speaker
    in al, 0x61
    and al, 0xfc       # Clear bits 0 and 1 to disable speaker
    out 0x61, al

    ret