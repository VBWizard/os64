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
	
	void scheduler_init();
	void scheduler_enable();
	void scheduler_disable();
	void scheduler_submit_new_task(task_t *newTask);
	void scheduler_change_thread_queue(thread_t* thread, eThreadState newState);
	void scheduler_yield(core_local_storage_t *cls);
	void scheduler_trigger(core_local_storage_t *cls);
	void scheduler_wake_isleep_task(task_t *task);
    bool in_scheduler_context(void);
#endif
