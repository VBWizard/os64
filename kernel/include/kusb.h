#ifndef KUSB_H
#define KUSB_H

// kusb.h — the USB drainer: the bottom half every xHCI controller shares
// (docs/design/pending/XHCI_INTERRUPT.md § The design).
//
// One kernel daemon, pinned to the BSP, parked on one doorbell. The
// controllers' interrupt rings the bell (xhci_isr), and so does the tick,
// once a tick, which is what a controller with no interrupt runs on. Each
// wake is one PASS (xhci_pass): every
// controller's event ring drained against a deadline, then Bluetooth's
// clockwork and the keyboards' typematic repeat, then park. kusb is the one
// consumer of the event rings once the scheduler runs, which is what lets a
// drain stop at a deadline and be picked up where it stopped.

#include <stdbool.h>
#include <stdint.h>
#include "doorbell.h"
#include "task.h"

extern doorbell_t kUsbDoorbell;
extern task_t*    kKusbTask;

// Diagnostics for /sys/usb, in TSC cycles where they are times
// (kCPUCyclesPerSecond converts). The latencies mean what knet.h says its
// own mean: WAKE latency is a ring answered after the park slept (it waited
// on the scheduler), RESPONSE latency a ring answered without a sleep (it
// waited for this loop to come round). Only a ring stamped on kusb's own
// core is a sample.
extern volatile uint64_t kKusbWakes;
extern volatile uint64_t kKusbWakeCyclesMax;          // the longest single wake, start to park
extern volatile uint64_t kKusbWakeLatencyMax;
extern volatile uint64_t kKusbWakeLatencySamples;
extern volatile uint64_t kKusbWakeLatencyTotal;
extern volatile uint64_t kKusbWakeLatencyLate1ms;
extern volatile uint64_t kKusbWakeLatencyLate10ms;
extern volatile uint64_t kKusbResponseLatencySamples;
extern volatile uint64_t kKusbResponseLatencyMax;
extern volatile uint64_t kKusbResponseLatencyLate10ms;

// The daemon's body. task.c gives the "/kusb" builtin this entry point.
void kusb_thread(void);

#endif // KUSB_H
