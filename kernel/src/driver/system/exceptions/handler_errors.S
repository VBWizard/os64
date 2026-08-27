.code64
.intel_syntax noprefix
.section .text

.extern gLastFaultRbp
.extern gLastFaultRsp
.extern gLastFaultRegs
.extern kTestingPageFaults

# ── The stale-capture problem, and the macro that ends it ───────────────────
#
# gLastFaultRegs / gLastFaultRbp / gLastFaultRsp are written by ONE stub in this
# file: page_fault_handler. Every other exception funnels into exception_panic,
# which prints them as "the register set the fault interrupted" — so a #GP
# reported whatever the LAST PAGE FAULT left behind, under a label saying it was
# current. Demand paging means a page fault has always just happened, so the
# `gLastFaultRegs == 0` guard in dump_fault_registers never once fired.
#
# That is the exact failure the CR2 comment in exception_panic warns about — a
# confident-looking field that means nothing — living two functions away from
# the warning. Chris found the missing half of it on a live #PF: the one handler
# that DOES capture registers had a report that never printed them.
#
# Until every stub captures for real (the follow-on: normalize the frame with a
# dummy error code and give them all one shared prologue), the honest thing is
# to say nothing rather than to lie. This clears the three globals so the
# existing guards report "not captured for this exception" — which is true.
#
# rax is free: every user of this macro is about to hand its own values to C.
.macro CLEAR_STALE_FAULT_CAPTURE
    cld                     # DF clear before any C runs (rd27's IDT audit). These are the
                            # EXCOLD fallback stubs — opt-in, diagnostic, never resume the
                            # faulter — but each calls C, and every one of them runs this
                            # macro first, so this is the one place that covers all eight.
    xor rax, rax
    mov qword ptr [rip + gLastFaultRegs], rax
    mov qword ptr [rip + gLastFaultRbp], rax
    mov qword ptr [rip + gLastFaultRsp], rax
.endm

.global divide_by_zero_handler
divide_by_zero_handler:
    cli
    CLEAR_STALE_FAULT_CAPTURE
    mov rdi, [rsp]  # Get faulting RIP from the stack
    call handle_divide_by_zero
    hlt

.global invalid_opcode_handler
invalid_opcode_handler:
    cli
    CLEAR_STALE_FAULT_CAPTURE
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
    CLEAR_STALE_FAULT_CAPTURE
    mov rdi, [rsp+8]     # RIP that was executing
    mov rsi, [rsp+32]    # RSP at the time — the likely culprit
    mov rdx, [rsp+24]    # RFLAGS — garbage here means a corrupt frame
    call handle_double_fault_frame
    hlt

.global general_protection_fault_handler
general_protection_fault_handler:
    cli
    CLEAR_STALE_FAULT_CAPTURE
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

    # The 16 qwords just pushed ARE the interrupted register set — the handler
    # has always had them and simply never told C where they were, so every
    # fault report so far has been RIP and CR2 and guesswork. RSP points at the
    # pushf result, with r15..rax ascending above it (see fault_regs_t).
    # Captured AFTER the two above so this stays a pure addition to a sequence
    # whose offsets are already load-bearing.
    mov qword ptr [rip + gLastFaultRegs], rsp

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
    CLEAR_STALE_FAULT_CAPTURE
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
    CLEAR_STALE_FAULT_CAPTURE
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
    CLEAR_STALE_FAULT_CAPTURE
    mov rdi, \vec
    pop rsi                 # error code the CPU pushed
    mov rdx, [rsp]          # ...RIP is next
    call handle_unexpected_exception
    hlt
.endm

EXC_NOERR 1,  debug_exception_handler          # #DB — single-step / breakpoints
EXC_NOERR 3,  breakpoint_handler               # #BP — int3
EXC_NOERR 4,  overflow_handler                 # #OF
EXC_NOERR 5,  bound_range_handler              # #BR
EXC_NOERR 7,  device_not_available_handler     # #NM — cannot happen: CR0.EM and CR0.TS are never set (fpu.c)
EXC_ERR   10, invalid_tss_handler              # #TS
EXC_ERR   11, segment_not_present_handler      # #NP
EXC_ERR   12, stack_segment_handler            # #SS
EXC_NOERR 16, x87_fpu_handler                  # #MF — ring 3 kills only on the unified path; EXCOLD panics like every other vector here
EXC_ERR   17, alignment_check_handler          # #AC
EXC_NOERR 19, simd_fpu_handler                 # #XM — same

# ── NMI (vector 2): the one exception that is also an INSTRUMENT ────────────
#
# Every other vector above names a fault and stops. NMI is different: it is the
# only way to reach a core that has stopped taking maskable interrupts, so this
# gate does double duty. If the core was asked a question (nmi_probe_capture
# returns true) it answers and RESUMES, untouched. If this is a genuine
# hardware NMI — parity error, watchdog — it falls through to exactly the panic
# path it has always had. The request word in nmi_probe.c is what tells the two
# apart, and getting that backwards would mean silently swallowing the one
# class of NMI that means the machine is on fire.
#
# This gate runs on IST2 (idt.c) — its own per-core stack — because a core
# worth probing may be wedged BY its stack, and an instrument must not depend
# on the health of the thing it is measuring.
#
# NOTE: no `cli` here, unlike every handler above. The interrupt gate already
# cleared IF on delivery, and the hardware blocks further NMIs until our
# `iretq`. Adding one would be noise pretending to be caution.

# The saved-register block, pushed low-address-last so it lands in memory as
# nmi_regs_t reads it: rflags lowest, then r15..r8, rbp, rdi, rsi, rdx, rcx,
# rbx, rax ascending. Deliberately the same layout the #PF stub established for
# fault_regs_t — one convention, not two.
#
# CHANGE EITHER MACRO AND YOU MUST CHANGE nmi_regs_t IN nmi_probe.h.
.macro NMI_PUSH_REGS
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
.endm

.macro NMI_POP_REGS
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
.endm

.global nmi_handler
nmi_handler:
    NMI_PUSH_REGS
    cld                     # DF clear before any C runs (Codex #29 rd27). An NMI
                            # interrupts ANYTHING, ring 3 mid-`std` included, and
                            # nmi_probe_capture is C with compiler-emitted string
                            # ops. The user's flags are safe: NMI_PUSH_REGS pushf'd
                            # them and NMI_POP_REGS popf's them back.

    mov rdi, rsp            # nmi_regs_t*  — the 16 qwords just pushed
    lea rsi, [rsp + 128]    # nmi_frame_t* — RIP, CS, RFLAGS, RSP, SS

    # SysV wants RSP 16-byte aligned at the call. Rather than reason about the
    # residue the CPU's own pre-push alignment leaves us with, force it — and
    # keep the way back in RBX, whose real value is already safe on the stack
    # two dozen bytes below.
    mov rbx, rsp
    and rsp, -16
    call nmi_probe_capture
    mov rsp, rbx

    test al, al
    jz .nmi_real_hardware_fault

    # Ours. Put the core back exactly as we found it and leave.
    NMI_POP_REGS
    iretq

.nmi_real_hardware_fault:
    # Not ours. Unwind our block so the frame is where the shared reporter
    # expects it, then take the road every unclaimed vector takes.
    NMI_POP_REGS
    cli
    mov rdi, 2              # which exception
    xor rsi, rsi            # NMI pushes no error code
    mov rdx, [rsp]          # faulting RIP (top of frame)
    call handle_unexpected_exception
    hlt
