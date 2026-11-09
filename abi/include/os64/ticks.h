#ifndef OS64_ABI_TICKS_H
#define OS64_ABI_TICKS_H

// The kernel's monotonic clock, as SYSCALL_TICKS hands it to ring 3.
//
// This is the STOPWATCH, not the calendar. It starts at boot, only counts
// up, and can never be set — which is exactly what makes it safe to measure
// intervals with: CPU%, timeouts, benchmarks, uptime. Calendar time (epoch,
// dates, anything a human sets or an NTP daemon corrects) is a DIFFERENT
// instrument with a different job and will arrive as its own named thing
// the day something demands to print a date. Unix spent ~30 years measuring
// durations against a settable clock before CLOCK_MONOTONIC repaired it;
// os64 gets to have the distinction on day one of caring about time.
// (You can build the calendar from the stopwatch plus one boot-time anchor;
// you can never build a trustworthy stopwatch from a settable calendar.)
//
// `per_second` is the ACTIVE scheduler tick rate, reported live — and
// deliberately NOT an ABI constant. Baking the rate into a header would
// freeze it into every compiled binary; reporting it here means a kernel
// rebuilt at a faster tick makes every existing program's arithmetic (and
// sleep's granularity) better with zero recompiles. The everyday sums:
//
//     uptime seconds   = ticks / per_second
//     ms per tick      = 1000 / per_second     (the honest floor of sleep())
//     CPU%% of a thread = its ticks-delta / this ticks-delta, two samples apart
//
// THE SAME STOPWATCH HAS A FINER FACE: micros() (SYSCALL_MICROS, 59) returns
// microseconds since boot as a bare number, read from the CPU's cycle
// counter. `ticks` counts timer interrupts the kernel received, and under
// load it LOSES some (a syscall's interrupts-off window can outlast a tick
// period; DEBTS § wall-clock hardening); micros reads a counter and loses
// none. The two drift apart by exactly the lost ticks. Measure an INTERVAL
// with micros; divide two tick deltas for a CPU%, because both sides of
// that ratio lose the same ticks and the error cancels.
typedef struct {
	uint64_t ticks;        // scheduler ticks since boot — monotonic, never jumps
	uint32_t per_second;   // the ACTIVE tick rate (TICKS_PER_SECOND; 100 today)
} os64_ticks_t;

#endif // OS64_ABI_TICKS_H
