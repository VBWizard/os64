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

#define VERIFY_QUEUE(q) if (q<0 || (q>THREAD_STATE_ISLEEP && q!=THREAD_STATE_ZOMBIE)) panic("VERIFY_QUEUE: Invalid state %u\n", q)

const char* THREAD_STATE_NAMES[] = {"None","Running","Runnable","Stopped","Uninterruptable Sleep","Interruptable Sleep","Exited","Zombie"};

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

void scheduler_change_thread_queue(thread_t* thread, eThreadState newState)
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
        thread->prioritizedTicksInRunnable=0;
    else if (newState==THREAD_STATE_RUNNING)
        thread->lastRunStartTicks=kTicksSinceStart;
}

void scheduler_submit_new_task(task_t *newTask)
{
	task_t* slot=scheduler_find_open_next_task_slot();
	if (slot==NO_TASK)
	{
		slot = newTask;
		kTaskList = newTask;
		slot->next=NO_TASK;
		newTask->prev=NO_TASK;
	}
	else
	{
		slot->next=newTask;
		newTask->prev=slot;
		newTask->next=NO_TASK;
	}

	if (newTask->threads==NULL)
		panic("scheduler_submit_new_task: Task does not have a thread assigned\n");

	scheduler_change_thread_queue(newTask->threads, THREAD_STATE_RUNNABLE);
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

void debug_print_registers(uint64_t apic_id, char* prefix, bool unconditional)
{
	__uint128_t savedDebugFlags;
	if (unconditional)
	{
		savedDebugFlags = kDebugLevel;
		savedDebugFlags |= DEBUG_SCHEDULER;
	}

    printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*\t%s: CR3=0x%016lx, CS=0x%04X, RIP=0x%016lx, SS=0x%04X, DS=0x%04X, RAX=0x%016lx, RBX=0x%016lx, RCX=0x%016lx, RDX=0x%016lx, RSI=0x%016lx, RDI=0x%016lx, RSP=0x%016lx, RBP=0x%016lx, FLAGS=0x%016lx\n",
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
	if (unconditional)
	{
		kDebugLevel = savedDebugFlags;
	}
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
        thread->regs.RFLAGS=mp_isrSavedRFlags[apic_id];
        thread->regs.ES=mp_isrSavedES[apic_id];
        thread->regs.FS=mp_isrSavedFS[apic_id];
        thread->regs.GS=mp_isrSavedGS[apic_id];
        thread->regs.CR3=mp_isrSavedCR3[apic_id];
    }
#if SCHEDULER_DEBUG == 1
	debug_print_registers(apic_id, "save (or not)", false);
#endif
}

void scheduler_load_thread(core_local_storage_t *cls, thread_t* thread)
{
	//task_t* task = cls->currentThread->ownerTask;
	//task_t* ownerTask = ((task_t*)cls->currentThread->ownerTask)->ownerTask;
	uint64_t apic_id = cls->apic_id;
	thread_t* forkedThread = (thread_t*)thread->forkedThread;

	cls->currentThread = thread;
	cls->threadID = thread->threadID;
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
    mp_isrSavedRFlags[apic_id]=thread->regs.RFLAGS;
    mp_isrSavedES[apic_id]=thread->regs.ES;
    mp_isrSavedFS[apic_id]=thread->regs.FS;
    mp_isrSavedGS[apic_id]=thread->regs.GS;
    mp_isrSavedCR3[apic_id]=thread->regs.CR3;
    
    printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"scheduler_load_thread: Loading SYSENTER_ESP_MSR with value 0x%08x\n",thread->regs.RSP0);
    //kInitialTSS

	//Set the syscall (ring 0) stack pointer.  This replaces using SYSENTER_ESP_MSR from the old 32-bit code.
	kInitialTSS.rsp0 = thread->regs.RSP0;

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
#if SCHEDULER_DEBUG == 1
	debug_print_registers(apic_id, "load", false);
#endif
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

    if (task->threads->threadState == THREAD_STATE_ISLEEP) {
        scheduler_change_thread_queue(task->threads, THREAD_STATE_RUNNABLE);
    }
    task->threads->prioritizedTicksInRunnable += HIGH_PRIORITY_TICKS_BOOST;
    scheduler_trigger(NULL);
}

thread_t *scheduler_find_thread_to_run(core_local_storage_t *cls, bool justBrowsing)
{
    uint32_t mostIdleTicks=0, oldTicks;
    task_t *task;
    thread_t *thread, *threadToRun = NO_THREAD;
    thread_t *queue=qRunnable;
    
    int queEntryNum = 0;
    while (queue!=NO_NEXT)
    {
		thread = queue;
		task = (task_t*)thread->ownerTask;
		oldTicks=thread->prioritizedTicksInRunnable;
		//This is where we increment all the runnable ticks, based on the process' priority
		if (!thread->idleThread)
			thread->prioritizedTicksInRunnable+=(RUNNABLE_TICKS_INTERVAL-task->priority)+1;
		if (!justBrowsing)
			printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*\t%u-Thr 0x%08x (tsk 0x%08x-%s), pri=%i, oldt=%u, newt=%u (runt=%u)\n",
					queEntryNum,
					thread->threadID, 
					task->taskID,
					task->exename,
					task->priority,
					oldTicks,
					thread->prioritizedTicksInRunnable, 
					thread->totalRunTicks);
		if ( thread->prioritizedTicksInRunnable >= mostIdleTicks)
		{
			//All non-idle threads, and idle threads belonging to the current core, can be selected to run
			if (!thread->idleThread || (thread->idleThread && thread->mp_apic==cls->apic_id))
			{
				if (thread->idleThread)
					printd(DEBUG_SCHEDULER | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"*\t\tfindTaskToRun: Found idle thread for APIC %u\n",cls->apic_id);
				threadToRun=thread;
				mostIdleTicks=thread->prioritizedTicksInRunnable;
			}
		}
        queEntryNum++;
        queue=queue->next;
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
    mp_waitingForScheduler[cls->apic_id] = true;
    mp_schedulerEnabled[cls->apic_id] = true;

	//Since we're calling a different vector than the APIC timer does, we need to reset the timer count
    mp_restart_apic_timer_count();
    send_ipi(cls->apic_id, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
	__asm__ volatile("sti\nhlt\n");      //Halt till the scheduler runs again
}

void scheduler_debug_rsp_value(char* prefix)
{
	uint64_t temp_rsp;

	__asm__ volatile("mov %0, rsp\n": "=r" (temp_rsp));
	printd(DEBUG_SCHEDULER | DEBUG_DETAILED, "scheduler_debug_rsp_value: %s RSP is 0x%016lx\n", prefix, temp_rsp);
}


/// @brief Yield control of the CPU
/// @param cls Optional - can be NULL if not valid in the calling context
/// @details Yields control of the CPU if there is another thread that is ready to run.  If not, does a sti\nhlt\n to wait for the next timer tick (BSP) or scheduling IPI
void scheduler_yield(core_local_storage_t *cls)
{
	 if (!cls)
	 	cls = get_core_local_storage();

	thread_t* thread=scheduler_find_thread_to_run(cls, true);

	//If another thread is ready to run then trigger the scheduler, otherwise just hlt until the next scheduling IPI
	if (thread != NO_THREAD && thread->threadID != cls->threadID)
		scheduler_trigger(cls);
	else
		__asm__("sti\nhlt\n");
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
		printd(DEBUG_SCHEDULER,"*Found thread 0x%08x to take off CPU @0x%04x:0x%08x (exited=%u, retval=0x%08x).\n",
				taskToStop->taskID, 
				mp_isrSavedCS[apic_id],mp_isrSavedRIP[apic_id],
				threadToStop->exited, 
				threadToStop->retVal);

		if (threadToStop->exited)
		{
			printd(DEBUG_SCHEDULER,"*Thread (0x%08x) ended, moving it to the zombie queue.\n",threadToStop->threadID);

			threadToStopNewQueue=THREAD_STATE_ZOMBIE;
			//TODO: If this is the last thread for the task then do something with the task, INCLUDING resetting its GDT entry
		}
        else if (threadToStop->signals.sigind && SIGSLEEP)
			threadToStopNewQueue=THREAD_STATE_ISLEEP;
		else
            threadToStopNewQueue=THREAD_STATE_RUNNABLE;
        scheduler_store_thread(cls, threadToStop);              //we're taking it off the cpu so save the registers
        scheduler_change_thread_queue(threadToStop, threadToStopNewQueue);
	}
	printd(DEBUG_SCHEDULER | DEBUG_DETAILED,"*Finding thread to run\n");
    thread_t* threadToRun=scheduler_find_thread_to_run(cls, false);
	task_t* taskToRun = (task_t*)threadToRun->ownerTask;
	
    if (threadToStop && threadToRun->threadID==threadToStop->threadID)
    {
        printd(DEBUG_SCHEDULER,"*No new thread to run, continuing with the current task\n");
		#if SCHEDULER_DEBUG == 1
		debug_print_registers(apic_id, "continue2", false);
		#endif
        if (threadToStop->execDontSaveRegisters)
        {
            printd(DEBUG_SCHEDULER,"Thread to keep running was just exec'd, loading registers from tss\n");
            //TODO: Should be able to get rid of the load statement
			scheduler_load_thread(cls, threadToStop);
            threadToStop->execDontSaveRegisters = false;
        }
        scheduler_change_thread_queue(threadToStop,THREAD_STATE_RUNNING);   //switch it back to the running queue
    }
	else
	{
        printd(DEBUG_SCHEDULER,"*Found thread to move to CPU (%x - %s)\n",threadToRun->threadID, taskToRun->exename);
        scheduler_change_thread_queue(threadToRun, THREAD_STATE_RUNNING);
        scheduler_load_thread(cls, threadToRun);
		task_t *pTask = (task_t*)threadToRun->ownerTask;
        if (!strnstr(pTask->exename, "/idle",10))
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
		printd(DEBUG_SCHEDULER,"*Restarting CPU with new process (0x%04x) @ 0x%04x:0x%08x\n",threadToRun->threadID,threadToRun->regs.CS,threadToRun->regs.RIP);
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
	} //New thread loaded
}

//NOTE: When this method is entered, it is time to reschedule.
void scheduler_do() 
{
	core_local_storage_t *cls = get_core_local_storage();
	uint8_t apic_id = cls->apic_id;
    mp_waitingForScheduler[apic_id] = false;
    printd(DEBUG_SCHEDULER,"****************************** SCHEDULER *******************************\n");
    printd(DEBUG_SCHEDULER,"scheduler: AP %u, current CR3 = 0x%08x\n",apic_id,getCR3());
#ifdef SCHEDULER_DEBUG
    uint64_t ticksBefore=rdtsc();
#endif
	//Lock the section of code from the time we start looking for another thread to run, until we're done 
	//either switching threads, or have identified that there's no new thread to run
	while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1));
    thread_t* threadToRun=scheduler_find_thread_to_run(cls, true);
  	if (threadToRun != NO_THREAD && threadToRun->threadID!=cls->threadID)
    {
		printd(DEBUG_SCHEDULER, "Time to make the donuts. (switch threads)\n");
		scheduler_run_new_thread();
	}
	else
	{
		#if SCHEDULER_DEBUG == 1
		debug_print_registers(apic_id, "continue", true);
		#endif
        printd(DEBUG_SCHEDULER,"*Shortcut! No new thread to run, continuing with 0x%016lx-%s\n", cls->currentThread->threadID, ((task_t*)cls->currentThread->ownerTask)->exename);
	}
	__sync_lock_release(&kSchedulerSwitchTasksLock);   
    kSchedulerCallCount++;
#ifdef SCHEDULER_DEBUG
    uint64_t ticksAfter=rdtsc();
#endif
    mp_CoreHasRunScheduledThread[apic_id] = true;

#ifdef SCHEDULER_DEBUG
    printd(DEBUG_SCHEDULER,"*Scheduler: calls=%u, task switchs=%u, ticks since start=0x%08x\n",kSchedulerCallCount, kTaskSwitchCount, kTicksSinceStart);
    uint64_t diff = ticksAfter-ticksBefore;
    uint64_t timeInScheduler = (diff/kCPUCyclesPerSecond)*100;
    printd(DEBUG_SCHEDULER,"%lu ticks expired (%lu CPU cycles)\n",timeInScheduler, diff);
#endif
	printd(DEBUG_SCHEDULER,"**************************************************************************\n");
}
