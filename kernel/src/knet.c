// knet.c — the network drainer. knet.h is the contract, DOORBELL.md the
// design. This is the bottom half of every NIC in os64, in the sense
// 4.2BSD gave the phrase: the interrupt handler does the least it can (ring
// the bell), and the work happens here, in a thread, later. Linux arrived
// at the same shape in 2021 (threaded NAPI) after twenty years of softirqs;
// os64 starts here because it already had the kernel threads and the wake
// machinery and none of the softirq apparatus.

#include "knet.h"
#include "doorbell.h"
#include "random.h"             // the pool's thread-context fold, and the drain's own moment
#include "driver/net/net_device.h"
#include "driver/net/loopback.h"
#include "driver/net/tcp.h"
#include "driver/net/dhcp.h"
#include "driver/net/e1000.h"   // kE1000IntxDivorced — the no-silent-fallbacks receipt
#include "kernel.h"
#include "CONFIG.h"
#include "serial_logging.h"
#include "BasicRenderer.h"
#include "smp_core.h"
#include "x86_64.h"             // rdtsc

doorbell_t kNetDoorbell;
task_t*    kKnetTask;

volatile uint64_t kKnetWakes;
volatile uint64_t kKnetDrainRounds;
volatile uint64_t kKnetBudgetExhausted;
volatile uint64_t kKnetWakeCyclesMax;
volatile uint64_t kKnetWakeLatencyMax;
volatile uint64_t kKnetWakeLatencySamples;
volatile uint64_t kKnetWakeLatencyTotal;
volatile uint64_t kKnetWakeLatencyLate1ms;
volatile uint64_t kKnetWakeLatencyLate10ms;
volatile uint64_t kKnetResponseLatencySamples;
volatile uint64_t kKnetResponseLatencyMax;
volatile uint64_t kKnetResponseLatencyLate10ms;

// The nap between rings. The tick rings it once a tick, so this is never the
// cadence anything runs at; it is the backstop for a ring that lands in the
// one window the service points do not cover (DOORBELL.md).
#define KNET_BACKSTOP_TICKS (TICKS_PER_SECOND)

// The drain budget per wake, before the timers get their turn. A round
// drains every device; a device that emptied its ring in a round has nothing
// to say in the next. Under a flood the rounds never come back empty, and
// the timers — retransmission, TIME_WAIT, DHCP's patience — must still run,
// so the loop hands them the CPU when the budget is spent and rings its own
// bell to come straight back. NAPI's budget, by another name.
//
// THE BUDGET IS TIME, with the round count as a backstop. A round's size is
// each driver's ring — the RTL8125's is 1024 frames, chosen for how long the
// card may wait for THIS thread (r8125.h) — so a budget counted in rounds
// grows with every ring and a flood could hold the protocol clocks off for
// seconds. Two ticks is the clocks' own granularity twice over; knet is
// pinned, so the TSC it reads is one core's (SCHEDULER.md's rule), and a
// preemption inside the window counts against the budget, as wall time
// should.
#define KNET_DRAIN_ROUNDS      64
#define KNET_DRAIN_BUDGET_TICKS 2

void knet_thread(void)
{
	thread_t* self = get_core_local_storage()->currentThread;
	doorbell_register(&kNetDoorbell, self, "knet");
	printd(DEBUG_NET, "knet: running on APIC %u, %u device(s) to drain\n",
	       get_core_local_storage()->apic_id, kNetDeviceCount);

	bool parked = false;   // did the park at the foot of the last turn actually sleep?
	for (;;)
	{
		// CLEAR BEFORE THE WORK, never after: a ring that lands during the
		// drain sets the bit again and the park below returns at once,
		// instead of vanishing into a cleared-after window (the classic lost
		// wakeup, and the reason the e1000's old doorbell flag was consumed
		// in this order too).
		// Read the edge stamp BEFORE clearing: a ring landing after the clear
		// re-stamps for the NEXT wake, and this wake answers the one before.
		// A backstop wake (nothing rung) measures nothing — the stamp it
		// would read belongs to a ring already answered. A stamp from another
		// core's TSC measures nothing either (doorbell.c's edge rule): the
		// NIC interrupts and the tick ring from this thread's own core, which
		// are the waits the receive ring has to cover; lo's producers ring
		// from wherever they run and are counted, not timed.
		//
		// TWO KINDS OF WAIT, kept apart (knet.h). A ring answered after the
		// park SLEPT waited on the scheduler — the wake, the pick, the run —
		// and that is wake latency. A ring answered without a sleep landed
		// while this loop was already running (a drain, the timers, the
		// budget's own self-ring) and waited for the loop to come round:
		// response latency — the loop's work plus any preemption of it,
		// wall time. Lumping them would let a long loop read as a late
		// wake; the split does not go further than that (knet.h).
		bool rung = kNetDoorbell.rung;
		uint64_t stamp = kNetDoorbell.rung_stamp;
		kNetDoorbell.rung = false;
		__sync_synchronize();
		kKnetWakes++;
		uint64_t started = rdtsc();
		if (rung && doorbell_stamp_apic(stamp) == get_core_local_storage()->apic_id)
		{
			uint64_t waited = doorbell_stamp_elapsed(stamp, started);
			if (parked)
			{
				kKnetWakeLatencySamples++;
				kKnetWakeLatencyTotal += waited;
				if (waited > kKnetWakeLatencyMax)
					kKnetWakeLatencyMax = waited;
				if (waited > kCPUCyclesPerSecond / 1000)
					kKnetWakeLatencyLate1ms++;
				if (waited > kCPUCyclesPerSecond / 100)
					kKnetWakeLatencyLate10ms++;
			}
			else
			{
				kKnetResponseLatencySamples++;
				if (waited > kKnetResponseLatencyMax)
					kKnetResponseLatencyMax = waited;
				if (waited > kCPUCyclesPerSecond / 100)
					kKnetResponseLatencyLate10ms++;
			}
		}

		uint64_t budget = (kCPUCyclesPerSecond / TICKS_PER_SECOND) * KNET_DRAIN_BUDGET_TICKS;
		uint32_t rounds = 0;
		bool moved;
		do
		{
			moved = false;
			for (uint32_t i = 0; i < kNetDeviceCount; i++)
			{
				net_device_t* d = kNetDevices[i];
				if (d == NULL || d->ops == NULL || d->ops->drain == NULL)
					continue;
				if (d->ops->drain(d))
					moved = true;
			}
			// lo is not a card and so not in the table (loopback.h), but its
			// queue is a receive ring like any other.
			if (kNetLoopback != NULL && kNetLoopback->ops->drain(kNetLoopback))
				moved = true;
			rounds++;
		} while (moved && rounds < KNET_DRAIN_ROUNDS && (rdtsc() - started) < budget);
		kKnetDrainRounds += rounds;
		// Thread context, so the pool may take its lock here: fold what the
		// interrupt handlers and the tick have accumulated, and add this
		// wake's own moment if frames moved (RANDOM.md).
		if (rounds > 1 || moved)
			random_add_timing(RANDOM_SOURCE_DRAIN);
		random_fold_fast_pools();
		if (moved)
		{
			// Budget spent while the last round was still moving: after the
			// timers, come straight back rather than waiting for a ring nobody
			// will send (the device already interrupted for the frames it
			// holds). "Still moving" is not "still has work" — a drain
			// reports what it did, not what remains, and the last round may
			// have emptied the ring — so a trip here can be a false one. It
			// costs one wake whose drains find nothing and park, which is the
			// doorbell's own discipline (re-evaluate, never remember an edge)
			// and cheaper than a confirmation round under every flood; the
			// counter is named for what it counts.
			kKnetBudgetExhausted++;
			// Stamped like any ring (doorbell.c's edge rule), so the wake
			// that answers it measures the timers' run and not the age of
			// whatever ring last set the bit.
			doorbell_stamp_edge(&kNetDoorbell);
			kNetDoorbell.rung = true;
		}

		// The e1000's runtime divorce (storm breaker #2 in e1000.c) happens
		// in interrupt context; announcing it is this thread's job, once.
		if (kE1000IntxDivorced)
		{
			kE1000IntxDivorced = false;
			printf("e1000: INTx wire went hostile (stranger storm) — back to polling\n");
		}

		// The protocol clocks. DHCP's is one state compare when the lease is
		// settled; TCP's walks live connections for retransmission deadlines,
		// connect timeouts and the TIME_WAIT reaper — the thing that makes
		// the stream reliable the first time a packet is lost.
		dhcp_poll();
		tcp_poll();

		uint64_t cycles = rdtsc() - started;
		if (cycles > kKnetWakeCyclesMax)
			kKnetWakeCyclesMax = cycles;

		parked = doorbell_park(&kNetDoorbell, KNET_BACKSTOP_TICKS);
	}
}
