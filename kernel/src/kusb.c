// kusb.c — the USB drainer. kusb.h is the contract; XHCI_INTERRUPT.md § The
// design is the argument. The loop is knet's (knet.c) where the doorbell is
// concerned: take the edge before the work, park only if it is still clear.
// The work is not knet's rounds: it is one pass against a deadline
// (xhci_pass), because a budget checked between whole rounds bounds nothing.

#include "kusb.h"
#include "doorbell.h"
#include "driver/system/usb/xhci.h"
#include "kernel.h"
#include "CONFIG.h"
#include "serial_logging.h"
#include "smp_core.h"
#include "signals.h"            // kProcessSignals — nothing rings the bell before it
#include "x86_64.h"             // rdtsc

doorbell_t kUsbDoorbell;
task_t*    kKusbTask;

volatile uint64_t kKusbWakes;
volatile uint64_t kKusbWakeCyclesMax;
volatile uint64_t kKusbWakeLatencyMax;
volatile uint64_t kKusbWakeLatencySamples;
volatile uint64_t kKusbWakeLatencyTotal;
volatile uint64_t kKusbWakeLatencyLate1ms;
volatile uint64_t kKusbWakeLatencyLate10ms;
volatile uint64_t kKusbResponseLatencySamples;
volatile uint64_t kKusbResponseLatencyMax;
volatile uint64_t kKusbResponseLatencyLate10ms;

// The nap between rings. The tick rings the bell once a tick, so this is
// never the cadence anything runs at; it is the backstop for a ring that
// lands in the one window the scheduler's service points do not cover
// (DOORBELL.md).
#define KUSB_BACKSTOP_TICKS (TICKS_PER_SECOND)

// How long a pass may drain before Bluetooth's clockwork gets its turn: one
// tick. Its consumer-to-be paces audio at 15 to 20 ms, and that clockwork
// is its clock. kusb is pinned, so the TSC it reads is one core's, and a
// preemption inside the window counts against it, as wall time should.
#define KUSB_PASS_BUDGET_TICKS 1

void kusb_thread(void)
{
	thread_t* self = get_core_local_storage()->currentThread;
	doorbell_register(&kUsbDoorbell, self, "kusb");
	printd(DEBUG_USB, "kusb: running on CPU %u, %u controller(s) to drain\n",
	       get_core_local_storage()->cpu, xhci_controller_count());

	bool parked = false;   // did the park at the foot of the last turn actually sleep?
	for (;;)
	{
		// The take clears the bit and hands back the stamp in one act, before
		// the work: a ring that lands during the pass raises a fresh edge
		// and the park below returns at once (knet.c has the long form).
		uint64_t edge = doorbell_take(&kUsbDoorbell);
		// NO WORK BEFORE SIGNAL PROCESSING IS ON. kernel_init enables the
		// scheduler, waits a second, and only then turns signal processing
		// on, which is what rings this bell every tick. A pass in that
		// second would start Bluetooth's clockwork (a controller reset
		// with a two-second answer window, on the P5) and then leave it
		// unserved until the wait ended. The first ring after signals are
		// on ends this park.
		if (!kProcessSignals)
		{
			parked = doorbell_park(&kUsbDoorbell, KUSB_BACKSTOP_TICKS);
			continue;
		}
		kKusbWakes++;
		uint64_t started = rdtsc();
		if (doorbell_edge_rung(edge) && doorbell_edge_cpu(edge) == get_core_local_storage()->cpu)
		{
			uint64_t waited = doorbell_edge_elapsed(edge, started);
			if (parked)
			{
				kKusbWakeLatencySamples++;
				kKusbWakeLatencyTotal += waited;
				if (waited > kKusbWakeLatencyMax)
					kKusbWakeLatencyMax = waited;
				if (waited > kCPUCyclesPerSecond / 1000)
					kKusbWakeLatencyLate1ms++;
				if (waited > kCPUCyclesPerSecond / 100)
					kKusbWakeLatencyLate10ms++;
			}
			else
			{
				kKusbResponseLatencySamples++;
				if (waited > kKusbResponseLatencyMax)
					kKusbResponseLatencyMax = waited;
				if (waited > kCPUCyclesPerSecond / 100)
					kKusbResponseLatencyLate10ms++;
			}
		}

		uint64_t deadline = started +
			(kCPUCyclesPerSecond / TICKS_PER_SECOND) * KUSB_PASS_BUDGET_TICKS;
		bool work_left = false;
		// A refused pass (the serializing flag was held) did no work. The
		// holder's release rings this bell, so the park below is ended by
		// it; nothing is raised here, or this loop would spin against the
		// holder.
		if (xhci_pass(started, deadline, &work_left) && work_left)
		{
			// A controller was stopped by the deadline or not reached: come
			// straight back. Raised like any ring, so the wake that answers
			// it measures from here.
			doorbell_raise(&kUsbDoorbell);
		}

		uint64_t cycles = rdtsc() - started;
		if (cycles > kKusbWakeCyclesMax)
			kKusbWakeCyclesMax = cycles;

		parked = doorbell_park(&kUsbDoorbell, KUSB_BACKSTOP_TICKS);
	}
}
