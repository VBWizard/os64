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

# ── The exceptions nobody had claimed (added 2026-08-01) ───────────────────
#
# The IDT registered only #DE/#UD/#DF/#GP/#PF/#MC. Every OTHER CPU exception
# hit an unpopulated gate, and the CPU's answer to "cannot deliver that
# vector" is a #GP whose error code names the IDT slot it failed on — so a
# debug exception surfaced as a General Protection Fault blaming whatever
# code happened to be executing (it named task_idle_loop, which had done
# nothing wrong). Naming a fault correctly is the difference between a
# five-minute diagnosis and an afternoon.
#
# Two shapes, because the CPU pushes an error code for some vectors and not
# others; getting that wrong shifts every value on the stack by eight bytes.
.macro EXC_NOERR vec, name
.global \name
\name:
    cli
    mov rdi, \vec           # which exception
    xor rsi, rsi            # no error code for this one
    mov rdx, [rsp]          # faulting RIP (top of frame)
    call handle_unexpected_exception
    hlt
.endm

.macro EXC_ERR vec, name
.global \name
\name:
    cli
    mov rdi, \vec
    pop rsi                 # error code the CPU pushed
    mov rdx, [rsp]          # ...RIP is next
    call handle_unexpected_exception
    hlt
.endm

EXC_NOERR 1,  debug_exception_handler          # #DB — single-step / breakpoints
EXC_NOERR 2,  nmi_handler                      # NMI
EXC_NOERR 3,  breakpoint_handler               # #BP — int3
EXC_NOERR 4,  overflow_handler                 # #OF
EXC_NOERR 5,  bound_range_handler              # #BR
EXC_NOERR 7,  device_not_available_handler     # #NM — FPU/SSE without CR0 setup
EXC_ERR   10, invalid_tss_handler              # #TS
EXC_ERR   11, segment_not_present_handler      # #NP
EXC_ERR   12, stack_segment_handler            # #SS
EXC_NOERR 16, x87_fpu_handler                  # #MF
EXC_ERR   17, alignment_check_handler          # #AC
EXC_NOERR 19, simd_fpu_handler                 # #XM
