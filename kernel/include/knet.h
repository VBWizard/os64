#ifndef KNET_H
#define KNET_H

// knet.h — the network drainer: the bottom half every NIC shares.
//
// One kernel daemon, pinned to the BSP, parked on one doorbell. A NIC's
// interrupt handler rings the bell (microseconds, no locks: the top half);
// knet wakes, drains every registered device until its rings are empty,
// runs the protocol timers, and parks again. The tick rings the same bell
// once a tick, which is what keeps a NIC with no interrupt (virtio, or any
// NIC under NETPOLL) at exactly the cadence it always had, and what runs
// the timers when the wire is quiet. DOORBELL.md carries the argument; the
// short form is that the stack now runs in a thread, on nobody's lock but
// its own, and a reader wakes when its bytes land rather than at the tick.

#include <stdbool.h>
#include "doorbell.h"
#include "task.h"

extern doorbell_t kNetDoorbell;
extern task_t*    kKnetTask;

// Diagnostics for /sys/net/knet: how often the daemon woke, how many drain
// rounds it ran, the longest single wake, and how long rings waited for
// their wake — all in TSC cycles (kCPUCyclesPerSecond converts).
extern volatile uint64_t kKnetWakes;
extern volatile uint64_t kKnetDrainRounds;
extern volatile uint64_t kKnetBudgetExhausted;   // wakes that spent their budget (time, or the round cap) with work left
extern volatile uint64_t kKnetWakeCyclesMax;
// WAKE LATENCY: from a bell's leading edge (doorbell_t.rung_tsc) to the top
// of the loop that answers it. This is the time a NIC's ring has to cover,
// and the number that says whether knet is slow to DRAIN (wake_cycles_max)
// or slow to be SCHEDULED (this) — pinned to the BSP, it waits behind
// whatever the BSP is running when a ring finds it runnable but off the
// CPU. The two "late" counts are wakes that waited longer than a
// millisecond and longer than ten.
extern volatile uint64_t kKnetWakeLatencyMax;
extern volatile uint64_t kKnetWakeLatencyTotal;
extern volatile uint64_t kKnetWakeLatencyLate1ms;
extern volatile uint64_t kKnetWakeLatencyLate10ms;

// The daemon's body. task.c gives the "/knet" builtin this entry point.
void knet_thread(void);

#endif // KNET_H
