#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include "limine.h"
#include "BasicRenderer.h"

/// @brief Number of IRQ0 ticks per second
extern uint64_t kTicksPerSecond;
/// @brief Is kernel initialization complete?
extern volatile bool kInitDone;
extern bool kEnableSMP;
// true (the default) = tickless scheduling: idle AP timers masked, work wakes
// cores via directed nudge IPIs. false = SCHED=periodic legacy mode (every
// core ticks at 100Hz; diagnostic/repro only). See kernel.c for the ruling.
extern bool kTicklessScheduler;
extern bool kSchedBackstopEnabled;   // NOBACKSTOP clears it (kernel.c doctrine)
extern int  kSchedBackstopMS;        // lease ms: SCHED_BACKSTOP_MS default,
                                     // BACKSTOP=<ms> per boot (validated 1..1000)
extern bool kEnableKWorker;
extern volatile uint64_t kSystemStartTime, kUptime;
extern volatile uint64_t kSystemCurrentTime;
extern volatile uint64_t kTicksSinceStart;
extern int kTimeZone;

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_hhdm_request hhmd_request;
extern volatile struct limine_module_request module_request;
extern volatile struct limine_smp_request smp_request;
extern struct limine_module_response *limine_module_response;
extern struct limine_memmap_response *memmap_response;
extern int kTimeZone;
extern BasicRenderer kRenderer;
extern uint64_t kCPUCyclesPerSecond;

// BOOTMARK: boot-phase mile-markers, printed via printf (direct to screen +
// serial, unqueued — visible even when the log path itself is the suspect).
// Each line carries tick AND raw TSC, so two markers give wall time (TSC
// delta) and tick-advance per phase — the tick-starvation ratio that cracked
// the 54-second VBox boot (2026-07-11). Enable with the BOOTMARKS cmdline
// flag; costs nothing when off. Callers need x86_64.h (rdtsc) in scope.
extern bool kEnableBootmarks;
#define BOOTMARK(name) do { if (kEnableBootmarks) \
    printf("BOOTMARK %-22s tick=%lu tsc=%lu\n", name, kTicksSinceStart, rdtsc()); } while (0)

void kernel_main();

#endif
