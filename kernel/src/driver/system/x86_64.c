#include "driver/system/x86_64.h"
#include "time.h"
#include "CONFIG.h"
#include "serial_logging.h"

uint64_t kMPIdReg=0;

void cpuid(uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ __volatile__ (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)  // Outputs
        : "a"(*eax)                                       // Input
        : "memory"                                        // Clobbers
    );
}

// Function to read from a memory-mapped APIC register
uint32_t read_apic_register(uintptr_t reg) {
    return *((volatile uint32_t*)reg);
}

// Function to write to a memory-mapped APIC register
void write_apic_register(uintptr_t reg, uint32_t value) {
    *((volatile uint32_t*)reg) = value;
}

// Function to read the APIC ID of the current processor
//NOTE: Returns 0 if SMP is not yet initialized
uint32_t read_apic_id() {
    uint32_t id;
    
    if (kMPIdReg)
        id = read_apic_register(kMPIdReg);
    else
        id = 0;
    return id >> 24;
}

__inline__ uint64_t rdtsc()
{
    unsigned hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}

uint64_t getCR3()
{
    uint64_t cr3;
    __asm__("mov %0,cr3\n":"=r" (cr3));
    return cr3;
}

// Returns uint64_t, NOT int: a modern TSC ticks ~3-4 BILLION times a second,
// which overflows int32 and sign-extends into garbage in the uint64_t this
// lands in (kCPUCyclesPerSecond). The old int return meant every consumer of
// that global did arithmetic with a wrapped negative on real hardware —
// found when CPU-time accounting became the first consumer that CHECKED.
uint64_t tscGetCyclesPerSecond()
{
    uint64_t cyclesBefore=rdtsc();
    uint64_t cyclesDiff;
    wait(1000);
    cyclesDiff=(rdtsc()-cyclesBefore);
    printd(DEBUG_EXCEPTIONS,"tscGetCyclesPerSecond: TSC cycles per second = %lu\n",cyclesDiff);
    return cyclesDiff;
}
