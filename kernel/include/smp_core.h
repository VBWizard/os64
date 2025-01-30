#ifndef SMP_CORE_H
#define SMP_CORE_H
#include "smp.h"

#define TIMER_SYNC_ITERATIONS 5
#define IA32_GS_BASE 0xC0000101 
#define APIC_TIMER_INIT_COUNT 0x380
#define APIC_TIMER_CURRENT_COUNT 0x390
#define APIC_LVT_TIMER 0x320
#define APIC_TIMER_DIVIDE_CONFIG 0x3E0
#define APIC_TIMER_PERIODIC_MODE_BIT 17  // Bit for setting the timer to periodic mode
#define APIC_LVT_MASK_BIT  16  // The mask bit is typically the 16th bit

#define BASE_TIMER_VECTOR 0xF1          // Base timer vector for local APIC timer interrupts        
#define IPI_SCHEDULE_VECTOR 0x7E        // IPI vector for scheduling APs
#define IPI_ENABLE_SCHEDULING_VECTOR 0x7D
#define IPI_DISABLE_SCHEDULING_VECTOR 0x7C
#define IPI_INVALIDATE_TLB_VECTOR 0x7B  // IPI vector for invalidating the TLB on APs

#define ENABLE_TIMER(val) (val & ~(1U << APIC_LVT_MASK_BIT))
#define DISABLE_TIMER(val) (val | (1U << APIC_LVT_MASK_BIT))

#define BOOTSTRAP_PROCESSOR_ID 0
void ap_initialization_handler();
void mp_enable_scheduling_vector(int apic_id);
void mp_restart_apic_timer_count();
void send_ipi(uint32_t apic_id, uint32_t vector, uint32_t delivery_mode, uint32_t level, uint32_t trigger_mode);

static inline core_local_storage_t* get_core_local_storage(void)
{
    core_local_storage_t *cls;
    asm volatile (
        "mov %0, [gs:0]"
        : "=r"(cls)
    );
    return cls;
}

#endif // SMP_CORE_H