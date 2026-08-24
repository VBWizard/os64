#include "scheduler.h"
#include "kmalloc.h"
#include "CONFIG.h"
#include "serial_logging.h"
#include "kernel.h"
#include "panic.h"
#include "memset.h"
#include "x86_64.h"
#include "smp_core.h"
#include "gdt.h"
#include "tss.h"
#include "strcmp.h"
#include "paging.h"
#include "strstr.h"

volatile uint64_t mp_isrSavedRAX[MAX_CPUS],mp_isrSavedRBX[MAX_CPUS],mp_isrSavedRCX[MAX_CPUS],mp_isrSavedRDX[MAX_CPUS],mp_isrSavedRSI[MAX_CPUS],
                  mp_isrSavedRDI[MAX_CPUS],mp_isrSavedRBP[MAX_CPUS],mp_isrSavedCR0[MAX_CPUS],mp_isrSavedCR3[MAX_CPUS],mp_isrSavedCR4[MAX_CPUS],
                  mp_isrSavedDS[MAX_CPUS],mp_isrSavedES[MAX_CPUS],mp_isrSavedFS[MAX_CPUS],mp_isrSavedGS[MAX_CPUS],mp_isrSavedSS[MAX_CPUS],
                  mp_isrSavedRSP[MAX_CPUS],mp_isrSavedRFlags[MAX_CPUS],mp_isrSavedErrorCode[MAX_CPUS],mp_isrSavedRIP[MAX_CPUS],
                  mp_isrSavedCS[MAX_CPUS],mp_isrSavedCR2[MAX_CPUS],mp_isrSavedTR[MAX_CPUS], mp_isrSavedStack[MAX_CPUS],
				  mp_isrSavedR8[MAX_CPUS], mp_isrSavedR9[MAX_CPUS], mp_isrSavedR10[MAX_CPUS], mp_isrSavedR11[MAX_CPUS], mp_isrSavedR12[MAX_CPUS], 
				  mp_isrSavedR13[MAX_CPUS], mp_isrSavedR14[MAX_CPUS], mp_isrSavedR15[MAX_CPUS];

// The scheduler's exit breadcrumb: the RIP each core last iretq'd to, stamped
// in scheduler.S right before the iretq. This used to ride in R15 itself —
// which silently clobbered every resumed thread's real R15. Harmless at -O0
// (the compiler almost never keeps a live value there), catastrophic the day
// an -O2 build does. Same information, now in working storage where a halted
// guest gives it up by name:  (gdb) p/x mp_lastIretqRIP[core]
volatile uint64_t mp_lastIretqRIP[MAX_CPUS];

// The RFLAGS tripwire's evidence lockers (SCHEDULER_STRAY_WRITE.md).
// scheduler.S judges mp_isrSavedRFlags at the point of consumption — just
// before the iretq, which is AFTER the proven-clean copy from the thread
// struct and therefore inside the stray writer's window. A value with TF
// set or any bit outside the legal RFLAGS mask is impounded here (the raw
// qword IS the clue: a .rodata pointer names a table, a .text pointer names
// a function, a stack address names a frame), counted, and sanitized so the
// boot survives to report it. processSignals prints new impounds.
volatile uint64_t mp_rflagsTripValue[MAX_CPUS];
volatile uint64_t mp_rflagsTripCount;
volatile uint64_t mp_rflagsTripReported;

//List of all of the active tasks in the system.  Each task has one or more threads to be scheduled
task_t *kTaskList;
//List of all of the active threads in the system.  Use next & prev to access threads in the list
thread_t *kThreadList = NO_THREAD;
//List of all of the zombie threads.  These are threads which don't have a parent thread
thread_t *qZombie = NO_THREAD;
//List of all of the currently running threads.
thread_t *qRunning = NO_THREAD;
//List of all of the threads waiting to run.
thread_t *qRunnable = NO_THREAD;
//List of all of the threads that have been stopped.
thread_t *qStopped = NO_THREAD;
//List of all of the threads which are in a blocking sleep (waiting for event to happen)
thread_t *qUSleep = NO_THREAD;
//List of all of the threads which are in a non-blocking sleep (just ... waiting)
thread_t *qISleep = NO_THREAD;

volatile uint64_t kTaskSwitchCount=0;
volatile uint64_t kIdleTicks[MAX_CPUS] = {0};
volatile uintptr_t mp_schedStack[MAX_CPUS]; //Loaded by scheduler when it is called 
volatile uint32_t mp_timesEnteringScheduler[MAX_CPUS] = {0};
volatile bool mp_inScheduler[MAX_CPUS] = {false};
volatile bool mp_waitingForScheduler[MAX_CPUS] = {false};
volatile bool kMasterSchedulerEnabled = false;
volatile bool kSchedulerInitialized = false;
bool mp_schedulerEnabled[MAX_CPUS] = {false};
uint64_t kSchedulerCallCount = 0;
volatile int kSchedulerSwitchTasksLock;
bool mp_CoreHasRunScheduledThread[MAX_CPUS] = {false};
bool mp_schedulerTaskSwitched[MAX_CPUS] = {false};
uint8_t mp_SchedulerTaskSwitched[MAX_CPUS] = {false};
uint64_t mp_ForkReturn[MAX_CPUS] = {false};
//pipe_t *kActiveSTDOUT, *kActiveSTDIN, *kActiveSTDERR;

extern pt_entry_t kKernelPML4v;
extern uint64_t kHHDMOffset;
extern bool kTicklessScheduler;
extern bool kSMPInitDone;

#define VERIFY_QUEUE(q) if (q<0 || (q>THREAD_STATE_ISLEEP && q!=THREAD_STATE_ZOMBIE)) panic("VERIFY_QUEUE: Invalid state %u\n", q)

// ── Queue-lock discipline (the tickless fan-out made this load-bearing) ─────
//
// kSchedulerSwitchTasksLock protects the scheduler's doubly-linked queues
// (qRunning/qRunnable/qISleep/...) and the kTaskList spine. Two kinds of
// customer take it:
//
//   1. The scheduler's own entry path (scheduler_do, processSignals): raw
//      test-and-set with IF *enabled* — safe because mp_inScheduler[] already
//      guarantees this core cannot re-enter the scheduler on top of itself,
//      and no other ISR touches these queues.
//
//   2. EVERYONE ELSE — thread-context wake/spawn paths (pipe wakes, task
//      spawn, exit-wakes-parent). These MUST hold the lock with IF disabled
//      (spinlock.h doctrine): a tick landing on a core that holds this lock
//      in thread context would enter scheduler_do, spin on its own lock, and
//      self-deadlock. For years these paths ran unlocked and got away with it
//      because every unpinned thread lived on the BSP, where IF-masked
//      syscalls made queue mutations same-core-exclusive by accident. The
//      fan-out (b4ee823) put user threads on APs, turning that accident into
//      a genuine cross-core race: a pipe wake on an AP relinking the same
//      list the BSP's tick pass was walking corrupted the chain and wedged
//      VBox solid (BSP looping in a torn list WHILE holding the lock — every
//      core piles up behind it, only raw IRQ echo survives).
//
// scheduler_queues_lock/unlock are the type-2 idiom. Anything already inside
// the lock calls the *_locked variants below; the public names lock for you.

// ── TEMP DIAG (VBox slow-motion hunt, 2026-07-30) ───────────────────────
// Once a second the BSP's pass prints these to serial (DEBUG_BOOT). The
// question they answer: WHERE do the cycles go when VBox drops into slow
// motion — tick starvation (ticks vs TSC-ms disagree), an IPI storm
// (nudge/trigger/send counts), lock convoy (max spin), or settle timeouts
// (smp_core.c's counters)? Racy unlocked updates — fine for diagnosis.
// REMOVE when the hunt closes.
volatile uint64_t kDiagNudgeUnpinned = 0;
volatile uint64_t kDiagNudgePinned = 0;
volatile uint64_t kDiagTriggerCalls = 0;
volatile uint64_t kDiagLockMaxSpins = 0;
volatile uint64_t kDiagPassCount[MAX_CPUS] = {0};
volatile uint64_t kDiagRunnableLen = 0;
// Lease expiries per core: passes on a tickless AP driven by the backstop's
// one-shot timer (vector 0x7E) rather than a nudge. NOT a diag temp — this is
// the observable the backstop test asserts on (a preempted hog = a nonzero,
// growing count on its core), and the counter /sys/cpu can surface someday.
volatile uint64_t kSchedBackstopFires[MAX_CPUS] = {0};
extern volatile uint64_t kDiagIPISends, kDiagICRMaxSpins, kDiagSettleBroadcasts,
	kDiagSettleTimeouts, kDiagSettleMaxSpins, kDiagSettleLastLateAPIC;
extern volatile uint32_t kDiagLastVector[MAX_CPUS], kDiagLVT[MAX_CPUS];
extern uint32_t apic_in_service_vector(void);
extern uint32_t read_apic_register(uintptr_t reg);
extern volatile uintptr_t kMPApicBase;

static inline uint64_t scheduler_queues_lock(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\tpop %0" : "=r"(flags) :: "memory");
	__asm__ volatile("cli" ::: "memory");
	uint64_t spins = 0;                                // TEMP DIAG
	while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1))
	{
		spins++;                                       // TEMP DIAG
		__builtin_ia32_pause();
	}
	if (spins > kDiagLockMaxSpins)                     // TEMP DIAG
		kDiagLockMaxSpins = spins;                     // TEMP DIAG
	return flags;
}

static inline void scheduler_queues_unlock(uint64_t flags)
{
	__sync_lock_release(&kSchedulerSwitchTasksLock);
	if (flags & 0x200)  // restore IF only if the caller had interrupts enabled
		__asm__ volatile("sti" ::: "memory");
}

const char* THREAD_STATE_NAMES[] = {"None","Running","Runnable","Stopped","Uninterruptable Sleep","Interruptable Sleep","Exited","Zombie"};

// NOTE: there was a scheduler_invoke_vector() here that entered the scheduler
// with a direct software `int`.  Removed with scheduler_yield(): a software
// int never sets the APIC in-service bit, so such entries dodge the
// EOI-based nesting protection that every hardware/IPI entry gets.  All
// scheduler entries now go through a real LAPIC interrupt (timer or
// send_ipi in scheduler_trigger).

static bool scheduler_thread_can_run_on_core(thread_t *thread, core_local_storage_t *cls)
{
	if (thread == NULL || cls == NULL) {
		return false;
	}

	return thread->mp_apic == THREAD_NO_AFFINITY || thread->mp_apic == cls->apic_id;
}

static void scheduler_nudge_parked_aps(thread_t *thread)
{
	if (!kTicklessScheduler || !kSMPInitDone || thread == NULL || thread->idleThread) {
		return;
	}

	core_local_storage_t *cls = get_core_local_storage();
	uint64_t current_apic_id = cls ? cls->apic_id : BOOTSTRAP_PROCESSOR_ID;

	// PINNED work nudges its designated core, as it always has.
	if (thread->mp_apic != THREAD_NO_AFFINITY) {
		if (thread->mp_apic == BOOTSTRAP_PROCESSOR_ID ||
		    thread->mp_apic == current_apic_id ||
		    mp_inScheduler[thread->mp_apic]) {
			return;
		}
		printd(DEBUG_SCHEDULER | DEBUG_DETAILED,
			"scheduler_nudge_parked_aps: nudging APIC %lu for pinned thread 0x%08x (task %s)\n",
			thread->mp_apic, thread->threadID,
			((task_t*)thread->ownerTask)->exename);
		kDiagNudgePinned++;                             // TEMP DIAG
		send_ipi(thread->mp_apic, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
		return;
	}

	// UNPINNED work recruits an idle core. This used to read "in BSP
	// scheduler mode, generic work stays on the BSP" — a conservative rule
	// from the nudge slice that quietly turned a 12-core P5 into a 2-core
	// machine: three hogs left ten cores asleep while two of them split
	// the BSP (Chris caught it on the way to bed, 2026-07-30). A runnable
	// thread with nowhere pinned deserves the first idle core that can
	// take it; the nudged core's own scheduler pass pulls from qRunnable,
	// so this hands out a WAKE-UP, not a thread — the queue stays the one
	// source of truth. One core per nudge (no thundering herd): each new
	// runnable recruits at most one sleeper, which is exactly the arrival
	// rate that created the demand.
	//
	// Cross-core reads here are safe: CLS and thread structs live in the
	// shared upper half, and a stale read costs one wasted (or missed)
	// nudge, self-healed by the next scheduler pass. KNOWN LIMIT, not new
	// tonight: tickless APs don't preempt, so a compute-bound tenant owns
	// its core until it blocks or dies (kworker read 0% while a hog held
	// its core — pre-existing nudge-only semantics; AP fairness is its own
	// future slice).
	for (int i = 0; i < kMPCoreCount; i++)
	{
		uint32_t apic_id = kCPUInfo[i].apicID;
		if (apic_id == BOOTSTRAP_PROCESSOR_ID || apic_id == current_apic_id)
			continue;
		// NOT gated on mp_schedulerEnabled, deliberately: under tickless the
		// enable ISR leaves AP timers masked and never sets that flag, so
		// requiring it excluded every core the tickless mode itself parked — the
		// first fan-out test recruited exactly ONE core (kworker's, enabled
		// by its pin) and left the rest asleep. _schedule_ap gates only on
		// re-entry (mp_inScheduler), so a manual nudge is safe for a core
		// that has never been "enabled": its pass handles the never-ran
		// case explicitly. kSMPInitDone (checked above) is the real
		// readiness gate.
		if (mp_inScheduler[apic_id])
			continue;

		core_local_storage_t *target = get_core_local_storage_for_core(apic_id);
		if (target == NULL || target->currentThread == NULL ||
		    target->currentThread == NO_THREAD)
			continue;
		if (!target->currentThread->idleThread)
			continue;   // busy core — an idle one may still be ahead

		printd(DEBUG_SCHEDULER | DEBUG_DETAILED,
			"scheduler_nudge_parked_aps: recruiting idle APIC %u for thread 0x%08x (task %s)\n",
			apic_id, thread->threadID,
			((task_t*)thread->ownerTask)->exename);
		kDiagNudgeUnpinned++;                           // TEMP DIAG
		send_ipi(apic_id, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
		return;
	}
	// No idle core: the busy ones and the BSP pick it up on their own passes.
}

void scheduler_enable()
{
	core_local_storage_t *cls = get_core_local_storage();
	//This will enable the scheduler for the calling core
	mp_schedulerEnabled[cls->apic_id] = true;
}

void scheduler_disable()
{
	core_local_storage_t *cls = get_core_local_storage();
	//This will disable the scheduler for the calling core
	mp_schedulerEnabled[cls->apic_id] = false;
}

thread_t* scheduler_get_queue(eThreadState state)
{
    switch (state)
    {
		case THREAD_STATE_NONE:
			return NULL;
			break;
        case THREAD_STATE_RUNNABLE:
            return qRunnable;
            break;
        case THREAD_STATE_RUNNING:
            return qRunning;
            break;
        case THREAD_STATE_ZOMBIE:
            return qZombie;
            break;
        case THREAD_STATE_USLEEP:
            return qUSleep;
            break;
        case THREAD_STATE_ISLEEP:
            return qISleep;
            break;
        case THREAD_STATE_STOPPED:
            return qStopped;
            break;
        default:
            printd(DEBUG_SCHEDULER,"scheduler_get_queue: Invalid queue 0x%02X - %s",state,THREAD_STATE_NAMES[state]);
            return NULL;
            break;
    }
}

void scheduler_init()
{
	kTaskList = NO_TASK;
	kThreadList = NO_THREAD;
	kSchedulerInitialized = true;
    printd(DEBUG_SCHEDULER,"\tInitialized kThreadList @ 0x%08x, sizeof(thread_t)=0x%02X\n",kThreadList,sizeof(thread_t));

    for (int cnt=0;cnt<kMPCoreCount;cnt++)
    {
        printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "\tAllocating stack for CPU %u, 0x%04x bytes\n",cnt,SCHEDULER_STACK_SIZE);
        mp_schedStack[cnt] = (uintptr_t)kmalloc_aligned(0x4000);
		uintptr_t base = mp_schedStack[cnt] & ~(kHHDMOffset);
        printd(DEBUG_SCHEDULER, "\t\tStack is at 0x%016x\n",mp_schedStack[cnt]);
        paging_map_pages((uintptr_t*)kKernelPML4v, (uintptr_t)mp_schedStack[cnt], base, SCHEDULER_STACK_SIZE/PAGE_SIZE, 0x7);
        mp_schedStack[cnt] += SCHEDULER_STACK_SIZE - sizeof(uintptr_t);
        printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "\t\tAdjusted stack is at 0x%016x\n",mp_schedStack[cnt]);
    }
}

task_t* scheduler_find_open_next_task_slot()
{
	task_t* list=kTaskList;
	int slotNum = 0;
	//There are no tasks in this list so just return the list
	if (list==NO_TASK)
		return list;
	while (list->next!=(task_t*)NO_TASK)
	{
		list=list->next;
        slotNum++;
	}
	printd(DEBUG_SCHEDULER, "scheduler_find_open_next_task_slot: Found open next task at slot # %u\n", slotNum);
	return list;
}

thread_t* scheduler_find_open_next_queue_slot(thread_t *queue)
{
	thread_t *q=queue;
	int slotNum = 0;
	while (q->next!=(thread_t*)NO_THREAD)
	{
		q=q->next;
		slotNum++;
	}
	printd(DEBUG_SCHEDULER, "scheduler_find_open_next_queue_slot: Found available thread slot at slot # %u\n", slotNum);
	return q;
}

void set_queue_head(eThreadState queue, thread_t* thread)
{
	switch(queue)
	{
		case THREAD_STATE_RUNNABLE:
			qRunnable = thread;
			break;
		case THREAD_STATE_RUNNING:
			qRunning = thread;
			break;
		case THREAD_STATE_STOPPED:
			qStopped = thread;
			break;
		case THREAD_STATE_ISLEEP:
			qISleep = thread;
			break;
		case THREAD_STATE_USLEEP:
			qUSleep = thread;
			break;
		case THREAD_STATE_ZOMBIE:
			qZombie = thread;
			break;
		default:
			panic("set_queue_head: Queue %u not found\n", queue);
	}
	thread->prev = NO_PREV;
}

void scheduler_add_thread_to_queue(eThreadState queue, thread_t *thread)
{
	VERIFY_QUEUE(queue);
	bool found = false;
	thread_t *slot = scheduler_get_queue(queue);

	printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "scheduler_add_thread_to_queue: Adding thread 0x%08x to queue %s\n", thread->threadID, THREAD_STATE_NAMES[queue]);

	if (slot!=NO_THREAD)
		do
		{
			if (slot!=NO_THREAD)
			{
				if (slot->next==NO_NEXT)
				{
					found = true;
					break;
				}
				else
					slot = slot->next;
			}
		} while (slot!=NO_THREAD);
		
	if (slot==NO_THREAD)
	{
		set_queue_head(queue, thread);
		found = true;
	}
	else if (found)
	{
		slot->next = thread;
		thread->prev = slot;
		thread->next=NO_NEXT;
	}
	else
		panic("scheduler_add_thread_to_queue: Could not add thread with id %u to queue %s\n", thread->threadID, THREAD_STATE_NAMES[queue]);
}

void scheduler_set_queue_empty(eThreadState queue)
{
	switch (queue)
	{
		case THREAD_STATE_NONE:
			break;
        case THREAD_STATE_RUNNABLE:
            qRunnable = NO_THREAD;
            break;
        case THREAD_STATE_RUNNING:
            qRunning = NO_THREAD;
            break;
        case THREAD_STATE_ZOMBIE:
             qZombie = NO_THREAD;
            break;
        case THREAD_STATE_USLEEP:
            qUSleep = NO_THREAD;
            break;
        case THREAD_STATE_ISLEEP:
            qISleep = NO_THREAD;
            break;
        case THREAD_STATE_STOPPED:
            qStopped = NO_THREAD;
            break;
        default:
            break;
	}
}

void scheduler_remove_thread_from_queue(eThreadState queue, thread_t *thread)
{
    VERIFY_QUEUE(queue);

    thread_t *head = scheduler_get_queue(queue);
    bool found = false;
    if (head != NO_THREAD) {
        thread_t *slot = head;
        do {
            if (slot == thread) {
                // If this was the only item
                if (slot->prev == NO_THREAD && slot->next == NO_THREAD) {
                    scheduler_set_queue_empty(queue);
                }
                else {
                    // If removing head
                    if (slot->prev == NO_THREAD) {
                        set_queue_head(queue, slot->next);
                    } else {
                        slot->prev->next = slot->next;
                    }

                    // If there's a successor
                    if (slot->next != NO_THREAD) {
                        slot->next->prev = slot->prev;
                    }
                }
                found = true;
                break;
            }
            slot = slot->next;
        } while (slot != NO_THREAD);
    }

    if (!found) {
        panic("scheduler_remove_thread_from_queue: Unable to find thread with id %u in queue %s\n",
              thread->threadID, THREAD_STATE_NAMES[queue]);
    }

    // Make sure the removed thread’s links are cleared
    thread->next = thread->prev = NO_THREAD;
}

// Unlink a task from the kTaskList spine. Undertaker use (task.c burial
// phase 1): after this, no NEW walker can reach the task. Deliberately does
// NOT clear the corpse's own prev/next — procfs walks this list locklessly,
// and a reader standing ON the corpse mid-walk must still be able to follow
// its intact ->next back into the live list. The corpse's links go stale
// harmlessly during its grace pass and die with it in task_destroy.
void scheduler_remove_task(task_t *task)
{
	if (task == NULL || task == (task_t *)NO_TASK)
		return;

	uint64_t flags = scheduler_queues_lock();
	task_t *prev = (task_t *)task->prev;
	task_t *next = (task_t *)task->next;

	if (kTaskList == task)
		kTaskList = next;   // may be NO_TASK — same sentinel the append uses
	else if (prev != NULL && prev != (task_t *)NO_TASK)
		prev->next = next;

	if (next != NULL && next != (task_t *)NO_TASK)
		next->prev = prev;
	scheduler_queues_unlock(flags);
}

void scheduler_reap_zombie_thread(thread_t *thread)
{
    if (thread == NULL) {
        return;
    }

    // Thread-context caller (waitpid) — IF must be off while holding the
    // queue lock or a tick on this core self-deadlocks in scheduler_do.
    uint64_t flags = scheduler_queues_lock();
    if (thread->threadState == THREAD_STATE_ZOMBIE) {
        scheduler_remove_thread_from_queue(THREAD_STATE_ZOMBIE, thread);
        thread->threadState = THREAD_STATE_NONE;
    }
    scheduler_queues_unlock(flags);
}

// The relink itself. Caller MUST hold kSchedulerSwitchTasksLock (scheduler_do,
// processSignals, console_wake_if_ready, and the locking wrapper below are the
// customers). The nudge at the bottom fires an IPI while the lock is held —
// that's fine, send_ipi never waits; the woken AP just spins briefly on this
// same lock before its pass proceeds.
void scheduler_change_thread_queue_locked(thread_t* thread, eThreadState newState)
{
    printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*\tchangeThreadQueue: Changing thread state for 0x%04x from %s to %s\n",
            thread->threadID,
            THREAD_STATE_NAMES[thread->threadState],
            THREAD_STATE_NAMES[newState]);
	//A thread can be in no queue when this method is called.  If it is then don't do the remove step
	if (thread->threadState!=THREAD_STATE_NONE)
    	scheduler_remove_thread_from_queue(thread->threadState,thread);
    if (thread->threadState==THREAD_STATE_RUNNING)  //old state
    {
        thread->totalRunTicks+=(kTicksSinceStart-thread->lastRunStartTicks);
    }
    thread->threadState=newState;
	    scheduler_add_thread_to_queue(newState,thread);
	    if (newState==THREAD_STATE_RUNNABLE)
	    {
	        thread->prioritizedTicksInRunnable=0;
	        thread->runnableSinceTick=kTicksSinceStart;   // cache-home rule's clock starts
	        scheduler_nudge_parked_aps(thread);
	    }
	    else if (newState==THREAD_STATE_RUNNING)
	        thread->lastRunStartTicks=kTicksSinceStart;
}

// Public entry: takes the queue lock (IF off — see the doctrine block up top)
// around the relink. This is the one every thread-context caller uses: pipe
// wakes, boot-time submissions, anything not already inside the scheduler.
void scheduler_change_thread_queue(thread_t* thread, eThreadState newState)
{
	uint64_t flags = scheduler_queues_lock();
	scheduler_change_thread_queue_locked(thread, newState);
	scheduler_queues_unlock(flags);
}

// Wake a thread parked in ISLEEP — the pipe/console blocking-loop wake. Only
// a thread that has ACTUALLY parked can be woken here; one still RUNNING has
// not finished registering as a waiter — the caller's level-triggered sweep
// catches it next pass (pipe.c documents that contract). The ISLEEP check,
// the SIGSLEEP cancel, and the relink must be one atomic act under the queue
// lock: without it, a wake running in the WAKER's thread context (possibly
// on an AP since the fan-out) races processSignals' tick-driven walk of the
// very same queue on the BSP.
//
// _locked variant: for callers already inside the lock (processSignals'
// level-triggered pipe sweep). Public variant: thread-context fast paths
// (pipe_read/pipe_write direct wakes).
void scheduler_wake_isleep_thread_locked(thread_t *w)
{
	if (w == NULL || w->threadState != THREAD_STATE_ISLEEP)
		return;

	sigset_del(&w->signals.sigind, SIGSLEEP);      // cancel the backstop sleep
	w->signals.sigdata[SIGSLEEP] = 0;
	scheduler_change_thread_queue_locked(w, THREAD_STATE_RUNNABLE);
}

void scheduler_wake_isleep_thread(thread_t *w)
{
	if (w == NULL)
		return;

	uint64_t flags = scheduler_queues_lock();
	scheduler_wake_isleep_thread_locked(w);
	scheduler_queues_unlock(flags);
}

void scheduler_submit_new_task(task_t *newTask)
{
	if (newTask->threads==NULL)
		panic("scheduler_submit_new_task: Task does not have a thread assigned\n");

	// Fully initialize the new node BEFORE it becomes reachable: lockless
	// walkers (procfs) follow ->next with no protection, so publishing the
	// node first and setting its links after hands them a torn read.
	newTask->next=NO_TASK;
	newTask->prev=NO_TASK;

	// The kTaskList walk-and-append and the queue insert ride under the queue
	// lock: spawn runs in the PARENT's thread context, which since the
	// fan-out can be an AP racing the BSP's tick pass ("hog 1000 &" from a
	// recruited husk was one of the wedges).
	uint64_t flags = scheduler_queues_lock();
	task_t* slot=scheduler_find_open_next_task_slot();
	if (slot==NO_TASK)
	{
		kTaskList = newTask;
	}
	else
	{
		slot->next=newTask;
		newTask->prev=slot;
	}

	scheduler_change_thread_queue_locked(newTask->threads, THREAD_STATE_RUNNABLE);
	scheduler_queues_unlock(flags);
}

thread_t* scheduler_get_running_thread(uint64_t threadID)
{
	thread_t *slot = scheduler_get_queue(THREAD_STATE_RUNNING);
	bool found = false;

	if (slot!=NO_THREAD)
	do
	{
		if (slot->threadID == threadID)
		{
			found = true;
			break;
		}
		slot = slot->next;
	} while (slot!=NO_NEXT);
	
	if (!found)
		panic("scheduler_get_running_thread: Can't find thread with id %lu in running queue", threadID);
	return slot;
}

// Dump the saved interrupt-frame registers for one core.
//
// This used to take an `unconditional` flag that forced the dump out by
// TEMPORARILY SETTING kDebugLevel |= DEBUG_SCHEDULER and restoring it after.
// That was removed 2026-08-01, for three reasons, in ascending order of
// severity:
//
//   1. It didn't work. The printd below requires DEBUG_SCHEDULER *and*
//      DEBUG_EXTRA_DETAILED (printd's gate is all-bits-must-match); the
//      flip set only the first, so the dump stayed silent regardless.
//   2. kDebugLevel is GLOBAL and this kernel is SMP. Between the set and
//      the restore, every other core's DEBUG_SCHEDULER printd escaped its
//      gate — which is how stray scheduler banners turned up in a log
//      whose owner had deliberately gated them off (Chris spotted them
//      while tailing logd's file and correctly refused to believe printd
//      was at fault).
//   3. Two cores in that window can make it PERMANENT: A sets the bit, B
//      saves a copy that already has it, A restores (clearing), B restores
//      its copy (setting it forever).
//
// The lesson worth keeping: a debug flag that one core can flip on behalf
// of all cores is not a debug flag, it is a race. If a message must always
// print, give it a level that is always on — never mutate the gate.
void debug_print_registers(uint64_t apic_id, char* prefix)
{
    printd(DEBUG_SCHEDULER | DEBUG_EXTRA_DETAILED,"*\t%s: CR3=0x%016lx, CS=0x%04X, RIP=0x%016lx, SS=0x%04X, DS=0x%04X, RAX=0x%016lx, RBX=0x%016lx, RCX=0x%016lx, RDX=0x%016lx, RSI=0x%016lx, RDI=0x%016lx, RSP=0x%016lx, RBP=0x%016lx, FLAGS=0x%016lx\n",
            prefix,
			mp_isrSavedCR3[apic_id],
            mp_isrSavedCS[apic_id],
            mp_isrSavedRIP[apic_id],
            mp_isrSavedSS[apic_id],
            mp_isrSavedDS[apic_id],
            mp_isrSavedRAX[apic_id],
            mp_isrSavedRBX[apic_id],
            mp_isrSavedRCX[apic_id],
            mp_isrSavedRDX[apic_id],
            mp_isrSavedRSI[apic_id],
            mp_isrSavedRDI[apic_id],
            mp_isrSavedRSP[apic_id],
            mp_isrSavedRBP[apic_id],
            mp_isrSavedRFlags[apic_id]);
}

void scheduler_store_thread(core_local_storage_t *cls, thread_t* thread)
{
    int apic_id = cls->apic_id;
	task_t* task = (task_t*)cls->currentThread->ownerTask;
    if (apic_id > 0 && mp_timesEnteringScheduler[apic_id]==1)
    {
        printd(DEBUG_SCHEDULER,"storeISRSavedRegs: AP hasn't been through the scheduler before, not saving registers\n");
        return;
    }
    if (mp_isrSavedCS[apic_id] == 0)
        panic("scheduler_store_thread: AP%u storing CS=0 into thread 0x%x (%s), RIP=0x%016lx\n",
            apic_id, thread->threadID, task->exename, mp_isrSavedRIP[apic_id]);

    if (thread->execDontSaveRegisters)
    {
        printd(DEBUG_SCHEDULER, "* storeISRSavedRegs: ***Process %u exec'd, not saving registers***\n", task->taskID);
        thread->execDontSaveRegisters = false;
    }
    else
    {
        thread->regs.CS=mp_isrSavedCS[apic_id];
        thread->regs.RIP=mp_isrSavedRIP[apic_id];
        thread->regs.SS=mp_isrSavedSS[apic_id];
        thread->regs.DS=mp_isrSavedDS[apic_id];
        thread->regs.RAX=mp_isrSavedRAX[apic_id];
        thread->regs.RBX=mp_isrSavedRBX[apic_id];
        thread->regs.RCX=mp_isrSavedRCX[apic_id];
        thread->regs.RDX=mp_isrSavedRDX[apic_id];
        thread->regs.RSI=mp_isrSavedRSI[apic_id];
        thread->regs.RDI=mp_isrSavedRDI[apic_id];
        thread->regs.RSP=mp_isrSavedRSP[apic_id];
        thread->regs.RBP=mp_isrSavedRBP[apic_id];
        // R8-R15 joined the save set 2026-08-14. The asm prologue has always
        // captured them into the per-core arrays, but nothing carried them
        // into thread->regs — so a thread resuming on a DIFFERENT core ran
        // with whatever R8-R15 the previous tenant of that core left behind.
        // Two consequences, both observed in the hog -n 6 crash frames: a
        // migrated thread computes on a stranger's registers, and ring 3
        // inherits raw kernel pointers (R10 held a kernel text address in a
        // user segfault report — an info leak on top of the corruption).
        // Same-core resumes were accidentally correct, which is why this
        // survived every single-core test since SMP bring-up.
        thread->regs.R8=mp_isrSavedR8[apic_id];
        thread->regs.R9=mp_isrSavedR9[apic_id];
        thread->regs.R10=mp_isrSavedR10[apic_id];
        thread->regs.R11=mp_isrSavedR11[apic_id];
        thread->regs.R12=mp_isrSavedR12[apic_id];
        thread->regs.R13=mp_isrSavedR13[apic_id];
        thread->regs.R14=mp_isrSavedR14[apic_id];
        thread->regs.R15=mp_isrSavedR15[apic_id];
        thread->regs.RFLAGS=mp_isrSavedRFlags[apic_id];
        thread->regs.ES=mp_isrSavedES[apic_id];
        thread->regs.FS=mp_isrSavedFS[apic_id];
        thread->regs.GS=mp_isrSavedGS[apic_id];
        thread->regs.CR3=mp_isrSavedCR3[apic_id];
    }
    debug_print_registers(apic_id, "save (or not)");
}

void scheduler_load_thread(core_local_storage_t *cls, thread_t* thread)
{
	// Dispatch history for /proc (thread.h has the doctrine): cls is the
	// TARGET core's storage, so this is correct even when the BSP loads a
	// thread onto another core under tickless scheduling.
	thread->lastRunApicID = cls->apic_id;
	//task_t* task = cls->currentThread->ownerTask;
	//task_t* ownerTask = ((task_t*)cls->currentThread->ownerTask)->ownerTask;
	uint64_t apic_id = cls->apic_id;
	thread_t* forkedThread = (thread_t*)thread->forkedThread;

	cls->currentThread = thread;
	cls->threadID = thread->threadID;
	// AND THE TASK, IN THE SAME BREATH (2026-08-23). cls->task used to be
	// assigned ~57 lines further down scheduler_do, which left a window where
	// the core carried the NEW thread and still named the OLD task — and the
	// two are supposed to describe one thing.
	//
	// It cost a program. Signal delivery read the handler table out of
	// cls->task, hit that window right after a thread migrated cores, asked
	// the IDLE task whether a handler was installed, got "no", and killed a
	// program that had one. Intermittently, which is the worst way to find
	// out. (Chris, on sigdemo: "sometimes it exits if I hit CTRL+C at just
	// the right time".)
	//
	// The signal path was fixed to take the task from the THREAD, which is
	// unambiguous and stays the rule for anything deciding on a task. This
	// closes the window itself, because ~40 other places read cls->task and
	// none of them should have to know it can lag: a value that is only
	// SOMETIMES right is a trap laid for whoever reads it next.
	cls->task = (task_t *)thread->ownerTask;
    mp_isrSavedCS[apic_id]=thread->regs.CS;
    mp_isrSavedRIP[apic_id]=thread->regs.RIP;
    mp_isrSavedSS[apic_id]=thread->regs.SS;
    mp_isrSavedDS[apic_id]=thread->regs.DS;
    mp_isrSavedRAX[apic_id]=thread->regs.RAX;
    mp_isrSavedRBX[apic_id]=thread->regs.RBX;
    mp_isrSavedRCX[apic_id]=thread->regs.RCX;
    mp_isrSavedRDX[apic_id]=thread->regs.RDX;
    mp_isrSavedRSI[apic_id]=thread->regs.RSI;
    mp_isrSavedRDI[apic_id]=thread->regs.RDI;
    mp_isrSavedRSP[apic_id]=thread->regs.RSP;
    mp_isrSavedRBP[apic_id]=thread->regs.RBP;
    // The restore half of the R8-R15 fix (see scheduler_store_thread): the
    // asm epilogue loads R8-R15 from these arrays, so without this a
    // dispatched thread received the core's previous tenant's values.
    mp_isrSavedR8[apic_id]=thread->regs.R8;
    mp_isrSavedR9[apic_id]=thread->regs.R9;
    mp_isrSavedR10[apic_id]=thread->regs.R10;
    mp_isrSavedR11[apic_id]=thread->regs.R11;
    mp_isrSavedR12[apic_id]=thread->regs.R12;
    mp_isrSavedR13[apic_id]=thread->regs.R13;
    mp_isrSavedR14[apic_id]=thread->regs.R14;
    mp_isrSavedR15[apic_id]=thread->regs.R15;
    // ── BORROWED-STACK TRIPWIRE (2026-08-12, the stack poisoner's headstone) ─
    // A context about to be dispatched whose saved RSP lies inside ANY core's
    // per-core scratch stack was PARKED THERE — it slept or was preempted
    // while standing on a stack that is bolted to a core and re-issued to
    // that core's next dying thread / cikc call. Resuming it puts two live
    // contexts on one stack, silently stomping each other's frames at
    // deterministic offsets: that was the poisoner that forged top's %s
    // pointers (3, 4), the settle loop's -29874 index, and the 0x7ec6xxxx
    // wrapped-write fatals. The exit path was cured by the teardown split
    // (task.c); this tripwire stands guard over every door we HAVEN'T found.
    // The cikc-hosted disk closes were the standing suspects, and one of them
    // was guilty: the burial close (task.c) borrowed the scratch stack from
    // kworker with interrupts ON, so a scheduling IPI could park it here.
    // call_in_kernel_context masks interrupts across the borrow now
    // (task_exit_asm.S, 2026-08-20) — every borrower, not just that one.
    // The tripwire stays anyway: it is the proof, not the cure, and the next
    // door will not announce itself either. Unlike the TF tripwire below, there is no safe
    // "clear and continue": a collided stack cannot be un-collided, so the
    // only honest move is a loud stop that NAMES the door.
    {
        int scratch_owner = kernel_scratch_stack_owner(thread->regs.RSP);
        if (scratch_owner >= 0)
        {
            task_t *bs_task = (task_t *)thread->ownerTask;
            panic("BORROWED-STACK TRIPWIRE: thread 0x%08x (%s) was parked on core %d's scratch stack "
                  "(saved RSP 0x%016lx) and core %u tried to resume it — a call_in_kernel_context or "
                  "exit-path function BLOCKED on a borrowed stack; the call chain in this thread's "
                  "frames names the door\n",
                  thread->threadID, bs_task ? bs_task->exename : "?", scratch_owner,
                  thread->regs.RSP, (uint32_t)apic_id);
        }
    }
    // ── TF TRIPWIRE ────────────────────────────────────────────────────────
    // RFLAGS.TF (bit 8) makes the CPU single-step: it executes ONE
    // instruction after the iretq and raises #DB. Nothing in this kernel
    // ever wants that — createThread sets 0x202 and no code path sets TF —
    // so if it is set here, something corrupted a saved register frame.
    //
    // Catching it HERE, as the frame is loaded, is the whole point: by the
    // time the CPU acts on it the evidence is gone and the report blames
    // whatever instruction happened to be next (a #GP in task_idle_loop,
    // 2026-08-01, which had done nothing wrong). This says WHOSE flags,
    // on which core, with the actual value.
    //
    // We CLEAR it rather than letting it fire. A surviving core with a
    // loud report is strictly more debuggable than a core parked forever
    // in the exception panic's cli/hlt — and a parked core silently eats
    // every thread the scheduler later hands it (that is what made `top`
    // start, exit cleanly, and never draw a single character).
    if (thread->regs.RFLAGS & 0x100)
    {
        static volatile bool tf_reported_on_screen = false;
        task_t *tf_task = (task_t *)thread->ownerTask;
        printd(DEBUG_BOOT, "TF TRIPWIRE: thread 0x%08x (%s) on AP %u had RFLAGS=0x%016lx "
               "(TF set) — cleared before dispatch\n",
               thread->threadID, tf_task ? tf_task->path : "?", (uint32_t)apic_id,
               thread->regs.RFLAGS);
        if (!tf_reported_on_screen)
        {
            tf_reported_on_screen = true;   // once per boot: the glass is not a log
            printf("TF TRIPWIRE: thread 0x%08x (%s) on AP %u: RFLAGS=0x%016lx, TF cleared\n",
                   thread->threadID, tf_task ? tf_task->path : "?", (uint32_t)apic_id,
                   thread->regs.RFLAGS);
        }
        thread->regs.RFLAGS &= ~0x100UL;
    }
    mp_isrSavedRFlags[apic_id]=thread->regs.RFLAGS;
    mp_isrSavedES[apic_id]=thread->regs.ES;
    mp_isrSavedFS[apic_id]=thread->regs.FS;
    mp_isrSavedGS[apic_id]=thread->regs.GS;
    mp_isrSavedCR3[apic_id]=thread->regs.CR3;
    
    printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"scheduler_load_thread: Loading SYSENTER_ESP_MSR with value 0x%08x\n",thread->regs.RSP0);

	// Update the per-core TSS so SYSCALL transitions land on the correct kernel stack
	if (cls->tss)
	{
		tss_set_rsp0(cls->apic_id, thread->regs.RSP0);
	}

	//TODO: Handle forked threads
    if (((task_t*)cls->currentThread->ownerTask)->justForked)
    {
		panic("scheduler_load_thread: Finish the fork register load\n");
        printd(DEBUG_SCHEDULER,"loadISRSavedRegs: Fork return for newly spawned child thread\n");
        mp_isrSavedCS[apic_id] = thread->regs.CS;
        mp_isrSavedRIP[apic_id] = thread->regs.RIP;
        mp_isrSavedSS[apic_id] = thread->regs.SS;
        mp_isrSavedDS[apic_id] = thread->regs.DS;
        mp_isrSavedRAX[apic_id] = thread->regs.RAX;
        mp_isrSavedRBX[apic_id] = thread->regs.RBX;
        mp_isrSavedRCX[apic_id] = thread->regs.RCX;
        mp_isrSavedRDX[apic_id] = thread->regs.RDX;
        mp_isrSavedES[apic_id] = thread->regs.RSI;
        mp_isrSavedRDI[apic_id] = thread->regs.RDI;
        mp_isrSavedRSP[apic_id] = forkedThread->regs.RSP;
        mp_isrSavedRBP[apic_id] = forkedThread->regs.RBP;
        //Removed line of code that was setting the EBP directly to the parent's.  The above code is correct for assigning the EBP after a fork
        mp_isrSavedRFlags[apic_id] = thread->regs.RFLAGS;
        mp_isrSavedES[apic_id] = thread->regs.ES;
        mp_isrSavedFS[apic_id] = thread->regs.FS;
        mp_isrSavedGS[apic_id] = thread->regs.GS;
        //We need to load the CR3 because whatever CR3 the parent was using, that's what the child should use for the FIRST return from syscall.
        mp_isrSavedCR3[apic_id] = thread->regs.CR3; 
//        memcpy((uintptr_t*)((process_t*)task->process)->stackStart, (uintptr_t*)parent->stackStart, ((process_t*)task->process)->stackSize);
    }
	debug_print_registers(apic_id, "load");
}

void scheduler_add_to_queue(thread_t *queue, thread_t* thread)
{
    printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*\t\taddToQ: Adding thread 0x%04x to queue %s\n",thread->threadID,THREAD_STATE_NAMES[thread->threadState]);
    while (queue->next!=NO_NEXT)
    {
        queue++;
    }
	queue->next = thread;
	thread->prev = queue;
    panic("Can't find queue entry to add task to!");
}

void scheduler_remove_from_queue(thread_t *queue, thread_t* thread, bool panicOnNotFound)
{
	printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*\t\tremoveFromQ: Removing thread 0x%08x (0x%16lx) from queue %s\n",
			thread->threadID,
			thread,
			THREAD_STATE_NAMES[thread->threadState]);
    while (queue!=NO_NEXT)
    {
        if (queue->threadID==thread->threadID)
        {
            if (queue->next != NO_NEXT)
			{
				if (queue->prev != NO_PREV)
					((thread_t*)queue->prev)->next = queue->next;
				else
					((thread_t*)queue->prev)->next = NO_NEXT;
			}
			else
				if (queue->prev != NO_PREV)
					((thread_t*)queue->prev)->next = NO_NEXT;
            return;
        }
		queue=queue->next;
    }

    if (panicOnNotFound)
        panic("scheduler_remove_from_queue: Can't find queue entry to remove!");
}

void scheduler_wake_isleep_task(task_t *task) {
    if (task == NULL || task->threads == NULL) return; // Ensure task is valid

    // Check-and-relink atomically (this runs in thread context — task_exit
    // waking a parent — which the fan-out can place on any core). The
    // trigger stays OUTSIDE the lock: it hlt-waits for a scheduler pass that
    // needs this very lock, so triggering while holding it is a guaranteed
    // self-deadlock.
    // Delegate to the helper that already gets this exactly right (above):
    // ISLEEP test + SIGSLEEP cancel + relink as ONE act under the lock. The
    // cancel MUST live in here rather than in the caller, and it must be
    // conditional on the thread having actually parked — cancelling a backstop
    // for a thread that is still mid-park destroys the only retry the waiter
    // has, and it lands in ISLEEP with no flag and no deadline: asleep for
    // good. task_enqueue_dead_child used to do exactly that (see its comment,
    // 2026-08-09). Leaving a not-yet-parked thread untouched is the whole
    // trick: its own SIGSLEEP deadline fires a tick later and it re-checks.
    uint64_t flags = scheduler_queues_lock();
    scheduler_wake_isleep_thread_locked(task->threads);
    task->threads->prioritizedTicksInRunnable += HIGH_PRIORITY_TICKS_BOOST;
    scheduler_queues_unlock(flags);
    scheduler_trigger(NULL);
}

thread_t *scheduler_find_thread_to_run(core_local_storage_t *cls, bool justBrowsing)
{
    uint32_t mostIdleTicks=0, oldTicks;
    task_t *task;
    thread_t *thread, *threadToRun = NO_THREAD;
    thread_t *queue=qRunnable;
    // The best thread the cache-home veto skipped this pass (and its aging),
    // kept as a fallback. THE RULE (2026-08-14, born from Chris's P5 burn-in):
    // cache-home is a preference among cores that HAVE work — idling is not
    // preferable to anything. If the veto-respecting winner below turns out
    // to be the idle thread, we take this one instead. Without it the veto
    // is a livelock: a preempted thread's requeue RESETS runnableSinceTick
    // and fires the idle-core nudge in the same breath, so the nudged core
    // always inspects a zero-ticks-warm thread, declines, and sleeps through
    // the whole migratable window (its timer is parked — nobody re-checks).
    // hog -n 4 on 4 cores ran a full night at 75%: two threads trading one
    // core at 50% each while a core idled six inches away.
    thread_t *warmSkipped = NO_THREAD;
    uint32_t warmSkippedTicks = 0;
    
    int queEntryNum = 0;
    while (queue!=NO_NEXT)
    {
		thread = queue;
		task = (task_t*)thread->ownerTask;
		oldTicks=thread->prioritizedTicksInRunnable;
		//This is where we increment all the runnable ticks, based on the process' priority
        if (!thread->idleThread && !justBrowsing)
            thread->prioritizedTicksInRunnable+=(RUNNABLE_TICKS_INTERVAL-task->priority)+1;
			if (!justBrowsing)
                printd(DEBUG_SCHEDULER | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED, "*\t%u-Thr 0x%08x (tsk 0x%08x-%s), pri=%i, oldt=%u, newt=%u (runt=%u)\n",
                       queEntryNum,
                       thread->threadID,
                       task->taskID,
                       task->exename,
                       task->priority,
                       oldTicks,
                       thread->prioritizedTicksInRunnable,
                       thread->totalRunTicks);
            if (thread->prioritizedTicksInRunnable >= mostIdleTicks)
			{
				if (scheduler_thread_can_run_on_core(thread, cls))
				{
					// ── The cache-home rule (2026-08-13, thread.h doctrine) ──
					// A thread that last ran on ANOTHER core is cache-warm
					// there and cache-cold here: pass it over until it has
					// waited SCHED_MIGRATION_COST_TICKS of wall clock, the
					// point where waiting longer costs more than migrating.
					// Exempt: idle threads (per-core by construction), never-
					// dispatched threads (runCycles==0 — cold everywhere, so
					// migration is free), and of course this core's own. A
					// pinned thread is unaffected: can_run_on_core already
					// confines it to its pin, where it is always home. And
					// the veto is a PREFERENCE, not a refusal: every thread
					// it skips is remembered as warmSkipped, and if this
					// pass would otherwise hand the core its idle thread,
					// the fallback below wins — an idle core takes warm
					// work over doing nothing (the doctrine at warmSkipped's
					// declaration; the old claim here that "any foreign core
					// may take it after the threshold" was true only if a
					// foreign core happened to LOOK during the window, which
					// a parked idle core never did).
					if (!thread->idleThread && thread->runCycles != 0 &&
					    thread->lastRunApicID != cls->apic_id &&
					    (kTicksSinceStart - thread->runnableSinceTick) < SCHED_MIGRATION_COST_TICKS)
					{
						printd(DEBUG_SCHEDULER | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,
						       "*\t\tfindTaskToRun: thread 0x%08x is cache-warm on APIC %u, not migrating yet\n",
						       thread->threadID, thread->lastRunApicID);
						if (thread->prioritizedTicksInRunnable >= warmSkippedTicks)
						{
							warmSkipped = thread;
							warmSkippedTicks = thread->prioritizedTicksInRunnable;
						}
						queEntryNum++;
						queue=queue->next;
						continue;
					}
					if (thread->mp_apic != THREAD_NO_AFFINITY && !thread->idleThread)
						printd(DEBUG_SCHEDULER | DEBUG_DETAILED,
							"scheduler_find_thread_to_run: APIC %u selecting pinned thread 0x%08x for APIC %u\n",
							cls->apic_id,
							thread->threadID,
							thread->mp_apic);
					if (thread->idleThread)
						printd(DEBUG_SCHEDULER | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"*\t\tfindTaskToRun: Found idle thread for APIC %u\n",cls->apic_id);
					threadToRun=thread;
					mostIdleTicks=thread->prioritizedTicksInRunnable;
				}
			}
        queEntryNum++;
        queue=queue->next;
    }
	kDiagRunnableLen = queEntryNum;                    // TEMP DIAG

	// The idle-core exemption: if honoring the cache-home veto left this core
	// with only its idle thread while real work sat skipped-as-warm, take the
	// work. warmSkipped already passed scheduler_thread_can_run_on_core (the
	// veto check is nested inside that gate), so affinity is respected.
	// Busy cores are untouched — they only reach here with a real tenant
	// selected, so the veto still prevents needless migration between them.
	if (warmSkipped != NO_THREAD &&
	    (threadToRun == NO_THREAD || threadToRun->idleThread))
	{
		printd(DEBUG_SCHEDULER | DEBUG_DETAILED,
		       "scheduler_find_thread_to_run: APIC %u idle — taking cache-warm thread 0x%08x from APIC %u rather than idling\n",
		       cls->apic_id, warmSkipped->threadID, warmSkipped->lastRunApicID);
		threadToRun = warmSkipped;
	}

	if (threadToRun == NO_THREAD && !justBrowsing)
		panic("scheduler_find_thread_to_run: No runnable threads found\n");
	if (!justBrowsing)
		printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "Found new thread 0x%08x to run\n", threadToRun->threadID);
	return threadToRun;
}

//NOTE: scheduler_trigger issues a STI so it can break things if you want interrupts to be disabled!
void scheduler_trigger(core_local_storage_t *cls)
{
	if (!cls)
		cls = get_core_local_storage();
	//If we got here, something in the scheduler called to trigger the scheduler.  Illogical ... find it and fix it!
	if (mp_inScheduler[cls->apic_id])
    {
        printd(DEBUG_SCHEDULER,"scheduler_trigger: ERROR: Called but already in scheduler, exiting!\n");
        return;
    }
//    printd(DEBUG_SCHEDULER,"scheduler_trigger: triggering scheduler\n");
    kDiagTriggerCalls++;                                // TEMP DIAG
    mp_waitingForScheduler[cls->apic_id] = true;
    mp_schedulerEnabled[cls->apic_id] = true;

	//Since we're calling a different vector than the APIC timer does, we need to reset the timer count
	// — but NOT on an AP whose timer is the backstop lease: that count is
	// the periodic 10ms value, and writing it to an UNMASKED one-shot LVT
	// would arm a rogue 10ms lease. The pass this trigger provokes re-grants
	// the real lease at its dispatch tail; nothing needs pre-arming here.
	// (Backstop off = LVT still masked = the write stays the harmless phase
	// reset it always was, so old tickless behavior is preserved exactly.)
	if (!(kTicklessScheduler && kSchedBackstopEnabled &&
	      cls->apic_id != BOOTSTRAP_PROCESSOR_ID))
		mp_restart_apic_timer_count();
    send_ipi(cls->apic_id, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
    // Wait until the ISR has run and cleared mp_waitingForScheduler.
    // Using a checked loop rather than a bare sti;hlt because the ISR may
    // fire before we reach hlt (e.g. during send_ipi's printd). If that
    // happens the thread gets context-switched away mid-function; when it
    // is later rescheduled it resumes here, the flag is already cleared,
    // and the loop exits without blocking.
    __asm__ volatile("sti");
    // Re-fetch CLS on EVERY check, not just after the loop: the scheduler
    // pass this triggers switches us out, and since the fan-out we may be
    // rescheduled on a DIFFERENT core. A stale cls here meant a migrated
    // thread hlt-waited on its OLD core's flag — which can legitimately be
    // set (that core mid-trigger for its own tenant), stranding us in hlt on
    // a core whose timer may be masked (tickless). The core that resumed us
    // cleared its own flag at scheduler_do entry, so the fresh read exits
    // immediately.
    while (mp_waitingForScheduler[get_core_local_storage()->apic_id])
        __asm__ volatile("hlt");
    cls = get_core_local_storage();
}

// scheduler_yield() used to live here.  It was retired in favor of
// scheduler_trigger() (above) once syscalls started yielding: it entered the
// scheduler via a direct software `int`, which never sets the APIC in-service
// bit — so unlike every hardware entry it had NO nesting protection (a
// pending timer could re-enter _schedule_ap mid-prologue), and its
// peek-the-queue-then-fire logic raced the world changing in between.
// scheduler_trigger's genuine self-IPI gives every scheduler entry — timer or
// manual — identical interrupt semantics.

// Terminate push delivery, v1 — Chris's os32 forced-syscall trick ("I *forced*
// the task to make a syscall") wearing a 64-bit seatbelt. If the thread this
// core is about to resume has a TERMINATING signal pending (Ctrl+C, or a write
// to its /proc ctl file), no handler installed, and was
// interrupted IN RING 3 (holding no kernel locks — the seatbelt), point its
// resume RIP at the task's exit trampoline (TASK_EXIT_TRAMPOLINE_VIRT, mapped
// read-only into every ring-3 task). The victim resumes, immediately executes
// `syscall`, and the dispatcher's SIGINT check does the honors — full
// task_exit in the victim's own context, handles closed safely, retVal 130.
// This closes the one gap in the pull design: a syscall-free spin loop.
// Threads interrupted mid-syscall (CS ring 0) are left alone — they die at
// the syscall boundary instead (console/pipe sentinels + dispatcher check).
// When userland signal delivery lands, the sighandler check below grows the
// second branch exactly as os32 had it: handler installed -> redirect to the
// handler instead of the gallows.
static void scheduler_sigint_forced_syscall(thread_t *thread, uint64_t apic_id)
{
	task_t *task = (task_t *)thread->ownerTask;

	if (!(sigset_any(thread->signals.sigind, SIGNALS_TERMINATING)))
		return;
	// `exiting` too (2026-08-09): a thread already inside task_exit_finish has
	// not set `exited` yet — that is now published only at the very end — so
	// testing `exited` alone would redirect a dying thread down the exit
	// trampoline a SECOND time, mid-teardown. See thread.h for the split.
	if (thread->exited || thread->exiting || thread->idleThread)
		return;
	if (task == NULL || task->kernelTask)
		return;
	// A SIGKILL is uncatchable by definition: an installed handler only earns
	// a reprieve from the catchable half. This is the one place the two
	// terminating signals genuinely differ today, and encoding it now means
	// userland signal delivery inherits the right semantics instead of
	// retrofitting them.
	// The handler table is the TASK's now, not the thread's (2026-08-23,
	// SIGNALS.md §2): the aim is already a broadcast, so per-thread handlers
	// would run one signal once per thread. `task` is non-NULL above.
	if (!(sigset_has(thread->signals.sigind, SIGKILL)) &&
	    task->sighandler[SIGINT] != NULL)
		return;                          // future: deliver, don't kill
	if ((thread->regs.CS & 3) != 3)
		return;                          // mid-syscall: the pull path owns it

	// Patch BOTH images of the frame: regs (authoritative store) and the
	// per-core isr array (what the ISR exit path actually IRETs from when the
	// same thread continues without a reload). Idempotent on repeat passes.
	thread->regs.RIP = TASK_EXIT_TRAMPOLINE_VIRT;
	mp_isrSavedRIP[apic_id] = TASK_EXIT_TRAMPOLINE_VIRT;

	printd(DEBUG_SCHEDULER, "*%s: forcing thread 0x%08x (%s) into the exit trampoline\n",
	       (sigset_has(thread->signals.sigind, SIGKILL)) ? "SIGKILL" : "SIGINT",
	       thread->threadID, task->exename);
}

void scheduler_run_new_thread()
{
	core_local_storage_t *cls = get_core_local_storage();
	uint64_t apic_id = cls->apic_id;
	thread_t* threadToStop=NULL;
    eThreadState threadToStopNewQueue=0;

    printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*AP%lu: In runAnotherTask, mp_CoreHasRunScheduledThread=%s!\n",apic_id,mp_CoreHasRunScheduledThread[apic_id]?"true":"false");

    if (apic_id != 0 && !mp_CoreHasRunScheduledThread[apic_id])
    {
        printd(DEBUG_SCHEDULER,"*AP%u: No threads have been scheduled on this core yet, no thread to stop!\n",apic_id);
    }
	else
	{
		threadToStop=scheduler_get_running_thread(cls->threadID);

		task_t *taskToStop = (task_t*)threadToStop->ownerTask;
        printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "*Found thread 0x%08x to take off CPU @0x%04x:0x%08x (exited=%u, retval=0x%08x).\n",
               taskToStop->taskID,
               mp_isrSavedCS[apic_id], mp_isrSavedRIP[apic_id],
               threadToStop->exited,
               threadToStop->retVal);

        if (threadToStop->exited)
		{
			printd(DEBUG_SCHEDULER,"*Thread (0x%08x) ended, moving it to the zombie queue.\n",threadToStop->threadID);

			threadToStopNewQueue=THREAD_STATE_ZOMBIE;
			//TODO: If this is the last thread for the task then do something with the task, INCLUDING resetting its GDT entry
		}
        else if (sigset_has(threadToStop->signals.sigind, SIGSLEEP))
			threadToStopNewQueue=THREAD_STATE_ISLEEP;
		else
            threadToStopNewQueue=THREAD_STATE_RUNNABLE;
        scheduler_store_thread(cls, threadToStop);              //we're taking it off the cpu so save the registers
        scheduler_change_thread_queue_locked(threadToStop, threadToStopNewQueue);   //scheduler_do holds the queue lock
	}
	printd(DEBUG_SCHEDULER | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"*Finding thread to run\n");
    thread_t* threadToRun=scheduler_find_thread_to_run(cls, false);
	task_t* taskToRun = (task_t*)threadToRun->ownerTask;
	
    if (threadToStop && threadToRun->threadID==threadToStop->threadID)
    {
        printd(DEBUG_SCHEDULER,"*No new thread to run, continuing with the current task\n");
		debug_print_registers(apic_id, "continue2");
        // Continue path resumes from the isr arrays WITHOUT a reload, so the
        // forced-syscall check must run here too (regs were just stored above,
        // so regs.CS is fresh for the ring-3 seatbelt).
        scheduler_sigint_forced_syscall(threadToStop, apic_id);
        if (threadToStop->execDontSaveRegisters)
        {
            printd(DEBUG_SCHEDULER,"Thread to keep running was just exec'd, loading registers from tss\n");
            //TODO: Should be able to get rid of the load statement
			scheduler_load_thread(cls, threadToStop);
            threadToStop->execDontSaveRegisters = false;
        }
        scheduler_change_thread_queue_locked(threadToStop,THREAD_STATE_RUNNING);   //switch it back to the running queue (queue lock held)
    }
	else
	{
        printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "*Found thread to move to CPU (%x - %s)\n", threadToRun->threadID, taskToRun->exename);
        scheduler_change_thread_queue_locked(threadToRun, THREAD_STATE_RUNNING);    //queue lock held by scheduler_do
        scheduler_load_thread(cls, threadToRun);
        // The switch path: load just synced regs -> isr arrays, so the
        // redirect (if owed) patches both images consistently.
        scheduler_sigint_forced_syscall(threadToRun, apic_id);
		task_t *pTask = (task_t*)threadToRun->ownerTask;
        // exename is a bare basename ("idle0", "idle1", ...) — no leading slash.
        if (strncmp(pTask->exename, "idle", 4) != 0)
        {
 /*           activeSTDIN = pTask->stdin;
            activeSTDIN->owner = pTask;
            activeSTDOUT = pTask->stdout;
            activeSTDOUT->owner = pTask;
            activeSTDERR = pTask->stderr;
            activeSTDERR->owner = pTask;
            activeTTY->stdInReadPipe->owner = activeTTY->stdInWritePipe->owner = 
                activeTTY->stdErrReadPipe->owner = activeTTY->stdErrWritePipe->owner = 
                activeTTY->stdOutReadPipe->owner = activeTTY->stdOutWritePipe->owner = taskToRun->process;
 */           //Keep track of the context switch count
            pTask->cSwitches++;
        }
        //printd(DEBUG_SCHEDULER,"Active STDIN/STDOUT/STDERR=0x%08x/0x%08x/0x%08x, owner %s\n",activeSTDIN, activeSTDOUT, activeSTDERR, (process_t*)(activeSTDIN->owner)->exename==NULL?"":(process_t*)(activeSTDIN->owner)->exename);
        // The task-switch trace: which thread, from which PROGRAM, at which RIP.
        // Its own level (DEBUG_TASKSWITCH, always on in MINIMAL) so you can watch
        // husk -> hello -> husk without enabling the DEBUG_SCHEDULER firehose, and
        // so the level names the message honestly. Runtime-gated, not #if-gated:
        // switch it off from the cmdline, no rebuild — same move as the register
        // dump above.
        printd(DEBUG_TASKSWITCH, "*Restarting CPU with new thread (0x%04x - %s) @ 0x%04x:0x%08x\n",
            threadToRun->threadID,
            ((task_t*)(threadToRun->ownerTask))->exename,
            threadToRun->regs.CS,
            threadToRun->regs.RIP);
        if (threadToStop && threadToStop != NO_THREAD)
		{
			task_t* taskToStop = (task_t*)threadToStop->ownerTask;
			printd(DEBUG_SCHEDULER,"*Total running ticks: 0x%04x: %u, 0x%04x: %u\n",
				taskToStop->taskID,
				threadToStop->totalRunTicks,
				taskToRun->taskID,
				threadToRun->totalRunTicks);
		}
		else
		{
			printd(DEBUG_SCHEDULER,"*Total running ticks: ----: ----, 0x%04x: %u\n",
				taskToRun->taskID,
				threadToRun->totalRunTicks);
		}
		mp_schedulerTaskSwitched[apic_id]=true;
		kTaskSwitchCount++;
		mp_ForkReturn[apic_id] = false;
		//TODO: Update the GDT to mark the task as not busy
        if (taskToRun->justForked)
        {
            mp_ForkReturn[apic_id] = mp_isrSavedRSP[apic_id];
            taskToRun->justForked = 0;
        }
        //Update the core local storage task
        // (Kept, and now redundant: scheduler_load_thread sets this beside
        // cls->currentThread so the pair cannot diverge — see the note there
        // for the program it cost. Left in place because it is harmless and
        // because deleting it would hide that this assignment ever lived
        // here, which is the whole story.)
        cls->task = taskToRun;
	} //New thread loaded
}

//NOTE: When this method is entered, it is time to reschedule.
void scheduler_do() 
{
	core_local_storage_t *cls = get_core_local_storage();
	uint8_t apic_id = cls->apic_id;
    mp_waitingForScheduler[apic_id] = false;

	// ── CPU-time accounting: the outgoing thread's slice ends HERE ──────────
	// Charged at the switch boundary, not tick-sampled, so sub-tick slices
	// are visible and nothing gets laundered into whoever the timer caught.
	// Both rdtsc reads in every delta happen on THIS core (this function runs
	// on the core being scheduled, even under tickless — the nudger only decides
	// WHEN, the IPI makes each core run its own pass), so TSC desync between
	// cores can never corrupt a delta. The ISR time between interrupt entry
	// and this line rides on the outgoing thread — documented v1 honesty,
	// fixable with entry stamps if it ever matters.
	uint64_t acctPassStart = rdtsc();
	if (cls->acctLastDispatchTSC != 0 &&
	    cls->currentThread != NULL && cls->currentThread != NO_THREAD)
	{
		// Halted current = idle wearing a task's name; the span goes to
		// this core's idle thread (smp.h acctCurrentHalted — the fix for
		// top billing the compositor 95% of a core for sleeping).
		thread_t *acctTarget = (cls->acctCurrentHalted && cls->acctIdleThread != NULL)
		                           ? cls->acctIdleThread
		                           : cls->currentThread;
		acctTarget->runCycles += acctPassStart - cls->acctLastDispatchTSC;
	}
	if (cls->acctZeroTSC == 0)
		cls->acctZeroTSC = acctPassStart;   // this core's meter starts now

	// The BSP's pass also tends the cycles→µs exchange rate (x86_64.c has
	// the doctrine: boot calibration is ±1% by construction; this converges
	// it). BSP only — the recalibrator's TSC samples must all come from one
	// core, and core 0 exists in every configuration.
	if (apic_id == 0)
		tsc_recalibrate();

    printd(DEBUG_SCHEDULER,"***** SCHEDULER *****\n");
    printd(DEBUG_SCHEDULER,"scheduler: AP %u\n",apic_id);
#if SCHEDULER_DEBUG == 1
    uint64_t ticksBefore = rdtsc();
#endif
	kDiagPassCount[apic_id]++;                          // TEMP DIAG
	// TEMP DIAG: name the interrupt that drove THIS pass, and this core's
	// LVT timer state, from this core's own LAPIC (core-local, safe here).
	kDiagLastVector[apic_id] = apic_in_service_vector();
	kDiagLVT[apic_id] = read_apic_register(kMPApicBase + 0x320);

	// A timer-vector pass on a tickless AP is a lease expiry — the backstop
	// preempting whoever ran past SCHED_BACKSTOP_MS without re-entering the
	// scheduler. Counted for the test suite and for anyone diagnosing "who
	// is interrupting my pinned core" (the answer is: its own lease).
	if (kTicklessScheduler && apic_id != BOOTSTRAP_PROCESSOR_ID &&
	    kDiagLastVector[apic_id] == IPI_TIMER_SCHEDULE_VECTOR)
		kSchedBackstopFires[apic_id]++;

	// ── TEMP DIAG: the once-per-second report, BSP pass only ────────────
	// ticks vs TSC-milliseconds answers "is the tick clock starving?";
	// the rest answers "who is spending the time?". Values are deltas
	// since the previous line except the max* fields, which reset here.
	if (apic_id == 0)
	{
		static uint64_t diagLastTick = 0, diagLastTSC = 0;
		static uint64_t dNudgeU = 0, dNudgeP = 0, dTrig = 0, dIPI = 0, dBcast = 0, dTO = 0;
		static uint64_t dPass[4] = {0};
		if (diagLastTSC == 0) { diagLastTick = kTicksSinceStart; diagLastTSC = acctPassStart; }
		if (kTicksSinceStart - diagLastTick >= TICKS_PER_SECOND)
		{
			uint64_t tscMS = ((acctPassStart - diagLastTSC) * 1000UL) / kCPUCyclesPerSecond;
            printd(DEBUG_DIAG,
                   "DIAG: ticks +%lu tsc +%lums | pass 0:%lu 1:%lu 2:%lu 3:%lu runq %lu | nudge u%lu p%lu trig %lu | ipi %lu icrmax %lu lockmax %lu | settle %lu to %lu (ap %lu) setmax %lu\n",
                   kTicksSinceStart - diagLastTick, tscMS,
                   kDiagPassCount[0] - dPass[0],
                   (kMPCoreCount > 1) ? kDiagPassCount[kCPUInfo[1].apicID] - dPass[1] : 0,
                   (kMPCoreCount > 2) ? kDiagPassCount[kCPUInfo[2].apicID] - dPass[2] : 0,
                   (kMPCoreCount > 3) ? kDiagPassCount[kCPUInfo[3].apicID] - dPass[3] : 0,
                   kDiagRunnableLen,
                   kDiagNudgeUnpinned - dNudgeU, kDiagNudgePinned - dNudgeP,
                   kDiagTriggerCalls - dTrig,
                   kDiagIPISends - dIPI, kDiagICRMaxSpins, kDiagLockMaxSpins,
                   kDiagSettleBroadcasts - dBcast, kDiagSettleTimeouts - dTO,
                   kDiagSettleLastLateAPIC, kDiagSettleMaxSpins);
            printd(DEBUG_DIAG,
                   "DIAG2: vec/lvt 0:0x%02x/0x%05x 1:0x%02x/0x%05x 2:0x%02x/0x%05x 3:0x%02x/0x%05x\n",
                   kDiagLastVector[0], kDiagLVT[0],
                   (kMPCoreCount > 1) ? kDiagLastVector[kCPUInfo[1].apicID] : 0,
                   (kMPCoreCount > 1) ? kDiagLVT[kCPUInfo[1].apicID] : 0,
                   (kMPCoreCount > 2) ? kDiagLastVector[kCPUInfo[2].apicID] : 0,
                   (kMPCoreCount > 2) ? kDiagLVT[kCPUInfo[2].apicID] : 0,
                   (kMPCoreCount > 3) ? kDiagLastVector[kCPUInfo[3].apicID] : 0,
                   (kMPCoreCount > 3) ? kDiagLVT[kCPUInfo[3].apicID] : 0);
            diagLastTick = kTicksSinceStart; diagLastTSC = acctPassStart;
			dNudgeU = kDiagNudgeUnpinned; dNudgeP = kDiagNudgePinned;
			dTrig = kDiagTriggerCalls; dIPI = kDiagIPISends;
			dBcast = kDiagSettleBroadcasts; dTO = kDiagSettleTimeouts;
			dPass[0] = kDiagPassCount[0];
			if (kMPCoreCount > 1) dPass[1] = kDiagPassCount[kCPUInfo[1].apicID];
			if (kMPCoreCount > 2) dPass[2] = kDiagPassCount[kCPUInfo[2].apicID];
			if (kMPCoreCount > 3) dPass[3] = kDiagPassCount[kCPUInfo[3].apicID];
			kDiagICRMaxSpins = 0; kDiagLockMaxSpins = 0; kDiagSettleMaxSpins = 0;
		}
	}

	//Lock the section of code from the time we start looking for another thread to run, until we're done
	//either switching threads, or have identified that there's no new thread to run
	{
		uint64_t lockSpins = 0;                        // TEMP DIAG
		while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1))
		{
			lockSpins++;                               // TEMP DIAG
			__builtin_ia32_pause();
		}
		if (lockSpins > kDiagLockMaxSpins)             // TEMP DIAG
			kDiagLockMaxSpins = lockSpins;             // TEMP DIAG
	}
    thread_t* threadToRun=scheduler_find_thread_to_run(cls, true);
  	if (threadToRun != NO_THREAD && threadToRun->threadID!=cls->threadID)
    {
		printd(DEBUG_SCHEDULER, "Time to make the donuts. (switch threads)\n");
		scheduler_run_new_thread();
        printd(DEBUG_SPECIAL, "SCHEDULER: Now running %s on core %u\n", ((task_t *)(cls->task))->path, cls->apic_id);
    }
    else
	{
		debug_print_registers(apic_id, "continue");
//        printd(DEBUG_SCHEDULER,"*Shortcut! No new thread to run, continuing with 0x%016lx-%s\n", cls->currentThread->threadID, ((task_t*)cls->currentThread->ownerTask)->exename);
	}
	__sync_lock_release(&kSchedulerSwitchTasksLock);   
    kSchedulerCallCount++;
#if SCHEDULER_DEBUG == 1
    uint64_t ticksAfter = rdtsc();
#endif
    mp_CoreHasRunScheduledThread[apic_id] = true;

#if SCHEDULER_DEBUG == 1
    printd(DEBUG_SCHEDULER, "*Scheduler: calls=%u, task switchs=%u, ticks since start=0x%08x\n", kSchedulerCallCount, kTaskSwitchCount, kTicksSinceStart);
    uint64_t diff = ticksAfter-ticksBefore;
    uint64_t timeInScheduler = (diff/kCPUCyclesPerSecond)*100;
    printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "%lu ticks expired (%lu CPU cycles)\n", timeInScheduler, diff);
#endif
    printd(DEBUG_SCHEDULER, "*********************\n");

    // ── CPU-time accounting: the incoming thread's slice starts HERE ────────
	// Covers both paths (switch and shortcut — a continued thread is still
	// dispatched). Everything between acctPassStart and now was the
	// scheduler's own time: it goes to this core's system bucket, which is
	// what lets top show ghost-churn as "system: climbing" instead of
	// laundering it into the innocent bystanders the timer interrupted.
	uint64_t acctPassEnd = rdtsc();
	cls->acctSchedCycles += acctPassEnd - acctPassStart;
	cls->acctLastDispatchTSC = acctPassEnd;
	// A dispatched thread is by definition not halted: every dispatch —
	// switch or shortcut — drops the halted flag. The compositor re-raises
	// it per nap; nobody else ever sets it.
	cls->acctCurrentHalted = false;

	// ── The preemption lease (smp_core.c doctrine block) ────────────────────
	// EVERY dispatch door funnels through this tail — switch, shortcut,
	// nudge-driven, lease-expiry-driven — so this is the one place the lease
	// is granted or revoked. Non-idle dispatch on a tickless AP: arm the
	// one-shot deadline; idle dispatch: stop the timer, return the core to
	// true silence. The BSP keeps its 100Hz heartbeat (timekeeping lives
	// there); periodic mode never reaches here with a masked timer at all.
	// IF may be 1 here (the pass runs interruptible by design — see
	// SCHEDULER_REENTRANCY.md); that is safe: the LAPIC writes are each
	// atomic, an interrupt between them at worst delays the arming count
	// write (which is deliberately last), and a nested pass is barred by
	// mp_inScheduler regardless.
	if (kTicklessScheduler && kSchedBackstopEnabled &&
	    apic_id != BOOTSTRAP_PROCESSOR_ID)
	{
		thread_t *dispatched = cls->currentThread;
		if (dispatched != NULL && dispatched != NO_THREAD && !dispatched->idleThread)
			sched_backstop_arm(cls);
		else
			sched_backstop_disarm();
	}
}
