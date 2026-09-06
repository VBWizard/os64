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
	// The queue lock for thread-context callers outside scheduler.c (IF off
	// while held, restored on unlock). doorbell_park is the customer.
	uint64_t scheduler_lock_queues(void);
	void scheduler_unlock_queues(uint64_t flags);
	void scheduler_reap_zombie_thread(thread_t *thread);
	// Unlink a task from the kTaskList spine (undertaker burial phase 1 —
	// see the walker-safety note at the definition).
	void scheduler_remove_task(task_t *task);
	void scheduler_trigger(core_local_storage_t *cls);
	// Wake the thread of `task` that is parked in task_wait (the waiter is
	// named because it is not always the first thread — task.h, waitThread).
	void scheduler_wake_task_waiter(task_t *task, thread_t *waiter);
    bool in_scheduler_context(void);
#endif
