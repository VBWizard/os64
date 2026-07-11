#include "signals.h"
#include "CONFIG.h"
#include "scheduler.h"
#include "kernel.h"
#include "serial_logging.h"
#include "panic.h"
#include "thread.h"
#include "smp_core.h"

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
		//Set the data first in case a task switch takes place before setting the sigind	
        thread->signals.sigdata[SIGSLEEP]=sigData;
            thread->signals.sigind |= SIGSLEEP;
            printd(DEBUG_SIGNALS, "Signalling SLEEP for thread 0x%08x, wakeTicks=%i\n", thread->threadID, sigData);
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
	thread_t *qSleep = qISleep;
	bool awoken = false;

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"processSignals: Start processing signals\n");
	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tScanning Interruptable Sleep queue\n");

	//Set the scheduler task switch lock so that other APs don't see inconsistent state
	while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1)) __builtin_ia32_pause();
	while (qSleep != NO_THREAD)
	{
		if (qSleep->signals.sigdata[SIGSLEEP] <= kTicksSinceStart) // Wake up the thread if the wake time is *now* or in the past
		{
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			qSleep->signals.sigind &= ~(SIGSLEEP);
			scheduler_change_thread_queue(qSleep, THREAD_STATE_RUNNABLE);
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP\n", qSleep->threadID);
			awoken = true;
		}
		qSleep = qSleep->next;
	}
	//Release the lock
	__sync_lock_release(&kSchedulerSwitchTasksLock);

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tprocessSignals: Done processing signals\n");
	//No need to act on "awoken" since processSignals() is called by the scheduler
}

void init_signals()
{
		signalProcTickFrequency = SIGNAL_PROCESS_TICK_FREQUENCY;
}