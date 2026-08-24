#include "smp_offsets.h"
#include "asm-offsets.h"

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

    # Publish WHERE that frame is, so the C side can rewrite it. Signal
    # delivery works by changing the address this syscall returns to (and the
    # stack it returns on), and this frame is the only place those live — the
    # thread's `regs` are the scheduler's save area and the fast path never
    # touches them. Cleared on the way out, below, so a checkpoint reached
    # from anywhere but a syscall cannot find a stale frame and deliver
    # against somebody else's return address.
    #
    # ON THE THREAD, not in CLS (2026-08-24). The frame lives on this thread's
    # kernel stack, and a blocking syscall PARKS with it live — a per-core slot
    # would keep naming it while other threads run on this core, and a woken
    # sleeper delivering on some OTHER core would read that core's leftover
    # instead of its own. r12 is ours to reuse: its user value is already
    # saved above and its current job (carrying user RSP) is done.
    mov r12, gs:[CLS_CURRENTTHREAD_OFFSET]
    mov [r12 + THREAD_SYSCALL_RETURN_FRAME_OFFSET], rsp

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
    # The frame is no longer live once we start consuming it — see the store
    # above for why a stale pointer would be dangerous rather than merely
    # untidy. gs:[currentThread] is re-read rather than kept from entry: if
    # this syscall parked and resumed, the thread may be on a different CORE
    # now, but gs always describes the core we are executing on and its
    # currentThread is always us. (r10 is caller-saved and already documented
    # as clobbered.)
    mov r10, gs:[CLS_CURRENTTHREAD_OFFSET]
    mov qword ptr [r10 + THREAD_SYSCALL_RETURN_FRAME_OFFSET], 0

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

    # SCRUB THE SCRATCH REGISTERS FIRST (2026-08-24). Whatever the kernel
    # last happened to leave in a caller-saved register was, until this
    # block, handed to ring 3 verbatim on EVERY syscall return. That is not
    # hypothetical residue: R10 provably holds the current thread_t* — the
    # `mov r10, gs:[CLS_CURRENTTHREAD_OFFSET]` a dozen lines up — so every
    # syscall disclosed a live kernel heap pointer to the program that made
    # it. The rest (RDI, RDX, R8, R9) carry whatever _syscall_dispatch's
    # epilogue left behind, which is not a thing anyone reasons about.
    #
    # It is worth the five instructions on plain hygiene: a boundary should
    # hand across what it MEANT to hand across, and nothing else. Ring 3 is
    # already entitled to treat these as clobbered — they are caller-saved,
    # and libos64's syscall stubs say so in their clobber lists — so zero is
    # as valid an answer as garbage, and it is the answer that discloses
    # nothing. os64 has no KASLR and its kernel base is a published constant,
    # so this leaks less here than it would elsewhere; that is an argument
    # about severity, not about whether to do it.
    #
    # NOT scrubbed, because each is load-bearing at this instruction:
    #   RAX — the syscall's return value, the whole point
    #   RCX — user RIP, consumed by SYSRETQ
    #   R11 — user RFLAGS, consumed by SYSRETQ
    #   RSI — was the user RSP; already moved into RSP on the line above, so
    #         it is zeroed below like the others
    # RBX/RBP/R12-R15 were popped from the frame and hold the USER's own
    # callee-saved values — restoring those is the contract, not a leak.
    #
    # WHEN THE RAX:RDX RETURN PAIR LANDS (ABI.md's designed value:status
    # convention, still interim sentinels today), RDX MOVES OUT OF THIS LIST
    # and into the load-bearing one above. Zeroing it then would silently
    # destroy every syscall's status word.
    xor edi, edi
    xor edx, edx
    xor esi, esi
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d

    sysretq

.size syscall_Enter, .-syscall_Enter
