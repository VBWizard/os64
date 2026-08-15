#include "driver/system/idt.h"
#include "smp_core.h"

extern void vector123();
extern void vector130();
extern void vector131();   // IPI_WATCHPOINT_SYNC_VECTOR — watchpoint.c's per-core sync
extern void vector132();   // IPI_FREEZE_VECTOR — stop every other core before a fatal report
extern void vector124();
extern void vector125();
extern void vector138();
extern void vector127();
extern void vector141();
extern void _schedule_ap();
struct IDTEntry kIDT[256];
struct IDTPointer kIDTPtr;

extern void handler_irq0_asm();
extern void handler_irq1_asm();
extern void handler_irq12_asm();
extern void handler_e1000_intx_asm();
// The unified exception path (2026-08-11): one prologue for all 32 vectors
// (exception_entry.S), one reporter (exception_report.c). The table holds the
// 32 stub addresses so the wiring below is a loop, not thirty-two lines each
// of which is a chance to typo a vector number.
extern uint64_t kExceptionStubs[32];
// EXCOLD on the cmdline wires the old per-vector stubs below instead — the
// runtime fallback for the day the new path turns out to be the thing that is
// broken. See kUseOldExceptions' comment in kernel_commandline.c. Safe to
// consult here because process_kernel_commandline runs before hardware_init.
extern bool kUseOldExceptions;
extern void divide_by_zero_handler();
extern void invalid_opcode_handler();
extern void double_fault_handler();
extern void general_protection_fault_handler();
extern void page_fault_handler();
extern void machine_check_handler();
// The vectors that had no gate until 2026-08-01 — see handler_errors.S for
// why an empty gate is worse than a handler that only names the fault.
extern void debug_exception_handler();
extern void nmi_handler();
extern void breakpoint_handler();
extern void overflow_handler();
extern void bound_range_handler();
extern void device_not_available_handler();
extern void invalid_tss_handler();
extern void segment_not_present_handler();
extern void stack_segment_handler();
extern void x87_fpu_handler();
extern void alignment_check_handler();
extern void simd_fpu_handler();

// Set an IDT entry
void set_idt_entry(int vector, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    kIDT[vector].offset_low = handler & 0xFFFF;
    kIDT[vector].selector = selector;
    kIDT[vector].ist = 0; // No IST by default
    kIDT[vector].type_attr = type_attr;
    kIDT[vector].offset_mid = (handler >> 16) & 0xFFFF;
    kIDT[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    kIDT[vector].zero = 0;
}

// Initialize IDT
void initialize_idt() {
    kIDTPtr.limit = sizeof(kIDT) - 1;
    kIDTPtr.base = (uint64_t)&kIDT;

	if (!kUseOldExceptions)
	{
		// THE UNIFIED PATH (default since 2026-08-11): every exception vector
		// gets the shared prologue from exception_entry.S, which captures the
		// full register set into a per-core, on-stack context and hands it to
		// exception_dispatch. One capture, one reporter, no vector can report
		// differently from its neighbours. All 32 vectors, including the ones
		// nobody has ever seen — an unpopulated gate surfaces as a #GP that
		// names the IDT slot instead of the real fault.
		//
		// Vector 2 is skipped ON PURPOSE: NMI is an instrument here, not a
		// fault (nmi_probe.c fires them at wedged cores and needs them to
		// RESUME), so it keeps its own stub and IST — wired below, common to
		// both paths.
		for (int v = 0; v < 32; v++)
		{
			if (v == 2)
				continue;
			set_idt_entry(v, kExceptionStubs[v], 0x28, 0x8E);
		}
	}
	else
	{
		// EXCOLD: the pre-unification wiring, kept bootable so a broken new
		// path can be diagnosed by comparison instead of by rebuild. These
		// stubs and this branch retire together once the unified path has
		// earned its keep on real hardware.
		//
		// IDT Entries for all major exceptions, all using 0x8E (Interrupt Gate)
		set_idt_entry(0x00, (uint64_t)&divide_by_zero_handler, 0x28, 0x8E); // #DE
		set_idt_entry(0x06, (uint64_t)&invalid_opcode_handler, 0x28, 0x8E); // #UD
		set_idt_entry(0x08, (uint64_t)&double_fault_handler, 0x28, 0x8E);
		set_idt_entry(0x0D, (uint64_t)&general_protection_fault_handler, 0x28, 0x8E); // #GP
		set_idt_entry(0x0E, (uint64_t)&page_fault_handler, 0x28, 0x8E); // #PF
		set_idt_entry(0x12, (uint64_t)&machine_check_handler, 0x28, 0x8E); // #MC
		// The rest of the CPU's exception range. Nothing here RECOVERS — they
		// name the fault and panic, which is strictly better than the #GP an
		// empty gate produces (that #GP names the IDT slot, not the problem,
		// and blames whatever code was running when it happened).
		set_idt_entry(0x01, (uint64_t)&debug_exception_handler, 0x28, 0x8E);      // #DB
		set_idt_entry(0x03, (uint64_t)&breakpoint_handler, 0x28, 0x8E);           // #BP
		set_idt_entry(0x04, (uint64_t)&overflow_handler, 0x28, 0x8E);             // #OF
		set_idt_entry(0x05, (uint64_t)&bound_range_handler, 0x28, 0x8E);          // #BR
		set_idt_entry(0x07, (uint64_t)&device_not_available_handler, 0x28, 0x8E); // #NM
		set_idt_entry(0x0A, (uint64_t)&invalid_tss_handler, 0x28, 0x8E);          // #TS
		set_idt_entry(0x0B, (uint64_t)&segment_not_present_handler, 0x28, 0x8E);  // #NP
		set_idt_entry(0x0C, (uint64_t)&stack_segment_handler, 0x28, 0x8E);        // #SS
		set_idt_entry(0x10, (uint64_t)&x87_fpu_handler, 0x28, 0x8E);              // #MF
		set_idt_entry(0x11, (uint64_t)&alignment_check_handler, 0x28, 0x8E);      // #AC
		set_idt_entry(0x13, (uint64_t)&simd_fpu_handler, 0x28, 0x8E);             // #XM
	}

	// #DF runs on IST1 — the per-core emergency stack tss_initialize_cpu
	// allocates. This is the ONE gate that must never depend on the faulting
	// code's RSP being sane, because a broken RSP is the most common reason
	// to arrive here at all: #PF (can't push) → #DF (can't push) → triple
	// fault, machine gone, nothing printed. With IST1 the CPU switches
	// stacks on delivery and the handler actually runs, which turns a
	// vanished window into a panic that names the thread and the core.
	// Set OUTSIDE the branch above: whichever stub owns the gate, the stack
	// switch is not optional.
	kIDT[0x08].ist = 1;

	// NMI (vector 2) — common to both paths, and NOT part of the unified
	// sweep. It runs on IST2, its own per-core stack, allocated alongside the
	// #DF stacks in tss_init_ist_stacks. Unlike every other gate here, vector
	// 2 is an INSTRUMENT as well as a fault: nmi_probe.c fires NMIs at cores
	// that have stopped taking maskable interrupts, and one very good reason
	// for a core to be in that state is that its stack is what broke. A probe
	// that borrows the patient's stack cannot diagnose a stack problem.
	set_idt_entry(0x02, (uint64_t)&nmi_handler, 0x28, 0x8E);                  // NMI
	kIDT[0x02].ist = 2;

    // Set IRQ handlers
    // IRQ0 (PIT). One entry covers both delivery paths today because
    // IRQ0_APIC_VECTOR is 0x20: the legacy PIC vector before
    // remap_irq0_to_apic() runs, and the IOAPIC vector after. If that constant
    // ever moves off 0x20 again (see its headstone in smp_core.h — it was tried
    // and reverted 2026-08-10), a SECOND entry is required here, because the
    // legacy PIC path still needs 0x20 during early boot.
    set_idt_entry(IRQ0_APIC_VECTOR, (uint64_t)&handler_irq0_asm, 0x28, 0x8E);
    set_idt_entry(0x21, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 (Keyboard), legacy-PIC vector
    set_idt_entry(0x2C, (uint64_t)&handler_irq12_asm, 0x28, 0x8E); // IRQ12 (PS/2 mouse), legacy-PIC vector

    // IOAPIC-delivered input IRQs use these HIGHER vectors instead: the APs
    // run with LAPIC TPR = 0x30 (smp_core.c), which silently masks every
    // vector below 0x40 — the legacy 0x2x vectors never fire on an AP. The
    // GUI routes keyboard/mouse at the compositor's core, so their vectors
    // must clear that bar. Same handlers; EOI logic is vector-agnostic.
    set_idt_entry(0x41, (uint64_t)&handler_irq1_asm, 0x28, 0x8E); // IRQ1 via IOAPIC
    set_idt_entry(0x4C, (uint64_t)&handler_irq12_asm, 0x28, 0x8E); // IRQ12 via IOAPIC
    set_idt_entry(0x45, (uint64_t)&handler_e1000_intx_asm, 0x28, 0x8E); // e1000 INTx via IOAPIC (≥0x40, same TPR bar)

	// SET MP handlers
	set_idt_entry(IPI_INVALIDATE_TLB_VECTOR, (uint64_t)&vector123, 0x28, 0x8E);		// Invalidate TLB IPI
	set_idt_entry(IPI_ACCT_SETTLE_VECTOR, (uint64_t)&vector130, 0x28, 0x8E);		// CPU-time settle-on-read IPI
	set_idt_entry(IPI_WATCHPOINT_SYNC_VECTOR, (uint64_t)&vector131, 0x28, 0x8E);	// Debug-register sync IPI
	set_idt_entry(IPI_FREEZE_VECTOR, (uint64_t)&vector132, 0x28, 0x8E);		// Freeze-the-machine IPI
	set_idt_entry(IPI_DISABLE_SCHEDULING_VECTOR, (uint64_t)&vector124, 0x28, 0x8E);		// AP Disable IPI
	set_idt_entry(IPI_ENABLE_SCHEDULING_VECTOR, (uint64_t)&vector125, 0x28, 0x8E);		// AP Enable IPI
	set_idt_entry(IPI_TIMER_SCHEDULE_VECTOR, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// AP Scheduler (timer ISR)
	set_idt_entry(IPI_AP_INITIALIZATION_VECTOR, (uint64_t)&vector127, 0x28, 0x8E);		// AP Initialization IPI
	set_idt_entry(IPI_MANUAL_SCHEDULE_VECTOR, (uint64_t)&_schedule_ap, 0x28, 0x8E);		// Scheduling IPI — same handler as the timer vector above, and deliberately in the SAME LAPIC priority class (see smp_core.h)

    // Load IDT
    asm volatile ("lidt %0" : : "m" (kIDTPtr));
}
