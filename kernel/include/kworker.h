#ifndef KWORKER_H
#define KWORKER_H

#include <stdbool.h>

bool kworker_thread(bool daemon);

// Get kworker out of its nap now, if it is napping. Caller holds the
// scheduler's queue lock (a pass, or a holder of scheduler_queues_lock).
void kworker_wake_locked(void);

#endif
