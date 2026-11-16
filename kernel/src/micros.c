// micros.c — microseconds since boot, off the CPU's cycle counter. One
// clock for ring 3's micros() and for kernel code whose stamps a program
// compares with it (the audio position).
//
// The rate is kCPUCyclesPerSecond, fixed for the life of the boot
// (kernel.c says why a moving rate is worse). Divided in two steps — whole
// seconds, then the remainder — so the multiply cannot overflow at any
// uptime: cycles × 1,000,000 passes 2^64 within a couple of hours on a
// 3GHz part.
//
// ticks() counts interrupts the BSP received and LOSES some under load
// (DEBTS § wall-clock hardening); this reads a counter and loses none. The
// two drift apart by exactly the lost ticks. A program measuring an
// interval wants this one; a CPU% that divides two tick deltas wants ticks.
//
// MONOTONIC BY CONSTRUCTION, not by assumption. kBootTSC was read on the
// BSP; this rdtsc runs on whatever core the caller is on, and the house
// rule (nvme.c's completion poll) is that a TSC read on one core may not
// be compared with one from another — QEMU and WSL2 have both been seen
// to hand cores counters with different offsets. So the raw figure is
// never returned as-is: it is folded through a global high-water mark
// (compare-and-swap, the only cross-core state), and a caller can only
// ever see a value at least as large as the last one anybody was given.
// A core whose counter lags therefore STALLS the clock by its offset
// rather than running it backward, and a core behind the boot anchor
// reads 0 rather than 2^64 - offset. A stall is still a wrong interval on
// a desynchronized machine; the boot-time sync check that would make it a
// right one is booked (DEBTS § micros).

#include "micros.h"
#include "driver/system/x86_64.h"   // rdtsc

extern uint64_t kCPUCyclesPerSecond;   // the TSC's rate, fixed at boot
extern uint64_t kBootTSC;              // the clock's zero (kernel.c stamps it first thing)

static volatile uint64_t s_micros_high_water;

uint64_t micros_now(void)
{
	uint64_t cps = kCPUCyclesPerSecond;
	if (cps == 0)
		return 0;   // asked before calibration — nothing has, but 0 beats a divide fault
	uint64_t now = rdtsc();
	uint64_t cycles = now > kBootTSC ? now - kBootTSC : 0;
	uint64_t seconds = cycles / cps;
	uint64_t rest = cycles % cps;
	uint64_t us = seconds * 1000000UL + (rest * 1000000UL) / cps;

	for (;;)
	{
		uint64_t seen = s_micros_high_water;
		if (us <= seen)
			return seen;   // behind somebody's reading: hand out theirs, never less
		if (__sync_bool_compare_and_swap(&s_micros_high_water, seen, us))
			return us;
	}
}
