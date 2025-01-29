.code64
.intel_syntax noprefix
.extern kTicksSinceStart, kSystemCurrentTime, kUptime, kTicksPerSecond
.extern update_framebuffer_from_shadow
.extern frameBufferRequiresUpdate

.global handler_irq0_asm
.section .text
handler_irq0_asm:
	cli
    push rax

	mov al, 0x20            # Send EOI to PIC
    out 0x20, al

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

	# Get the count of ticks since the last FB update
# #	mov rax, [fb_update_count]
# #	inc rax
# #	cmp rax, 500 # NOTE: As of now IRQ0 fires 100 times per second, so a count of 20 = 200 ms?
# #	jl over_fb_update

	# If it is the desired frequency
#	push rax
	# Check to see if other code has updated the FB
#	mov rax, [frameBufferRequiresUpdate]
#	cmp rax, 0
#	pop rax
#	je over_fb_update
#	#call update_framebuffer_from_shadow
	xor rax,rax
over_fb_update:
    mov [fb_update_count], rax

	mov al, 0x20            # Send EOI to PIC
    out 0x20, al
	# No need to STI as flags are popped on IRET
    pop rax
	sti
	iretq                    # Return from interrupt


.section .bss
	irq0_current_count:		.space 8
	fb_update_count:		.space 8
