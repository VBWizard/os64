.code64
.intel_syntax noprefix
#include "irq_accounting.inc"
.extern kTicksSinceStart, kSystemCurrentTime, kUptime, kTicksPerSecond, kProcessSignals
.extern processSignals
.extern signalProcTickFrequency
.extern kMPApicBase
.extern kIRQ0UsesLapic
.global handler_irq0_asm
.section .text

handler_irq0_asm:
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
	IRQ_ACCOUNT_ENTER 88

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

	# Acknowledge interrupt on the appropriate controller
	cmp byte ptr [rip + kIRQ0UsesLapic], 0
	je send_pic_eoi
	mov rax, qword ptr [rip + kMPApicBase]
	mov dword ptr [rax + 0xB0], 0
	jmp irq0_eoi_done

send_pic_eoi:
	mov al, 0x20
    out 0x20, al

irq0_eoi_done:
	IRQ_ACCOUNT_EXIT
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

.section .bss
	.global irq0_current_count       # <--- This line is mandatory!
	irq0_current_count:		.space 8
