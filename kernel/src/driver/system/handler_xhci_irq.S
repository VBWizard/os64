.code64
.intel_syntax noprefix
#include "utility_macros.inc"
#include "irq_accounting.inc"

# Vector 0x47 - every xHCI controller's MSI or MSI-X message
# (XHCI_INTERRUPT.md, D7). The RTL8125's stub's shape: a message is
# edge-triggered and ours alone, so there is no IOAPIC route to own. Class 4,
# beside the NICs' - above the AP TPR bar and below the scheduler's class.
#
# The work is in C: xhci_isr asks every interrupting controller whether it
# signalled, acknowledges the ones that did, and rings kusb's doorbell. No
# locks. EOI goes to the LAPIC.

.extern xhci_isr
.extern kMPApicBase
.global handler_xhci_irq_asm
.section .text
handler_xhci_irq_asm:
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
	IRQ_ACCOUNT_ENTER 88

    sub rsp, 8              # 16-byte alignment before calling C
    call xhci_isr
    add rsp, 8

    mov rax, qword ptr [rip + kMPApicBase]
    mov dword ptr [rax + 0xB0], 0     # LAPIC EOI
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
    iretq
