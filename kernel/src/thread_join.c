// thread_join.c — ring-3 threads: birth, the answer, and the wait.
//
// See thread_join.h for why the handle points at a join object rather than
// at the thread_t. This file is the whole of os64's user-threading
// machinery; the scheduler needed no changes at all, because a thread has
// always been the unit it schedules — there was simply never a way for a
// program to ask for a second one.

#include <stdint.h>
#include <stdbool.h>
#include "kernel.h"
#include "serial_logging.h"
#include "spinlock.h"
#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "memory/paging.h"
#include "smp_core.h"
#include "signals.h"
#include "scheduler.h"
#include "task.h"
#include "thread.h"
#include "thread_join.h"

extern uintptr_t kHHDMOffset;

static thread_join_t* kThreadJoinList = NULL;
static spinlock_t s_list_lock;

static void thread_join_unref(thread_join_t* j)
{
	if (__sync_sub_and_fetch(&j->refcount, 1) > 0)
		return;

	// Last reference: unlink and free. Both droppers (the exiting thread
	// and the closing handle) can run on different cores, so the list
	// surgery takes the lock even though the refcount already decided.
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	thread_join_t** pp = &kThreadJoinList;
	while (*pp != NULL && *pp != j)
		pp = &(*pp)->next;
	if (*pp == j)
		*pp = j->next;
	spinlock_release_irqrestore(&s_list_lock, lf);

	printd(DEBUG_THREAD, "thread_join: freed join object for thread 0x%08lx\n", j->threadID);
	kfree(j);
}

thread_join_t* thread_join_create(void* taskv, uint64_t entry, uint64_t arg,
                                  uint64_t exit_stub)
{
	task_t* task = (task_t*)taskv;

	thread_join_t* j = kmalloc(sizeof(*j));
	if (j == NULL)
		return NULL;

	// A ring-3 thread: createThread already gives it its own user stack AND
	// its own kernel stack, each with guard pages, each at a unique VA from
	// the task's virtual-address counter (task_reserve_task_virt). That last
	// part is why this feature needed no address-space work — whoever wrote
	// task_alloc_guarded_stack solved the multi-thread case years before
	// there were multiple threads.
	thread_t* t = createThread(task, false);
	if (t == NULL)
	{
		kfree(j);
		return NULL;
	}

	// Fresh entry: the function to run, its argument, and a stack whose
	// first return address is the userland exit stub — so a thread that
	// simply RETURNS lands in libos64's stub, which calls thread_exit.
	// (A kernel address could not go here: ring 3 would fault the moment
	// it tried to `ret` into the kernel's half of the map.)
	t->regs.RIP = entry;
	t->regs.RDI = arg;

	// AFFINITY, and the trap in it: "run anywhere" is THREAD_NO_AFFINITY,
	// which is 0xFFFFFFFFFFFFFFFF — NOT zero. createThread doesn't set the
	// field and kmalloc zeroes every allocation, so a thread born here
	// arrives claiming mp_apic == 0, and 0 is a perfectly valid APIC id:
	// the BSP. Every thread a program created would then be PINNED to core
	// 0 and they would timeshare one core while seven sat idle — the
	// scheduler faithfully doing what it was told. (Found within an hour of
	// the feature existing, by a hog that wouldn't spread. task_create
	// dodges it only because its callers pass THREAD_NO_AFFINITY by hand.)
	//
	// Inherit the creating task's main thread rather than hardcoding the
	// sentinel: if that thread is pinned somewhere on purpose, its threads
	// belong there too, and for every ordinary program it means "anywhere".
	t->mp_apic = task->threads->mp_apic;

	// Write the return address into the new thread's user stack. The stack
	// lives at a task-local lower-half VA that the kernel's own page tables
	// do not map, so it is reached the documented way: walk the TASK's
	// tables to the physical page, then write through the HHDM alias
	// (valid while allocated). See CLAUDE.md § Writing to Task Memory.
	uintptr_t phys_rsp = paging_walk_paging_table((pt_entry_t*)task->pml4v, t->regs.RSP);
	if (phys_rsp == 0 || phys_rsp == 0xbadbadba)
	{
		printd(DEBUG_THREAD, "thread_join_create: new thread's stack VA 0x%016lx is not mapped in %s — refusing\n",
		       t->regs.RSP, task->exename);
		kfree(j);
		return NULL;   // the thread_t leaks; thread teardown is a standing debt
	}
	*(uintptr_t*)(phys_rsp | kHHDMOffset) = (uintptr_t)exit_stub;

	j->threadID = t->threadID;
	j->exited = false;
	j->retval = 0;
	j->refcount = 2;      // one for the handle, one for the running thread
	j->waiter = NULL;

	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	j->next = kThreadJoinList;
	kThreadJoinList = j;
	spinlock_release_irqrestore(&s_list_lock, lf);

	// Chain the thread onto its task's own list (taskNext — NOT next/prev,
	// which the run queues own) and hand it to the scheduler.
	// scheduler_change_thread_queue takes the queue lock itself (the
	// _locked variant is for callers already inside it — see the queue-lock
	// discipline note at the top of scheduler.c). The taskNext splice is
	// done first and is safe unlocked: this thread is not reachable by any
	// walker until the queue insert publishes it.
	t->taskNext = task->threads->taskNext;
	task->threads->taskNext = t;
	scheduler_change_thread_queue(t, THREAD_STATE_RUNNABLE);

	printd(DEBUG_THREAD, "thread_join_create: task %s (id %lu) started thread 0x%08lx at RIP=0x%016lx "
	       "arg=0x%016lx stack=0x%016lx exit_stub=0x%016lx\n",
	       task->exename, task->taskID, t->threadID, entry, arg, t->regs.RSP, exit_stub);
	return j;
}

void thread_join_finish(uint64_t threadID, int64_t retval)
{
	thread_join_t* j = NULL;
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	for (thread_join_t* p = kThreadJoinList; p != NULL; p = p->next)
		if (p->threadID == threadID)
		{
			j = p;
			break;
		}
	spinlock_release_irqrestore(&s_list_lock, lf);

	if (j == NULL)
	{
		// No join object: the handle was closed AND the thread already
		// dropped its reference, or this was never a joinable thread.
		printd(DEBUG_THREAD, "thread_join_finish: thread 0x%08lx exited with %ld — nobody was listening\n",
		       threadID, retval);
		return;
	}

	j->retval = retval;
	j->exited = true;     // publish the ANSWER before the flag that says
	                      // an answer exists — a reader that sees exited
	                      // must never read a stale retval
	printd(DEBUG_THREAD, "thread_join_finish: thread 0x%08lx exited with %ld (waiter %s)\n",
	       threadID, retval, j->waiter ? "parked" : "none");
	thread_join_unref(j);   // the thread's own reference
}

long thread_join_read(thread_join_t* j, int64_t* out)
{
	core_local_storage_t* cls = get_core_local_storage();
	thread_t* self = cls->currentThread;

	for (;;)
	{
		if (sigset_any(self->signals.sigind, SIGNALS_TERMINATING))
			return THREAD_JOIN_ERR_INTERRUPTED;

		if (j->exited)
		{
			*out = j->retval;
			printd(DEBUG_THREAD | DEBUG_DETAILED, "thread_join_read: thread 0x%08lx answered %ld\n",
			       j->threadID, j->retval);
			return 0;
		}

		// Register, then park. The sweep only wakes threads that have
		// genuinely reached ISLEEP (see udp_conn.c for the hour that rule
		// cost), so a waiter caught mid-park stays registered for the
		// next pass instead of losing its wake entirely.
		j->waiter = self;
		signal_raise(SIGSLEEP, kTicksSinceStart + TICKS_PER_SECOND, self);
	}
}

void thread_join_close(thread_join_t* j)
{
	printd(DEBUG_THREAD, "thread_join_close: handle for thread 0x%08lx released (%s)\n",
	       j->threadID, j->exited ? "already exited" : "still running — detached");
	j->waiter = NULL;
	thread_join_unref(j);
}

void thread_join_wake_if_ready(void)
{
	uint64_t lf = spinlock_acquire_irqsave(&s_list_lock);
	for (thread_join_t* j = kThreadJoinList; j != NULL; j = j->next)
	{
		thread_t* w = j->waiter;
		if (w != NULL && j->exited && w->threadState == THREAD_STATE_ISLEEP)
		{
			j->waiter = NULL;
			scheduler_wake_isleep_thread_locked(w);
			printd(DEBUG_THREAD | DEBUG_DETAILED, "thread_join: woke reader of thread 0x%08lx\n",
			       j->threadID);
		}
	}
	spinlock_release_irqrestore(&s_list_lock, lf);
}
