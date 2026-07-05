#include "smp_offsets.h"

.code64
.section .text
.intel_syntax noprefix

.extern _syscall_dispatch

# ── os64 SYSCALL entry ────────────────────────────────────────────────────────
#
# Hardware state on arrival (SYSCALL from ring 3):
#   RCX = user RIP (the instruction after `syscall`)     — set by the CPU
#   R11 = user RFLAGS                                    — set by the CPU
#   RSP = STILL THE USER STACK (SYSCALL switches CS/SS but NOT the stack)
#   CR3 = still the user address space (kernel is mapped in every user PML4)
#   IF  = 0 (masked by SFMASK), so nothing can preempt us until we choose
#
# os64 syscall register convention (see syscall_numbers.h for the numbers):
#   RAX = syscall number
#   RDI, RSI, RDX, R10, R8, R9 = args 0-5   (R10 stands in for RCX, which the
#                                            CPU burned for the return RIP)
#   RAX = result
#   CLOBBERED for the caller: RCX, R11 (by the hardware), plus all C
#   caller-saved registers (RDI, RSI, RDX, R8-R10) — the same set a plain C
#   function call would clobber, so a C syscall stub needs no special saving.
#   Callee-saved registers (RBX, RBP, R12-R15) are preserved.
#
# Design rule: NEVER touch the user stack here.  A ring-3 program that points
# RSP at garbage and executes `syscall` must not be able to fault the kernel
# (a #PF here, before we have a good stack, would double-fault).  So the user
# RSP is stashed GS-relative in core-local storage — safe because IF is masked,
# so nothing runs on this core between the store and the reload below.

.globl syscall_Enter
.type syscall_Enter, @function
syscall_Enter:
    # Stash the user RSP in CLS and switch to this thread's kernel stack.
    # kernel_rsp0 is kept in sync with the running thread by tss_set_rsp0()
    # on every context switch, and is 16-byte aligned (stack top - 48).
    mov gs:[CLS_SYSCALL_USER_RSP_OFFSET], rsp
    mov rsp, gs:[CLS_KERNEL_RSP0_OFFSET]

    # Preserve the callee-saved registers (they hold live user values; the C
    # dispatcher is free to use them and will restore its own uses, but WE use
    # r12 as scratch below, and the convention above promises to preserve them).
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    # Frame for the user return context: [0]=RFLAGS, [8]=RIP, [16]=user RSP,
    # [24..39]=padding.  40 (not 32) so that after the `push r9` in the arg
    # marshal below, the C dispatcher is entered with RSP%16==8 per the ABI.
    sub rsp, 40
    mov [rsp], r11                              # user RFLAGS
    mov [rsp + 8], rcx                          # user RIP
    mov r12, gs:[CLS_SYSCALL_USER_RSP_OFFSET]
    mov [rsp + 16], r12                         # user RSP

    # Marshal the syscall registers into the SysV C argument slots for
    #   uint64_t _syscall_dispatch(nr, a0, a1, a2, a3, a4, a5)
    # i.e. RDI,RSI,RDX,RCX,R8,R9 + one stack slot.  Order matters: each move
    # writes a register only after its original value has been consumed.
    push r9                                     # a5 -> 7th arg (stack)
    mov r9, r8                                  # a4
    mov r8, r10                                 # a3
    mov rcx, rdx                                # a2
    mov rdx, rsi                                # a1
    mov rsi, rdi                                # a0
    mov rdi, rax                                # syscall number
    call _syscall_dispatch
    add rsp, 8                                  # drop the stacked a5

    # Result stays in RAX all the way back to the user.
    # Rebuild the SYSRET context from the saved frame.  RSI is used to carry
    # the user RSP across the callee-saved pops (it's caller-saved — already
    # documented as clobbered).
    mov r11, [rsp]                              # user RFLAGS (SYSRET reloads from R11)
    mov rcx, [rsp + 8]                          # user RIP    (SYSRET reloads from RCX)
    mov rsi, [rsp + 16]                         # user RSP
    add rsp, 40

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    # Back to the user stack and drop to ring 3.  SYSRETQ loads CS/SS from
    # STAR[63:48] (see smp_core.c) and RFLAGS from R11 — the user's IF comes
    # back on here (user RFLAGS always has IF set).
    mov rsp, rsi
    sysretq

.size syscall_Enter, .-syscall_Enter
