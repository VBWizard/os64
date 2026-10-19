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
extern volatile uint64_t kKnetBudgetExhausted;   // wakes whose budget (time, or the round cap) ran out while the last round still moved — knet rang itself to come back and check
extern volatile uint64_t kKnetWakeCyclesMax;
// HOW LONG A RING WAITED, from the bell's leading edge (doorbell_t.rung_stamp)
// to the top of the loop that answers it — the time a NIC's ring has to
// cover. Only a ring stamped on knet's own core is a sample (another core
// has another clock; a backstop wake has no ring), and the samples are split
// by what the ring waited ON:
//
//   WAKE latency     the park had SLEPT, so the ring waited on the
//                    scheduler — the wake, the pick, the run. Pinned to the
//                    BSP, knet waits behind whatever the BSP is running.
//                    `Samples` is the average's denominator; the two "late"
//                    counts are samples over a millisecond and over ten.
//   SERVICE latency  the ring landed while this loop was already running
//                    (a drain, the timers, the budget's self-ring) and
//                    waited on the loop itself: how long the drain IS, not
//                    how late it was scheduled.
//
// A NIC's rx_overruns climbing beside wake late_10ms means knet was
// scheduled late; beside service late_10ms, that the drain itself outlasts
// the ring (wake_cycles_max says by how much).
extern volatile uint64_t kKnetWakeLatencyMax;
extern volatile uint64_t kKnetWakeLatencySamples;
extern volatile uint64_t kKnetWakeLatencyTotal;
extern volatile uint64_t kKnetWakeLatencyLate1ms;
extern volatile uint64_t kKnetWakeLatencyLate10ms;
extern volatile uint64_t kKnetServiceLatencySamples;
extern volatile uint64_t kKnetServiceLatencyMax;
extern volatile uint64_t kKnetServiceLatencyLate10ms;

// The daemon's body. task.c gives the "/knet" builtin this entry point.
void knet_thread(void);

#endif // KNET_H
