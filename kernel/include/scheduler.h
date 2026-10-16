#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "stddef.h"
#include "thread.h"
#include "task.h"
#include "smp.h"

#define SCHEDULER_STACK_SIZE 0x4000
#define NO_TASK (void*)0xFFFFFFFFFFFFFFFF
#define NO_PREV (void*)NO_THREAD
#define NO_NEXT (void*)NO_THREAD
#define RUNNABLE_TICKS_INTERVAL 20
#define HIGH_PRIORITY_TICKS_BOOST 10000000

	extern task_t *kTaskList;
	extern thread_t *kThreadList;
	extern thread_t *qZombie;
	extern thread_t *qRunning;
	extern thread_t *qRunnable;
	extern thread_t *qStopped;
	extern thread_t *qUSleep;
	extern thread_t *qISleep;
	extern volatile bool kMasterSchedulerEnabled;
	extern bool mp_CoreHasRunScheduledThread[MAX_CPUS];
	extern volatile uint64_t kIdleTicks[MAX_CPUS];
	extern volatile bool mp_inScheduler[MAX_CPUS];
	extern volatile bool kSchedulerInitialized;
	// The RIP each core last iretq'd to (scheduler.S stamps it just before
	// the iretq). Debug breadcrumb only — it replaced the old R15 clobber.
	extern volatile uint64_t mp_lastIretqRIP[MAX_CPUS];
	// RFLAGS-tripwire evidence (see SCHEDULER_STRAY_WRITE.md and the
	// definitions in scheduler.c): stray values impounded per core, a
	// fire counter bumped by scheduler.S, and the reporter's high-water.
	extern volatile uint64_t mp_rflagsTripValue[MAX_CPUS];
	extern volatile uint64_t mp_rflagsTripCount;
	extern volatile uint64_t mp_rflagsTripReported;
	
	void scheduler_init();
	void scheduler_enable();
	void scheduler_disable();
	void scheduler_submit_new_task(task_t *newTask);
	void scheduler_change_thread_queue(thread_t* thread, eThreadState newState);
	// _locked variants: for callers already holding kSchedulerSwitchTasksLock
	// (scheduler_do's pass, processSignals and the wake sweeps it runs). The
	// unsuffixed names take the lock themselves — thread-context callers
	// (pipe fast-path wakes, spawn, exit-wakes-parent) use those. See the
	// queue-lock doctrine block in scheduler.c.
	void scheduler_change_thread_queue_locked(thread_t* thread, eThreadState newState);
	void scheduler_wake_isleep_thread(thread_t *w);
	void scheduler_wake_isleep_thread_locked(thread_t *w);
	// The owed wake (WAKE_OWED.md), for a waiter with no sweep behind it: a
	// parked thread is woken, one registered but not yet parked is owed the
	// wake, which the requeue that would park it pays. The deliverer owes
	// under the lock its waiter registers under; the public variant takes the
	// queue lock and never triggers a pass. The wait voids its own debt at
	// the top of every pass (scheduler_forget_owed_wake).
	void scheduler_wake_or_owe(thread_t *w);
	void scheduler_wake_or_owe_locked(thread_t *w);
	void scheduler_forget_owed_wake(thread_t *self);
	// What it has done since boot: debts owed (a wake owed to a thread that
	// already owes one is the same debt), debts paid at the park, debts
	// voided at a pass's top, and wakes refused because the slot named a
	// thread in no wait. owed = paid + voided + outstanding (WAKE_OWED.md).
	typedef struct {
		uint64_t owed, paid, voided, refused;
	} scheduler_wake_counts_t;
	scheduler_wake_counts_t scheduler_wake_counts(void);
	// The owed-wake fixture (kernel/test/test_main.c): processSignals aims
	// scheduler_wake_or_owe_locked at the target on the first pass at or
	// after the tick, then clears the target.
	extern thread_t *volatile kWakeOwedTestTarget;
	extern volatile uint64_t kWakeOwedTestTick;
	// The queue lock for thread-context callers outside scheduler.c (IF off
	// while held, restored on unlock). doorbell_park is the customer.
	uint64_t scheduler_lock_queues(void);
	void scheduler_unlock_queues(uint64_t flags);
	void scheduler_reap_zombie_thread(thread_t *thread);
	// Unlink a task from the kTaskList spine (undertaker burial phase 1 —
	// see the walker-safety note at the definition).
	void scheduler_remove_task(task_t *task);
	void scheduler_trigger(core_local_storage_t *cls);
	// Wake the thread of `task` that waits in task_wait, or owe it the wake
	// (the waiter is named because it is not always the first thread —
	// task.h, waitThread). Caller holds kDeadChildLock and triggers a pass
	// after releasing it.
	void scheduler_wake_task_waiter(task_t *task, thread_t *waiter);
    bool in_scheduler_context(void);
#endif
