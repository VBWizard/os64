.code64
.intel_syntax noprefix
.extern kTicksSinceStart, kSystemCurrentTime, kUptime, kTicksPerSecond, kProcessSignals
.extern update_framebuffer_from_shadow
.extern frameBufferRequiresUpdate
.extern processSignals
.extern signalProcTickFrequency
.global handler_irq0_asm
.section .text

handler_irq0_asm:
    push rax

	# Increment the number of ticks since the system was started
	lock inc qword ptr [kTicksSinceStart]
	# Grab the "current count" of ticks in the current second
	mov rax, [irq0_current_count]
	inc rax
	# Compare the current count of ticks to the number of ticks per second
	cmp rax, [kTicksPerSecond] 
	# If it is less, then don't need to update the system time, so skip it
	jb ticks_not_expired

	# Update the system time
	lock inc qword ptr [kSystemCurrentTime]
	lock inc qword ptr [kUptime]
	# Reset the current count
	xor rax, rax
ticks_not_expired:
	mov [irq0_current_count], rax

	# Send EOI to PIC
	mov al, 0x20            
    out 0x20, al

	pop rax
	iretq                    # Return from interrupt

.section .bss
	.global irq0_current_count       # <--- This line is mandatory!
	irq0_current_count:		.space 8
