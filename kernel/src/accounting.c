#include "accounting.h"

#include "scheduler.h"   // NO_THREAD
#include "smp_core.h"    // get_core_local_storage
#include "thread.h"
#include "driver/system/x86_64.h"

// Pause between failed reads so a normal charge can close its odd sequence;
// the bound still lets a stalled writer return an unavailable sample.
#define ACCOUNTING_READ_RETRIES 1024

static uint64_t accounting_irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
	return flags;
}

static void accounting_irq_restore(uint64_t flags)
{
	__asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");
}

static void sequence_begin(uint64_t *sequence)
{
	__atomic_fetch_add(sequence, 1, __ATOMIC_SEQ_CST);
}

static void sequence_end(uint64_t *sequence)
{
	__atomic_fetch_add(sequence, 1, __ATOMIC_SEQ_CST);
}

// The caller has IF masked. Only the core executing this code writes its
// open span, and the thread it names cannot run on another core at once.
static void charge_locked(core_local_storage_t *cls, uint64_t cs,
                          bool change_sched, bool next_sched,
                          bool change_halt, bool next_halt)
{
	if (cls == NULL)
		return;
	// A fault in this writer enters through the exception stamp again. Leave
	// its odd sequence alone so the original fault reaches the reporter.
	if (__atomic_load_n(&cls->acctSequence, __ATOMIC_RELAXED) & 1)
		return;
	thread_t *current = cls->currentThread;
	if (cls->acctLastDispatchTSC == 0 && !change_sched &&
	    (current == NULL || current == NO_THREAD))
		return;
	uint64_t now = rdtsc();
	sequence_begin(&cls->acctSequence);
	if (cls->acctLastDispatchTSC != 0 && now >= cls->acctLastDispatchTSC)
	{
		uint64_t delta = now - cls->acctLastDispatchTSC;
		if (cls->acctSchedSpanActive)
		{
			cls->acctSchedCycles += delta;
		}
		else if (current != NULL && current != NO_THREAD)
		{
			thread_t *target = (cls->acctCurrentHalted && cls->acctIdleThread != NULL)
			                       ? cls->acctIdleThread : current;
			sequence_begin(&target->acctSequence);
			target->runCycles += delta;
			if (target->idleThread || cls->acctCurrentHalted)
				target->idleCycles += delta;
			else if ((cs & 3) == 3)
			{
				target->userCycles += delta;
				cls->acctUserCycles += delta;
			}
			else
			{
				target->systemCycles += delta;
				if (current->acctFaultDepth != 0)
					target->faultCycles += delta;
				cls->acctSystemCycles += delta;
			}
			sequence_end(&target->acctSequence);
		}
		else
			cls->acctSchedCycles += delta;
	}
	if (cls->acctZeroTSC == 0)
		cls->acctZeroTSC = now;
	cls->acctLastDispatchTSC = now;
	if (change_sched)
		cls->acctSchedSpanActive = next_sched;
	if (change_halt)
		cls->acctCurrentHalted = next_halt;
	sequence_end(&cls->acctSequence);
}

void accounting_charge(uint64_t cs)
{
	uint64_t flags = accounting_irq_save();
	charge_locked(get_core_local_storage(), cs, false, false, false, false);
	accounting_irq_restore(flags);
}

void accounting_exception_enter(uint64_t interrupted_cs, uint64_t vector)
{
	uint64_t flags = accounting_irq_save();
	core_local_storage_t *cls = get_core_local_storage();
	charge_locked(cls, interrupted_cs, false, false, false, false);
	if (vector == 14 && cls != NULL && cls->currentThread != NULL &&
	    cls->currentThread != NO_THREAD)
		cls->currentThread->acctFaultDepth++;
	accounting_irq_restore(flags);
}

void accounting_exception_exit(uint64_t vector)
{
	uint64_t flags = accounting_irq_save();
	core_local_storage_t *cls = get_core_local_storage();
	charge_locked(cls, 0, false, false, false, false);
	if (vector == 14 && cls != NULL && cls->currentThread != NULL &&
	    cls->currentThread != NO_THREAD && cls->currentThread->acctFaultDepth != 0)
		cls->currentThread->acctFaultDepth--;
	accounting_irq_restore(flags);
}

void accounting_begin_scheduler(uint64_t interrupted_cs)
{
	uint64_t flags = accounting_irq_save();
	charge_locked(get_core_local_storage(), interrupted_cs, true, true, false, false);
	accounting_irq_restore(flags);
}

void accounting_end_scheduler(void)
{
	uint64_t flags = accounting_irq_save();
	charge_locked(get_core_local_storage(), 0, true, false, true, false);
	accounting_irq_restore(flags);
}

void accounting_halt_begin(void)
{
	// The caller already has IF masked through sti;hlt.
	charge_locked(get_core_local_storage(), 0, false, false, true, true);
}

void accounting_halt_end(void)
{
	uint64_t flags = accounting_irq_save();
	charge_locked(get_core_local_storage(), 0, false, false, true, false);
	accounting_irq_restore(flags);
}

bool accounting_read_thread(const thread_t *thread, accounting_thread_cycles_t *out)
{
	if (thread == NULL || out == NULL)
		return false;
	for (unsigned attempt = 0; attempt < ACCOUNTING_READ_RETRIES; attempt++)
	{
		uint64_t before = __atomic_load_n(&thread->acctSequence, __ATOMIC_ACQUIRE);
		if (before & 1)
		{
			__builtin_ia32_pause();
			continue;
		}
		accounting_thread_cycles_t copy = {
			__atomic_load_n(&thread->runCycles, __ATOMIC_RELAXED),
			__atomic_load_n(&thread->userCycles, __ATOMIC_RELAXED),
			__atomic_load_n(&thread->systemCycles, __ATOMIC_RELAXED),
			__atomic_load_n(&thread->idleCycles, __ATOMIC_RELAXED),
			__atomic_load_n(&thread->faultCycles, __ATOMIC_RELAXED),
		};
		// Keep tuple loads before the closing sequence check.
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		uint64_t after = __atomic_load_n(&thread->acctSequence, __ATOMIC_ACQUIRE);
		if (before == after && !(after & 1) &&
		    copy.runtime >= copy.user && copy.runtime - copy.user >= copy.system &&
		    copy.runtime - copy.user - copy.system == copy.idle &&
		    copy.fault <= copy.system)
		{
			*out = copy;
			return true;
		}
		__builtin_ia32_pause();
	}
	return false;
}

bool accounting_read_core(const core_local_storage_t *cls, accounting_core_cycles_t *out)
{
	if (cls == NULL || out == NULL)
		return false;
	for (unsigned attempt = 0; attempt < ACCOUNTING_READ_RETRIES; attempt++)
	{
		uint64_t before = __atomic_load_n(&cls->acctSequence, __ATOMIC_ACQUIRE);
		if (before & 1)
		{
			__builtin_ia32_pause();
			continue;
		}
		uint64_t zero = __atomic_load_n(&cls->acctZeroTSC, __ATOMIC_RELAXED);
		uint64_t last = __atomic_load_n(&cls->acctLastDispatchTSC, __ATOMIC_RELAXED);
		thread_t *idle = __atomic_load_n(&cls->acctIdleThread, __ATOMIC_RELAXED);
		accounting_core_cycles_t copy = {
			.total = last >= zero ? last - zero : 0,
			.user = __atomic_load_n(&cls->acctUserCycles, __ATOMIC_RELAXED),
			.system = __atomic_load_n(&cls->acctSystemCycles, __ATOMIC_RELAXED),
			.idle = idle ? __atomic_load_n(&idle->runCycles, __ATOMIC_RELAXED) : 0,
			.sched = __atomic_load_n(&cls->acctSchedCycles, __ATOMIC_RELAXED),
		};
		// Keep tuple loads before the closing sequence check.
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		uint64_t after = __atomic_load_n(&cls->acctSequence, __ATOMIC_ACQUIRE);
		if (before == after && !(after & 1) &&
		    copy.total >= copy.user && copy.total - copy.user >= copy.system &&
		    copy.total - copy.user - copy.system >= copy.idle &&
		    copy.total - copy.user - copy.system - copy.idle == copy.sched)
		{
			*out = copy;
			return true;
		}
		__builtin_ia32_pause();
	}
	return false;
}
