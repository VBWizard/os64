.intel_syntax noprefix
.section .text
.align 8
.code64


# IPI_ACCT_SETTLE_VECTOR (0x82) — CPU-time settle-on-read. The C handler
# charges THIS core's in-flight span to its current thread using THIS
# core's TSC (the whole point: cross-core TSC math is forbidden, so the
# reader asks each core to settle its own books). Same save-all shape as
# vector123 below.
.globl vector130
vector130:
	cli
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	pushf
	call acct_settle_ISR
	popf
	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rbp
	pop rdi
	pop rsi
	pop rdx
	pop rcx
	pop rbx
	pop rax
	sti
	iretq

.globl vector123
vector123: # IPI_INVALIDATE_TLB_VECTOR
	cli
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	pushf
	call inv_tlb_ISR
	popf
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
	sti
	iretq
.globl vector124
vector124: # AP Disable vector (IPI_DISABLE_SCHEDULING_VECTOR)
	cli
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	pushf	
	call disableAPScheduling_ISR 
	popf
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
	sti
	iretq
.globl vector125 
vector125: # AP Enable vector (IPI_ENABLE_SCHEDULING_VECTOR)
	cli
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	pushf
	call enableAPScheduling_ISR
	popf
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
	sti
	iretq
.globl vector127 # AP Initialization vector (IPI_AP_INITIALIZATION_VECTOR)
vector127:
	cli
	push rax
	push rbx
	push rcx
	push rdx
	push rsi
	push rdi
	push rbp
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	pushf
	call ap_initialization_handler
	popf
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
	sti
	iretq
