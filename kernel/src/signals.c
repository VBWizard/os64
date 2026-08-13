#include "signals.h"
#include "CONFIG.h"
#include "scheduler.h"
#include "kernel.h"
#include "serial_logging.h"
#include "panic.h"
#include "thread.h"
#include "smp_core.h"
#include "console.h"
#include "tty.h"     // tty_summon_wake — rousting kworker for a knocked terminal
#include "pipe.h"
#include "thread_join.h"
#include "BasicRenderer.h"   // renderer_flush_if_dirty — the blit-throttle rider
#include "driver/system/usb/xhci.h"
#include "driver/net/virtio_net.h"
#include "driver/net/e1000.h"
#include "driver/net/dhcp.h"
#include "driver/net/udp_conn.h"
#include "driver/net/tcp.h"
#include "driver/net/icmp_conn.h"

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
			scheduler_change_thread_queue_locked(qSleep, THREAD_STATE_RUNNABLE);   //we hold the queue lock (above)
			printd(DEBUG_SCHEDULER, "\tThread 0x%08x awoken from ISLEEP by a pending terminate\n", qSleep->threadID);
		}
		else if (qSleep->signals.sigdata[SIGSLEEP] <= kTicksSinceStart) // Wake up the thread if the wake time is *now* or in the past
		{
			qSleep->signals.sigdata[SIGSLEEP] = 0;
			qSleep->signals.sigind &= ~(SIGSLEEP);
			scheduler_change_thread_queue_locked(qSleep, THREAD_STATE_RUNNABLE);   //we hold the queue lock (above)
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

	// Same liveness ride for the NIC: drain TX completions and deliver RX
	// arrivals once per pass. Same economics as xhci_poll — a guard branch
	// and two ring-index compares when idle; interrupt wiring is a future
	// slice (NETWORK.md), and this path is what makes packets move today.
	virtio_net_poll();
	// Every registered NIC needs its own drainer — this is a per-DRIVER
	// call, not a per-device one, and each is a guard branch away from free
	// when its hardware is absent. (A generic "poll every net_device" verb
	// through the seam is the obvious tidy-up; it waits for a third driver
	// to make the abstraction pay, the same way the seam itself waited for
	// a second one.)
	//
	// Since 2026-08-06 the e1000 drain is DOORBELL-GATED when INTx is live:
	// the ISR (vector 0x45) raises kE1000RxWork and this pass consumes it.
	// Clear BEFORE draining — a packet that lands mid-drain re-raises the
	// flag and schedules the next pass, instead of vanishing into a
	// cleared-after window (the classic lost-wakeup shape, same reason the
	// wake sweeps re-evaluate conditions instead of remembering edges).
	// No confirmed wire = unconditional poll, yesterday's behavior exactly.
	if (!kE1000UsesIntx || kE1000RxWork)
	{
		// If the ISR divorced the wire at runtime (shared line gone hostile
		// — see storm breaker #2 in e1000.c), announce it exactly once. The
		// fallback itself already happened in interrupt context; this is
		// the no-silent-fallbacks receipt.
		if (kE1000IntxDivorced)
		{
			kE1000IntxDivorced = false;
			printf("e1000: INTx wire went hostile (stranger storm) — back to polling\n");
		}
		kE1000RxWork = false;
		e1000_poll();
	}

	// DHCP's retry timer rides the same pass (one state compare when the
	// lease is settled). Delivery of DHCP replies happens inside the poll
	// above (UDP demux → dhcp_rx); this call only handles the wire going
	// QUIET — resend, and eventually give up honestly.
	dhcp_poll();

	// TCP's clock: retransmission deadlines, connect timeouts, and the
	// TIME_WAIT reaper. THIS is what makes the stream reliable — a stack
	// with no timer is a stack that hangs the first time a packet is
	// lost. Walks only live connections (usually none).
	tcp_poll();

	// Blit-throttle flush: if a console scroll burst left the glass behind
	// its shadow (BasicRenderer's ~30Hz throttle), deliver the finished
	// frame. Costs one branch when the glass is current — which is always,
	// except mid-firehose, which is exactly when it earns its keep.
	renderer_flush_if_dirty();

	// The tty layer's half of the same discipline: a focused-VT scroll burst
	// defers the glass entirely (no per-line memmove — the frozen-cat fix),
	// and this rider repaints from the grid at the same ~30Hz. Ordered after
	// renderer_flush_if_dirty so a frame both riders touch settles in one
	// pass. Costs one branch when nothing is stale.
	tty_flush_if_dirty();

	// Wake any blocked console reader whose terminal has input (one sweep
	// over the tty fleet since VTs arrived). Done here — under the lock,
	// AFTER the qISleep walk above — so its queue surgery (ISLEEP->RUNNABLE)
	// can't corrupt that iteration. Level-triggered on the ring condition,
	// which is what makes console_read lost-wakeup-free.
	console_wake_if_ready();

	// And if a keystroke knocked on a DORMANT terminal, roust kworker early
	// to go play midwife (tty_summon_sweep spawns the shell in task context;
	// kworker's own backstop nap is 2s — fine for burials, rude for a human
	// standing at a dark terminal). Same wake idiom, same lock, same reasons.
	tty_summon_wake();

	// Same discipline, same reason, for pipes: wake any reader whose pipe has
	// bytes (or whose last writer just left — that is its EOF), and any writer
	// whose pipe has room (or whose last reader just left — that is its EPIPE).
	// The fast path already woke the common cases directly from pipe_read /
	// pipe_write; this level-triggered sweep exists to catch the one race they
	// cannot: a thread that registered as a waiter but had not yet parked when
	// the wake fired. Re-evaluating the CONDITION (not a remembered edge) is
	// what makes pipes lost-wakeup-free.
	pipe_wake_if_ready();

	// And for dialed network conversations: wake any reader whose datagram
	// ring is non-empty. Placed AFTER virtio_net_poll above on purpose —
	// a packet delivered this pass wakes its reader this same pass, so
	// blocking-read latency is one scheduler pass, not two. (UDP conns
	// have NO fast-path wake at all — their enqueue runs in RX context,
	// which may not hold this lock — so this sweep is their only waker;
	// see the context map in udp_conn.c.)
	udp_conn_wake_if_ready();

	// Same for TCP streams: a reader wakes for bytes, EOF, or death; a
	// writer wakes when its segment is acknowledged. Same level-triggered
	// re-evaluation, same reason.
	tcp_wake_if_ready();

	// And echo conversations: a reader wakes when its reply lands.
	icmp_conn_wake_if_ready();

	// And threads: wake anyone blocked reading a thread handle whose
	// thread has finished. Same level-triggered discipline — the answer
	// stays true once it exists, so a missed edge costs a tick, never a
	// hang.
	thread_join_wake_if_ready();

	//Release the lock
	__sync_lock_release(&kSchedulerSwitchTasksLock);

	// RFLAGS-tripwire reporter (SCHEDULER_STRAY_WRITE.md): scheduler.S
	// impounds any corrupt mp_isrSavedRFlags value it catches at the iretq
	// and bumps mp_rflagsTripCount; this prints each new impound exactly
	// once. AFTER the lock release on purpose — printing is slow and the
	// evidence isn't going anywhere. DEBUG_EXCEPTIONS because that bit is
	// always-on, and this line existing in a log is the entire point:
	// the raw value names the writer (a .rodata pointer names a table, a
	// .text pointer names a function, a stack address names a frame).
	if (mp_rflagsTripCount != mp_rflagsTripReported)
	{
		mp_rflagsTripReported = mp_rflagsTripCount;
		for (uint32_t core = 0; core < MAX_CPUS; core++)
			if (mp_rflagsTripValue[core] != 0)
			{
				printd(DEBUG_EXCEPTIONS,
					"RFLAGS TRIPWIRE: core %u had mp_isrSavedRFlags = 0x%016lx (sanitized to 0x202; see SCHEDULER_STRAY_WRITE.md)\n",
					core, mp_rflagsTripValue[core]);
				printf("RFLAGS TRIPWIRE: core %u caught the stray write! value 0x%016lx\n",
					core, mp_rflagsTripValue[core]);
				mp_rflagsTripValue[core] = 0;   // impound reported; re-arm the slot
			}
	}

	printd(DEBUG_SIGNALS | DEBUG_DETAILED,"\tprocessSignals: Done processing signals\n");
	//No need to act on "awoken" since processSignals() is called by the scheduler
}

void init_signals()
{
		signalProcTickFrequency = SIGNAL_PROCESS_TICK_FREQUENCY;
}