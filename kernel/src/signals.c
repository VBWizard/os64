#include "signals.h"
#include "CONFIG.h"
#include "scheduler.h"
#include "kernel.h"
#include "serial_logging.h"
#include "panic.h"
#include "thread.h"
#include "smp_core.h"
#include "console.h"
#include "pipe.h"
#include "driver/system/usb/xhci.h"

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
    (void)sigAction;   // reserved for real handler registration; unused until then
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

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"processSignals: Start processing signals\n");
	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tScanning Interruptable Sleep queue\n");

	//Set the scheduler task switch lock so that other APs don't see inconsistent state
	while (__sync_lock_test_and_set(&kSchedulerSwitchTasksLock, 1)) __builtin_ia32_pause();
	while (qSleep != NO_THREAD)
	{
		// Capture the neighbor BEFORE any requeue: scheduler_change_thread_queue
		// relinks the node into qRunnable, so following ->next afterward would
		// walk off into the wrong queue mid-iteration.
		thread_t *nextSleeper = qSleep->next;

		if (qSleep->signals.sigind & SIGNALS_TERMINATING)
		{
			// A pending terminate outranks the nap. Cancel the sleep and wake
			// the thread INTO its own blocking loop (console_read / pipe_read /
			// pipe_write), whose top-of-loop terminate check bails out to the
			// syscall boundary — where the default action (terminate, 130) is
			// enforced in the dying task's own context, free to sleep and to
			// close handles. This wake is what makes Ctrl+C (and a ctl write)
			// reach a task that is blocked and would otherwise never run again
			// to notice it.
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			qSleep->signals.sigind &= ~(SIGSLEEP);
			scheduler_change_thread_queue(qSleep, THREAD_STATE_RUNNABLE);
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP by a pending terminate\n", qSleep->threadID);
		}
		else if (qSleep->signals.sigdata[SIGSLEEP] <= kTicksSinceStart) // Wake up the thread if the wake time is *now* or in the past
		{
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			qSleep->signals.sigind &= ~(SIGSLEEP);
			scheduler_change_thread_queue(qSleep, THREAD_STATE_RUNNABLE);
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP\n", qSleep->threadID);
		}
		qSleep = nextSleeper;
	}

	// Drain the USB keyboard BEFORE the console wake check below, so a HID
	// report that just completed becomes a ring event the same pass its
	// reader wakes for. Polling here rides the exact liveness path the PS/2
	// wake uses — no interrupt wiring, ~one-pass latency. Cheap when idle
	// (guard branch + one cached-RAM read); does nothing before USB init.
	xhci_poll();

	// Wake a blocked console reader if the keyboard driver has input. Done
	// here — under the lock, AFTER the qISleep walk above — so its queue
	// surgery (ISLEEP->RUNNABLE) can't corrupt that iteration. Level-triggered
	// on keyboard_has_event(), which is what makes console_read lost-wakeup-free.
	console_wake_if_ready();

	// Same discipline, same reason, for pipes: wake any reader whose pipe has
	// bytes (or whose last writer just left — that is its EOF), and any writer
	// whose pipe has room (or whose last reader just left — that is its EPIPE).
	// The fast path already woke the common cases directly from pipe_read /
	// pipe_write; this level-triggered sweep exists to catch the one race they
	// cannot: a thread that registered as a waiter but had not yet parked when
	// the wake fired. Re-evaluating the CONDITION (not a remembered edge) is
	// what makes pipes lost-wakeup-free.
	pipe_wake_if_ready();

	//Release the lock
	__sync_lock_release(&kSchedulerSwitchTasksLock);

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tprocessSignals: Done processing signals\n");
	//No need to act on "awoken" since processSignals() is called by the scheduler
}

void init_signals()
{
		signalProcTickFrequency = SIGNAL_PROCESS_TICK_FREQUENCY;
}