.code64
.intel_syntax noprefix
.section .text

.extern gLastFaultRbp
.extern gLastFaultRsp
.extern kTestingPageFaults

.global divide_by_zero_handler
divide_by_zero_handler:
    cli
    mov rdi, [rsp]  # Get faulting RIP from the stack
    call handle_divide_by_zero
    hlt

.global invalid_opcode_handler
invalid_opcode_handler:
    cli
    mov rdi, [rsp]  # Get RIP
    call handle_invalid_opcode
    hlt

# #DF arrives on IST1 (see idt.c) — a known-good per-core stack — so this
# handler runs even when the faulting code's RSP was the problem.
#
# The frame layout matters here and was wrong before: #DF PUSHES AN ERROR
# CODE (always zero), so [rsp] is that zero and RIP is at [rsp+8]. Reading
# [rsp] reported a faulting instruction of 0x0 every time. The rest of the
# frame is worth passing too — [rsp+32] is the RSP that was too broken to
# push onto, which is the single most useful number in the whole event.
.global double_fault_handler
double_fault_handler:
    cli
    mov rdi, [rsp+8]     # RIP that was executing
    mov rsi, [rsp+32]    # RSP at the time — the likely culprit
    mov rdx, [rsp+24]    # RFLAGS — garbage here means a corrupt frame
    call handle_double_fault_frame
    hlt

.global general_protection_fault_handler
general_protection_fault_handler:
    cli
    pop rdi   # Get error code
    pop rsi   # Get RIP (next on stack)
    call handle_general_protection_fault
    hlt

.global page_fault_handler
page_fault_handler:
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

    lea rax, [rsp + 128]
    mov qword ptr [rip + gLastFaultRsp], rax
    mov rax, [rsp + 72]
    mov qword ptr [rip + gLastFaultRbp], rax

    mov rsi, [rsp + 128]     # Error code
    mov rdx, [rsp + 136]     # Faulting RIP
    mov rax, cr2
    mov rdi, rax

    call handle_page_fault

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

.page_fault_resume:
    add rsp, 8          # Discard error code before iretq
    iretq

.global machine_check_handler
machine_check_handler:
    cli
    mov rdi, [rsp]  # Get RIP
    call handle_machine_check
    hlt
