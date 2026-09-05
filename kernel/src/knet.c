// knet.c — the network drainer. knet.h is the contract, DOORBELL.md the
// design. This is the bottom half of every NIC in os64, in the sense
// 4.2BSD gave the phrase: the interrupt handler does the least it can (ring
// the bell), and the work happens here, in a thread, later. Linux arrived
// at the same shape in 2021 (threaded NAPI) after twenty years of softirqs;
// os64 starts here because it already had the kernel threads and the wake
// machinery and none of the softirq apparatus.

#include "knet.h"
#include "doorbell.h"
#include "driver/net/net_device.h"
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

// The nap between rings. The tick rings it once a tick, so this is never the
// cadence anything runs at; it is the backstop for a ring that lands in the
// one window the service points do not cover (DOORBELL.md).
#define KNET_BACKSTOP_TICKS (TICKS_PER_SECOND)

// Drain rounds per wake before the timers get their turn. A round drains
// every device; a device that emptied its ring in a round has nothing to
// say in the next. Under a flood the rounds never come back empty, and the
// timers — retransmission, TIME_WAIT, DHCP's patience — must still run, so
// the loop hands them the CPU after this many rounds and rings its own bell
// to come straight back. NAPI's budget, by another name.
#define KNET_DRAIN_ROUNDS 64

void knet_thread(void)
{
	thread_t* self = get_core_local_storage()->currentThread;
	doorbell_register(&kNetDoorbell, self, "knet");
	printd(DEBUG_NET, "knet: running on APIC %u, %u device(s) to drain\n",
	       get_core_local_storage()->apic_id, kNetDeviceCount);

	for (;;)
	{
		// CLEAR BEFORE THE WORK, never after: a ring that lands during the
		// drain sets the bit again and the park below returns at once,
		// instead of vanishing into a cleared-after window (the classic lost
		// wakeup, and the reason the e1000's old doorbell flag was consumed
		// in this order too).
		kNetDoorbell.rung = false;
		__sync_synchronize();
		kKnetWakes++;
		uint64_t started = rdtsc();

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
			rounds++;
		} while (moved && rounds < KNET_DRAIN_ROUNDS);
		kKnetDrainRounds += rounds;
		if (moved)
		{
			// Budget spent with work still on a ring: after the timers, come
			// straight back rather than waiting for a ring nobody will send
			// (the device already interrupted for the frames it holds).
			kKnetBudgetExhausted++;
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

		doorbell_park(&kNetDoorbell, KNET_BACKSTOP_TICKS);
	}
}
