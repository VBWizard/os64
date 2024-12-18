.code64
.intel_syntax noprefix
.extern kTicksSinceStart, kSystemCurrentTime, kUptime, kTicksPerSecond

.global handler_irq0_asm
.section .text
handler_irq0_asm:
	cli
	push rax
	# Increment the number of ticks since the system was started
	inc qword ptr [kTicksSinceStart]
	# Grab the "current count" of ticks in the current second
	mov rax, [irq0_current_count]
	inc rax
	# Compare the current count of ticks to the number of ticks per second
	cmp rax, [kTicksPerSecond] 
	# If it is less, then don't need to update the system time, so skip it
	jl ticks_not_expired
	# Update the system time
	inc qword ptr [kSystemCurrentTime]
	inc qword ptr [kUptime]
	# Reset the current count
	xor rax, rax
ticks_not_expired:
	mov [irq0_current_count], rax
    mov al, 0x20            # Send EOI to PIC
    out 0x20, al
	pop rax
	# No need to STI as flags are popped on IRET
    iretq                    # Return from interrupt


.section .bss
	irq0_current_count:		.space 8
