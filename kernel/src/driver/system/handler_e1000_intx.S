.code64
.intel_syntax noprefix
#include "utility_macros.inc"

# Vector 0x45 — the e1000's INTx doorbell (first wired 2026-08-06, the day
# os64's packets stopped waiting to be noticed).
#
# Same caller-saved-register ballet as the keyboard's IRQ1 stub, minus the
# legacy-PIC EOI fallback: this vector only exists after e1000_enable_intx()
# has routed a GSI through the IOAPIC, and that routing only happens in APIC
# mode — there is no PIC era for this handler, so there is no PIC path.
#
# The real work (read ICR, tally causes, raise the rx-work flag) lives in C:
# e1000_isr in e1000.c. It is a top half in the BSD sense — microseconds,
# no locks; the ring drain stays in processSignals until the bottom-half
# slice. EOI goes to the LAPIC unconditionally.

.extern e1000_isr
.extern kMPApicBase
.global handler_e1000_intx_asm
.section .text
handler_e1000_intx_asm:
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

    sub rsp, 8              # Align stack to 16 bytes before calling C code
    call e1000_isr
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
