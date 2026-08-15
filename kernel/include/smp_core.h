#ifndef SMP_CORE_H
#define SMP_CORE_H
#include "smp.h"

#define TIMER_SYNC_ITERATIONS 3
#define IA32_GS_BASE 0xC0000101 
#define APIC_TIMER_INIT_COUNT 0x380
#define APIC_TIMER_CURRENT_COUNT 0x390
#define APIC_LVT_TIMER 0x320
#define APIC_TIMER_DIVIDE_CONFIG 0x3E0
#define APIC_TIMER_PERIODIC_MODE_BIT 17  // Bit for setting the timer to periodic mode
#define APIC_LVT_MASK_BIT  16  // The mask bit is typically the 16th bit

#define APIC_SPURIOUS_VECTOR 0xF0

#define BASE_TIMER_VECTOR 0xF1          // Base timer vector for local APIC timer interrupts        
#define IPI_INVALIDATE_TLB_VECTOR 0x7B  // IPI vector for invalidating the TLB on APs
#define IPI_DISABLE_SCHEDULING_VECTOR 0x7C
#define IPI_ENABLE_SCHEDULING_VECTOR 0x7D
#define IPI_TIMER_SCHEDULE_VECTOR 0x7E        // IPI vector for scheduling APs
#define IPI_AP_INITIALIZATION_VECTOR 0x7f
// BOTH scheduler entry vectors MUST live in the same LAPIC priority class
// (vector >> 4 — here class 7, 0x70..0x7F).  This one was 0x81 = class 8 for
// no reason anyone recorded, which put it ABOVE the timer vector: a nudge from
// another core was therefore delivered straight into a core that was already
// mid-scheduler-pass, because the APIC only holds off interrupts of EQUAL or
// LOWER priority class.  Same class = the hardware itself holds the second
// entry pending until the first EOIs, which is the guarantee mp_inScheduler
// was only ever pretending to provide.  (2026-08-10, SCHEDULER_REENTRANCY.md.)
// Keep it here if you ever renumber: neighbors, not just ≥0x40.
#define IPI_MANUAL_SCHEDULE_VECTOR 0x7A
// CPU-time settle-on-read (accounting): each core charges its in-flight
// span locally so /proc reads never see books more than an IPI old.
// ≥0x40 REQUIRED: AP TPR is 0x30, lower vectors silently never fire there.
#define IPI_ACCT_SETTLE_VECTOR 0x82
// "Re-read the watchpoint table onto your own debug registers" (watchpoint.c).
// DR0-3/DR7 are per-CPU, so arming a watchpoint has to reach every core or it
// only watches one eighth of an eight-core machine. Same ≥0x40 rule as above.
#define IPI_WATCHPOINT_SYNC_VECTOR 0x83
// "Stop where you are, forever." Sent by a core that is about to report a
// FATAL exception and halt. Without it only the reporting core stops and the
// other seven keep scheduling — so a shell repainting on another core scribbles
// over the report before a human can read it. On real hardware that report is
// often the ONLY record (no serial capture), which makes this the difference
// between a diagnosis and a photograph of a screen that already moved on.
#define IPI_FREEZE_VECTOR 0x84

// IRQ0's vector, and the headstone of an attempt to move it (2026-08-10).
//
// TRIED AND REVERTED THE SAME DAY. Do not re-promote this without reading the
// whole note — the idea is sound, the measurement was real, and it still broke
// a supported hypervisor outright.
//
// THE ARGUMENT FOR MOVING IT: LAPIC delivery is arbitrated by priority CLASS
// (vector >> 4), so 0x20 puts the wall clock at class 2 — nearly the lowest
// thing in the machine. That is an inversion of the original design rather than
// an inheritance of it: on the 8259 IRQ0 was priority ZERO, the highest
// interrupt in the PC/AT, deliberately, because timekeeping must never be
// starved. The APIC turned priority into a function of the vector number, IRQ0
// kept its legacy vector out of inertia, and the clock was silently demoted.
// The cost is measurable: a scheduler pass holds ISR class 7, so IRQ0 cannot be
// delivered for the whole pass, and the IRR holds exactly ONE pending instance
// per vector — every tick landing in a long pass after the first is destroyed,
// not delayed. Chris measured ~30s of skew in minutes of periodic-mode soak.
//
// WHAT HAPPENED WHEN IT MOVED TO 0xE0 (class 14): QEMU was fine — 24+19 green,
// clock healthy, e1000 INTx probe confirmed on GSI 20. VBox froze at boot. The
// e1000's INTx probe reported EVERY candidate GSI silent, on both emulated card
// models, and the OS then wedged at scheduler start. A DIRECTLOG boot produced
// the decisive reading: kTicksSinceStart climbed normally to 560, and then all
// nine "GSI n stayed silent" lines carry the SAME timestamp, 597, across ~4.5
// seconds of wall clock. The clock did not merely slow — it STOPPED, and with
// it every other interrupt.
//
// That signature is one specific hardware state: a LAPIC ISR bit that is set
// and never cleared. With a class-14 bit stuck, PPR pins at 0xE0 and everything
// below is blocked forever — the NIC at class 4, the scheduler at class 7, and
// IRQ0 itself (a vector cannot re-deliver while its own ISR bit is set). One
// condition, every symptom.
//
// THE LESSON, and the reason this note is long: the promotion almost certainly
// did not CREATE that stuck bit — it made an old one lethal. At class 2 a stuck
// ISR bit blocks almost nothing and hides indefinitely; at class 14 it stops the
// machine. See the DEBTS row on the EOI window in ioapic_adopt_isa_irq for one
// mechanism that can strand exactly such a bit, found by inspection during this
// hunt (it does not by itself explain why 0x20 survives, so it is a lead, not a
// verdict). Proving the cause needs one reading: LAPIC ISR (0x100..0x170) on a
// wedged guest, looking for a set bit at the promoted vector.
//
// THE REAL FIX is not a different vector. Nothing high enough to outrank the
// scheduler's class 7 can avoid outranking devices at class 4, so this whole
// class of hazard is inherent to promoting an interrupt-counted clock. Stop
// COUNTING interrupts and READ a counter (TSC/HPET clocksource) — the DEBTS row
// carries the argument — and IRQ0's priority stops mattering at all.
#define IRQ0_APIC_VECTOR 0x20
void mpAcctSettleAll(void);
// Permanently stop every other core — the full stop that precedes a fatal
// exception report. See IPI_FREEZE_VECTOR above for why a report needs it.
void mpFreezeOtherCores(void);
// Which core's scratch ("kernel interrupt") stack contains rsp? -1 = none.
// The dispatch tripwire's oracle — see the definition's comment for the
// stack-poisoner story it guards against.
int kernel_scratch_stack_owner(uintptr_t rsp);
#define ENABLE_TIMER(val) (val & ~(1U << APIC_LVT_MASK_BIT))
#define DISABLE_TIMER(val) (val | (1U << APIC_LVT_MASK_BIT))

#define BOOTSTRAP_PROCESSOR_ID 0

extern bool kCLSInitialized;
extern bool kSMPInitDone;

void ap_initialization_handler();
void mp_enable_scheduling_vector(int apic_id);
void mp_restart_apic_timer_count();
void send_ipi(uint32_t apic_id, uint32_t vector, uint32_t delivery_mode, uint32_t level, uint32_t trigger_mode);
void send_ipi_int(uint32_t apic_id, uint32_t vector, uint32_t delivery_mode, uint32_t level, uint32_t trigger_mode, bool CLISTI);
void ap_wake_up_aps();
void ap_enable_schedulers();

// The tickless preemption lease (smp_core.c doctrine): arm a one-shot LAPIC
// deadline for the dispatched thread / stop the timer for an idle dispatch.
// Core-local by nature — call only for the core you are standing on, from
// scheduler_do's dispatch tail.
void sched_backstop_arm(core_local_storage_t *cls);
void sched_backstop_disarm(void);

static inline core_local_storage_t* get_core_local_storage(void)
{
    core_local_storage_t *cls;
    asm volatile (
        "mov %0, [gs:0]"
        : "=r"(cls)
    );
    return cls;
}

static inline core_local_storage_t* get_core_local_storage_for_core(uint64_t coreNum)
{
	return (core_local_storage_t*)kCoreLocalStorage +
                        coreNum;
}
#endif // SMP_CORE_H
