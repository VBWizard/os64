.code64
.intel_syntax noprefix
#include "utility_macros.inc"

# Vector 0x46 — the RTL8125's MSI (DOORBELL.md, 2026-09-05).
#
# The e1000's INTx stub's shape, minus the shared-line concerns: an MSI is a
# message the device writes to the LAPIC, edge-triggered and ours alone, so
# there is no IOAPIC route to own and no stranger to count. Class 4, beside
# the e1000's 0x45 — above the AP TPR bar and below the scheduler's class,
# as SCHEDULER_REENTRANCY.md's vector map requires.
#
# The real work is in C: r8125_isr reads the status register, hands the
# bits back (write-one-to-clear), and rings knet's doorbell. Microseconds,
# no locks — a top half in the BSD sense. EOI goes to the LAPIC.

.extern r8125_isr
.extern kMPApicBase
.global handler_r8125_msi_asm
.section .text
handler_r8125_msi_asm:
    cld                     # DF clear before any C runs (see exc_common)
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

    sub rsp, 8              # 16-byte alignment before calling C
    call r8125_isr
    add rsp, 8

    mov rax, qword ptr [rip + kMPApicBase]
    mov dword ptr [rax + 0xB0], 0     # LAPIC EOI

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
    iretq
