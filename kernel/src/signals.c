#include "signals.h"
#include "CONFIG.h"
#include "scheduler.h"
#include "paging.h"
#include "kernel.h"
#include "serial_logging.h"
#include "panic.h"
#include "thread.h"
#include "smp_core.h"

extern pt_entry_t kKernelPML4;
extern volatile int kSchedulerSwitchTasksLock;
bool kProcessSignals = false;
uint8_t signalProcTickFrequency;

/// @brief 
/// @param signal See SIGNALS.H for the enum
/// @param sigAction 
/// @param sigData Data to accompany the signal (i.e. for SIGSLEEP, wake up ticks)
/// @param thrd - The thread to signal.  If NULL the current thread is signaled
/// @return 
void *sigaction(int signal, uintptr_t *sigAction, uint64_t sigData, void *thrd)
{
    uintptr_t *a = sigAction; // Temporary workaround to suppress "unused parameter 'sigAction'" compiler error
    thread_t *thread = thrd;

	if (thread == NULL)
	{
		core_local_storage_t* cls = get_core_local_storage();
		thread = cls->currentThread;
	}

	switch (signal)
	{
		case SIGSLEEP:
			thread->signals.sigind|=SIGSLEEP;
			thread->signals.sigdata[SIGSLEEP]=sigData;
            printd(DEBUG_SIGNALS,"Signalling SLEEP for thread 0x%08x, wakeTicks=%i\n",thread->threadID,sigData);
			scheduler_trigger(NULL);
			break;
		case SIGLOGFLUSH:
			thread->signals.sigind |= SIGLOGFLUSH;
			printd(DEBUG_SIGNALS, "Signalling LOGFLUSH for thread 0x%08x\n", thread->threadID);
			scheduler_trigger(NULL);
			break;
		default:
			panic("sigaction: Unknown signal %u\n",signal);
	}

	return NULL;
}

//Iterate the running, runnable and sleeping queues, looking for new signals
void processSignals()
{
	uintptr_t priorCR3=0;
	thread_t *qSleep = qISleep;
	bool awoken = false;

    printd(DEBUG_SIGNALS | DEBUG_DETAILED,"processSignals: Start processing signals\n");

	// Disable interrupts and grab the current CR3.
	__asm__ __volatile__(
		"mov %%rbx, cr3\n"
		: "=b" (priorCR3)
		:
		: "memory"
	);

	// Update CR3 only if needed.
	if (priorCR3 != (uint64_t)kKernelPML4)
	{
		__asm__ __volatile__(
			"mov cr3, %[cr3Val]\n"
			:
			: [cr3Val] "r"((uint64_t)kKernelPML4)
			: "memory"
		);
	}

    printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tScanning Interruptable Sleep queue\n");

	//Set the scheduler task switch lock so that other APs don't see inconsistent state
	while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1));
	while (qSleep != NO_THREAD)
	{
		if (qSleep->signals.sigdata[SIGSLEEP] < kTicksSinceStart)
		{
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			qSleep->signals.sigind&=~(SIGSLEEP);
			scheduler_change_thread_queue(qSleep, THREAD_STATE_RUNNABLE);
    		printd(DEBUG_SCHEDULER,"\tThread 0x%08x awoken from ISLEEP\n", qSleep->threadID);
			awoken = true;
		}
		qSleep = qSleep->next;
	}
	//Relese the lock
	__sync_lock_release(&kSchedulerSwitchTasksLock);   

	
    printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tprocessSignals: Done processing signals\n");
    //No need to act on "awoken" since processSignals() is called by the scheduler
	if (priorCR3 != (uint64_t)kKernelPML4)
	    __asm__("mov cr3,%[cr3Val]\n"::[cr3Val] "r" (priorCR3));
}

void init_signals()
{
		signalProcTickFrequency = SIGNAL_PROCESS_TICK_FREQUENCY;
}