#include "driver/system/x86_64.h"
#include "time.h"
#include "CONFIG.h"
#include "serial_logging.h"

extern volatile uint64_t kSystemCurrentTime;
extern uint64_t kCPUCyclesPerSecond;

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
// Window length is the caller's call (TSCCAL= cmdline via kernel_init):
// precision is ±1 tick over the window, so seconds=1 is ±1%, 15 is ±0.07%.
// Clamped to [1, 60] — zero would divide by it, and a boot that
// deliberately stares at a counter for a minute has confused calibration
// with meditation.
uint64_t tscGetCyclesPerSecond(uint32_t seconds)
{
    if (seconds < 1)
        seconds = 1;
    if (seconds > 60)
        seconds = 60;
    uint64_t cyclesBefore=rdtsc();
    uint64_t cyclesDiff;
    wait(seconds * 1000);
    cyclesDiff=(rdtsc()-cyclesBefore) / seconds;
    printd(DEBUG_SYSTEM, "tscGetCyclesPerSecond: TSC cycles per second = %lu (%us window)\n", cyclesDiff, seconds);
    return cyclesDiff;
}

// Continuous TSC recalibration — the boot calibration's ±1% is a design
// floor, not a bug: wait(1000) is a 100-tick window, and one tick of
// boundary slop is one part in a hundred. Every µs the accounting ever
// reports is converted through that number, so the P5's idle threads all
// read 101.0% forever (the +1% was drawn in the machine's first second of
// life and never rechecked). The cure is window size: re-derive
// cycles-per-second from ΔTSC/Δepoch-seconds over an EVER-GROWING window —
// 60s in, the window is 6000 ticks (±0.017%); ten minutes in, ±0.002%.
// The number converges toward truth for as long as the machine is up.
//
// Called from the BSP's scheduler pass ONLY (apic_id 0): the base and
// every re-read are same-core TSC samples, so cross-core TSC desync — the
// documented landmine — never enters the math. Sanity band: a reading
// >25% away from the boot calibration is discarded as clock lunacy (a
// hypervisor pausing the world mid-window can manufacture one; VBox's
// tick stream is a known suspect) — better to keep a 1%-wrong constant
// than adopt a 45%-wrong "correction".
void tsc_recalibrate(void)
{
    static uint64_t baseTSC = 0;
    static uint64_t baseEpoch = 0;
    static uint64_t nextCheckEpoch = 0;

    if (baseTSC == 0)
    {
        baseTSC = rdtsc();
        baseEpoch = kSystemCurrentTime;
        nextCheckEpoch = baseEpoch + 60;
        return;
    }
    if (kSystemCurrentTime < nextCheckEpoch)
        return;

    uint64_t dS = kSystemCurrentTime - baseEpoch;
    if (dS == 0)
        return;
    uint64_t newRate = (rdtsc() - baseTSC) / dS;

    uint64_t lo = kCPUCyclesPerSecond - kCPUCyclesPerSecond / 4;
    uint64_t hi = kCPUCyclesPerSecond + kCPUCyclesPerSecond / 4;
    if (newRate >= lo && newRate <= hi)
    {
        // DEBUG_BOOT, not DEBUG_SCHEDULER: one line a minute about the
        // machine's clock is boot-subsystem news, and it must survive a
        // trimmed debug set — the first verification boot had it on
        // SCHEDULER and the recalibrator ran gagged for four minutes.
        // Now DEBUG_DIAG because DEBUG_BOOT isn't a catch-all either. 😏
        printd(DEBUG_DIAG, "tsc_recalibrate: %lu -> %lu cycles/sec (window %lus)\n",
               kCPUCyclesPerSecond, newRate, dS);
        kCPUCyclesPerSecond = newRate;
    }
    nextCheckEpoch = kSystemCurrentTime + 60;
}
