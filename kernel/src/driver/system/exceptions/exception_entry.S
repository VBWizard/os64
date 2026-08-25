.code64
.intel_syntax noprefix
.section .text

# ── The one prologue every exception shares ─────────────────────────────────
#
# Design and the reasons behind it live in exception_report.h. The short
# version: before this file, each vector had grown its own idea of what to save.
# #PF pushed a register block and left pointers to it in machine-wide globals;
# every other vector saved nothing and printed those globals anyway. This
# replaces all of it with one path, so a vector CANNOT report differently from
# its neighbours — there is only one piece of code that reports.
#
# THE FRAME, low address first, exactly matching exception_context_t:
#
#     r15 r14 r13 r12 r11 r10 r9 r8        <- rsp after the common pushes
#     rbp rdi rsi rdx rcx rbx rax
#     vector                                <- pushed by the per-vector stub
#     error_code                            <- CPU's, or our dummy zero
#     rip cs rflags rsp ss                  <- pushed by the CPU
#
# NORMALIZED ERROR CODE. The CPU pushes one for only ten of the thirty-two
# vectors. The stubs for the other twenty-two push a dummy zero, so every
# context has the same shape and the C side never has to ask "does this vector
# have an error code?" — the question that shifts every field by eight bytes
# when you get it wrong, and which this file exists to stop asking.
#
# THIS PATH CAN RETURN, and that is load-bearing: most page faults are the
# demand pager doing its job. exception_dispatch returns for a resolved fault
# and we iretq back into the faulting instruction; for anything fatal it reports
# and halts, so the tail below is simply never reached.

.extern exception_dispatch

# The per-vector stubs. Two shapes, differing only in who supplies the error
# code — which is the entire reason the old file needed two macros too.
.macro EXC_STUB_NOERR vec
.global exc_vector_\vec
exc_vector_\vec:
    push 0                  # dummy error code — normalize the frame
    push \vec
    jmp exc_common
.endm

.macro EXC_STUB_ERR vec
.global exc_vector_\vec
exc_vector_\vec:
    push \vec               # the CPU already pushed the error code
    jmp exc_common
.endm

exc_common:
    # DF CLEAR BEFORE ANY C RUNS (2026-08-24). An interrupt or exception gate
    # clears TF, NT, RF, VM — and IF for an interrupt gate — but NOT the
    # direction flag, so DF arrives here exactly as the interrupted code left
    # it. Ring 3 is allowed to set it (`std` is an ordinary unprivileged
    # instruction), and the SysV ABI lets the compiler emit `rep movsq` with
    # no `cld` in front because DF-clear is a precondition the caller owes.
    # The kernel carries twelve such copies even at -O0. One instruction here
    # covers all 32 vectors, since every stub jumps through this label.
    # (The SYSCALL path is covered by SFMASK instead — smp_core.c — because
    # an MSR can do it with no instruction at all. No MSR covers these gates.)
    cld

    # rax first so it lands at the HIGHEST address — push walks down, and the
    # struct is written low-address-first.
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

    # rsp now IS the exception_context_t. Hand it over.
    #
    # SysV wants rsp 16-byte aligned at the call, and we cannot assume it: a
    # ring0->ring0 delivery does not realign, so the residue depends on where
    # the interrupted code's stack happened to be. Force it, and keep the way
    # back in rbx — whose real value is already safe in the frame below, which
    # is precisely why rbx is the right scratch here.
    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    call exception_dispatch
    mov rsp, rbx

    # Resolved. Unwind in exact reverse and resume — possibly at a DIFFERENT
    # rip, because exception_dispatch is allowed to edit the context (the
    # page-fault self-test steers the iretq that way).
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

    add rsp, 16             # discard vector + error_code
    iretq

# ── The thirty-two vectors ──────────────────────────────────────────────────
#
# ALL of them, including the ones nobody has ever seen. An unpopulated IDT gate
# does not fail quietly: the CPU's answer to "cannot deliver that vector" is a
# #GP whose error code names the IDT slot, so the report blames whatever code
# was running rather than naming the real fault. That cost an afternoon once
# already (see the 2026-08-01 note in handler_errors.S), and the fix then only
# covered the vectors that existed in 2001. #VE, #CP, #HV, #VC and #SX are all
# deliverable on hardware this OS is expected to boot on.
#
# The ten that carry a CPU-pushed error code: 8, 10, 11, 12, 13, 14, 17, 21,
# 29, 30. Everything else gets the dummy.
EXC_STUB_NOERR 0        # #DE — divide error
EXC_STUB_NOERR 1        # #DB — debug
EXC_STUB_NOERR 2        # NMI — see below; idt.c does NOT wire this one
EXC_STUB_NOERR 3        # #BP — breakpoint (int3)
EXC_STUB_NOERR 4        # #OF — overflow
EXC_STUB_NOERR 5        # #BR — bound range exceeded
EXC_STUB_NOERR 6        # #UD — invalid opcode
EXC_STUB_NOERR 7        # #NM — device not available
EXC_STUB_ERR   8        # #DF — double fault (error code always zero)
EXC_STUB_NOERR 9        # coprocessor segment overrun (386-era, never delivered)
EXC_STUB_ERR   10       # #TS — invalid TSS
EXC_STUB_ERR   11       # #NP — segment not present
EXC_STUB_ERR   12       # #SS — stack-segment fault
EXC_STUB_ERR   13       # #GP — general protection
EXC_STUB_ERR   14       # #PF — page fault
EXC_STUB_NOERR 15       # reserved
EXC_STUB_NOERR 16       # #MF — x87 floating point
EXC_STUB_ERR   17       # #AC — alignment check
EXC_STUB_NOERR 18       # #MC — machine check
EXC_STUB_NOERR 19       # #XM — SIMD floating point
EXC_STUB_NOERR 20       # #VE — virtualization exception
EXC_STUB_ERR   21       # #CP — control protection
EXC_STUB_NOERR 22
EXC_STUB_NOERR 23
EXC_STUB_NOERR 24
EXC_STUB_NOERR 25
EXC_STUB_NOERR 26
EXC_STUB_NOERR 27
EXC_STUB_NOERR 28       # #HV — hypervisor injection
EXC_STUB_ERR   29       # #VC — VMM communication
EXC_STUB_ERR   30       # #SX — security exception
EXC_STUB_NOERR 31       # reserved

# A table so idt.c can wire these in a loop rather than thirty-two near-identical
# lines, each of which is a chance to typo a vector number.
#
# Slot 2 is present for completeness but idt.c deliberately skips it: NMI is an
# INSTRUMENT here, not a fault (nmi_probe.c fires them at wedged cores and needs
# them to RESUME), so it keeps its own stub and its own IST.
.section .data
.align 8
.global kExceptionStubs
kExceptionStubs:
    .quad exc_vector_0,  exc_vector_1,  exc_vector_2,  exc_vector_3
    .quad exc_vector_4,  exc_vector_5,  exc_vector_6,  exc_vector_7
    .quad exc_vector_8,  exc_vector_9,  exc_vector_10, exc_vector_11
    .quad exc_vector_12, exc_vector_13, exc_vector_14, exc_vector_15
    .quad exc_vector_16, exc_vector_17, exc_vector_18, exc_vector_19
    .quad exc_vector_20, exc_vector_21, exc_vector_22, exc_vector_23
    .quad exc_vector_24, exc_vector_25, exc_vector_26, exc_vector_27
    .quad exc_vector_28, exc_vector_29, exc_vector_30, exc_vector_31
