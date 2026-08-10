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

// THE WALL CLOCK OUTRANKS EVERYTHING (2026-08-10).
//
// The LAPIC decides who may preempt whom by PRIORITY CLASS = vector >> 4, and
// IRQ0 spent os64's whole life at its legacy PC/AT vector 0x20 — class 2,
// nearly the LOWEST thing in the machine. That is an inversion of the original
// design, not an inheritance of it: on the 8259, IRQ0 was priority ZERO, the
// highest interrupt in the PC/AT, deliberately, because timekeeping must never
// be starved. The APIC changed priority from a wire position to a function of
// the vector number, IRQ0 kept 0x20 out of inertia, and the system clock was
// silently demoted to the bottom.
//
// The cost was real and measured: a scheduler pass holds ISR class 7, so IRQ0
// at class 2 could not be delivered for the ENTIRE pass, and the LAPIC's IRR
// holds exactly ONE pending instance per vector — so every tick that landed in
// a long pass after the first was simply lost. Chris measured ~30 seconds of
// skew against the host clock in a few minutes of periodic-mode soak, and the
// same lost ticks made `top` (which refreshes on tick counts) visibly sluggish.
//
// 0xE0 = class 14: above every device IRQ and every IPI we route, below the
// spurious vector. Safe at that height precisely because handler_irq0_timer.S
// is a true micro-handler — three `lock inc`s on globals nobody else owns, no
// locks, no subsystem state — so it can nest into anything that is not `cli`,
// and it cannot nest into itself (same vector, ISR bit).
//
// NOTE: the IDT keeps its entry at 0x20 as well. Before remap_irq0_to_apic()
// runs, IRQ0 still arrives from the legacy PIC at 0x20.
#define IRQ0_APIC_VECTOR 0xE0
void mpAcctSettleAll(void);
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
