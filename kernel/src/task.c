#include "task.h"
#include "env.h"
#include "CONFIG.h"
#include "kmalloc.h"
#include "thread.h"
#include "serial_logging.h"
#include "paging.h"
#include "gdt.h"
#include "strcpy.h"
#include "strstr.h"
#include "time.h"
#include "memcpy.h"
#include "paging.h"
#include "strcmp.h"
#include "strstr.h"
#include "smp.h"
#include "smp_core.h"
#include "scheduler.h"
#include "panic.h"
#include "log.h"
#include "signals.h"
#include "kernel.h"
#include "elf_loader.h"
#include "shared_object.h"
#include "memory/vma.h"
#include "handle.h"
#include "sprintf.h"
#include "allocator.h"
#include "memset.h"
#include "kworker.h"
#include "gui/compositor.h"
#include "gui/gui_demos.h"
#include "tty.h"   // per-tty foreground (task_wait) + the shell-departed hook

extern volatile uint64_t kSystemCurrentTime;
extern task_t* kKernelTask;
extern uintptr_t kKernelPML4;

// (kForegroundTask lived here 2026-07..2026-08-08. It is tty_t.fgTask now —
// one per terminal — and NULL still means "nobody owns this console yet",
// with Ctrl+C staying an ordinary data byte there. See task.h and tty.h.)

// Shared virtual address bump pointer for all tasks that use kKernelPML4 directly.
// Tasks sharing the same PML4 must draw from the same counter or their stack
// allocations collide at the same virtual address, overwriting each other's PTEs.
static uintptr_t kKernelTaskMemoryNextVirt = KERNEL_TASK_MEMORY_BASE;

/// @brief Allocate page-aligned memory for task-specific use (stacks, structures, etc.)
/// This allocates memory in the lower half of the address space at task-specific virtual addresses.
/// @param task The task to allocate memory for
/// @param size Size in bytes to allocate (will be rounded up to page boundary)
/// @return Virtual address of allocated memory, or NULL on failure
void* task_alloc_aligned(task_t* task, size_t size)
{
	if (!task || size == 0) {
		return NULL;
	}

	// Round size up to page boundary
	size_t aligned_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	size_t page_count = aligned_size / PAGE_SIZE;

	// Allocate physical memory
	uintptr_t phys = allocate_memory_aligned(aligned_size);
	if (phys == 0) {
		printd(DEBUG_TASK, "task_alloc_aligned: Failed to allocate %lu bytes of physical memory\n", aligned_size);
		return NULL;
	}

	// Tasks sharing kKernelPML4 must use a shared counter; per-task counters all
	// start at KERNEL_TASK_MEMORY_BASE and would map at the same virtual address.
	// Since 2026-07-25 only ktask is in that position (idle tasks got their own
	// PML4 — see the address-space note in task_initialize), so this branch has
	// a single occupant today. It stays because the RULE is what matters: any
	// future task that shares an address space must draw from the shared
	// counter, and a lone occupant is not a reason to delete the guard rail.
	uintptr_t *counter = (task->pml4v == (uint64_t*)kKernelPML4v)
		? &kKernelTaskMemoryNextVirt
		: &task->taskMemoryNextVirt;

	uintptr_t virt = *counter;
	*counter += aligned_size;

	// Map pages into task's PML4
	paging_map_pages(task->pml4v, virt, phys, page_count, PAGE_PRESENT | PAGE_WRITE);

	printd(DEBUG_TASK | DEBUG_DETAILED, "task_alloc_aligned: Allocated %lu bytes (phys=0x%lx, virt=0x%lx) for task %s\n",
		aligned_size, phys, virt, task->exename);

	return (void*)virt;
}

/// @brief Reserve `size` bytes of task-address-space VA, advancing the correct
///        next-virtual counter, and return the base of the reserved range.
/// Tasks that SHARE kKernelPML4 must draw from the shared
/// kKernelTaskMemoryNextVirt counter: their per-task taskMemoryNextVirt all
/// start at KERNEL_TASK_MEMORY_BASE, so drawing from that would hand the same VA
/// to every such task and collide with their own stacks. Single source of truth
/// for both stack and arena VA allocation — keep all task-VA reservations here.
/// (Only ktask shares today; idle tasks stopped sharing on 2026-07-25 — see
/// task_initialize. This counter is the COUNTER-DRAW half of that problem; the
/// FIXED-CONSTANT half — TASK_ARGV_VIRT and friends — is what the split fixed.)
// See task.h for why this exists and why it is IRQ-safe.
void task_signal_all_threads(task_t* task, uint64_t signal)
{
	if (task == NULL)
		return;
	for (thread_t* th = task->threads; th != NULL; th = th->taskNext)
		th->signals.sigind |= signal;
}

// Bring down every thread of a dying task EXCEPT the one doing the dying.
//
// This is the single control point Chris asked for: every route to a task's
// death — ctl kill, Ctrl+C, SIGPIPE, the exit syscall, main simply
// returning, the ring-3 segfault kill — funnels through task_exit, so one
// fan-out here covers every path that exists and every path someone adds
// later. The per-site signal fan-out (procfs ctl, console, SIGPIPE) is now
// an OPTIMIZATION on top of this: it lets siblings notice in parallel
// instead of serially after the main thread gets around to dying.
//
// TWO mechanisms, because marking alone is not enough:
//
//  1. THE MARK. SIGKILL, not SIGINT — a dying task is not a request. The
//     scheduler's forced-syscall redirect (scheduler_sigint_forced_syscall)
//     sees a terminating bit and rewrites the thread's RIP to the exit
//     trampoline, so even a thread in a loop with no syscalls at all walks
//     into its own exit. No cooperation required.
//
//  2. THE NUDGE, and this is the part that only matters on the boots we
//     actually run. That redirect fires WHEN THE SCHEDULER RUNS ON THAT
//     CORE — and under tickless the AP timers are masked, so the scheduler
//     does not run on an AP spontaneously. A worker spinning on AP 5 would
//     carry its death warrant forever, unpreempted, with the redirect
//     armed and never firing. A scheduling IPI is an interrupt, so it
//     lands even on a masked-timer core: the scheduler runs there, sees
//     the bit, patches RIP, done. (Chris asked "how does this force a
//     thread off a core?" — it doesn't, until something knocks.)
void task_terminate_sibling_threads(task_t* task, thread_t* self)
{
	if (task == NULL)
		return;

	core_local_storage_t* cls = get_core_local_storage();
	uint64_t own_apic = cls ? cls->apic_id : BOOTSTRAP_PROCESSOR_ID;
	uint32_t marked = 0;

	for (thread_t* th = task->threads; th != NULL; th = th->taskNext)
	{
		// `exiting` as well as `exited`: a sibling already walking its own
		// teardown needs no second SIGKILL and no second nudge (2026-08-09,
		// when the two halves of dying were split — see thread.h).
		if (th == self || th->exited || th->exiting)
			continue;

		th->signals.sigind |= SIGKILL;
		marked++;
		printd(DEBUG_TASK | DEBUG_THREAD,
		       "task_exit: marking sibling thread 0x%08lx of %s (state %u, last ran on AP %lu) for termination\n",
		       th->threadID, task->exename, (uint32_t)th->threadState,
		       th->lastRunApicID);

		// Knock on the core it was last seen on, unless that is this core
		// (we are already inside the scheduler's reach) or the BSP (whose
		// timer is never masked, so it will notice on its own next tick).
		if (kSMPInitDone && th->lastRunApicID != own_apic &&
		    th->lastRunApicID != BOOTSTRAP_PROCESSOR_ID)
		{
			printd(DEBUG_TASK | DEBUG_THREAD,
			       "task_exit: nudging AP %lu so its scheduler can redirect thread 0x%08lx into the exit trampoline\n",
			       th->lastRunApicID, th->threadID);
			send_ipi(th->lastRunApicID, IPI_MANUAL_SCHEDULE_VECTOR, 0, 1, 0);
		}
	}

	if (marked)
		printd(DEBUG_TASK, "task_exit: %s is taking %u sibling thread%s with it\n",
		       task->exename, marked, marked == 1 ? "" : "s");
}

uintptr_t task_reserve_task_virt(task_t* task, size_t size)
{
	uintptr_t *counter = (task->pml4v == (uint64_t*)kKernelPML4v)
		? &kKernelTaskMemoryNextVirt
		: &task->taskMemoryNextVirt;
	uintptr_t base = *counter;
	*counter += size;
	return base;
}

/// @brief Allocate guarded stack memory for a task
/// Allocates stack with guard pages on both sides to detect stack overflow
/// @param task The task to allocate stack for
/// @param stackSize Size of usable stack (guard pages are added automatically)
/// @param isRing3 true for user stack (PAGE_USER set), false for kernel stack
/// @return Virtual address of stack base (bottom), or NULL on failure
void* task_alloc_guarded_stack(task_t* task, size_t stackSize, bool isRing3)
{
	if (!task || stackSize == 0) {
		return NULL;
	}

	// Calculate total allocation size including guard pages
	size_t total_size = stackSize + (THREAD_STACK_GUARD_PAGE_COUNT * PAGE_SIZE * 2);
	size_t aligned_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	// Allocate physical memory for entire region (including guards)
	uintptr_t phys = allocate_memory_aligned(aligned_size);
	if (phys == 0) {
		printd(DEBUG_TASK, "task_alloc_guarded_stack: Failed to allocate %lu bytes\n", aligned_size);
		return NULL;
	}

	uintptr_t virt_base = task_reserve_task_virt(task, aligned_size);

	// Map only the usable stack pages (skip guard pages on each end)
	uintptr_t phys_stack_start = phys + (THREAD_STACK_GUARD_PAGE_COUNT * PAGE_SIZE);
	uintptr_t virt_stack_start = virt_base + (THREAD_STACK_GUARD_PAGE_COUNT * PAGE_SIZE);
	size_t stack_page_count = (stackSize + PAGE_SIZE - 1) / PAGE_SIZE;  // Round up to cover full stack

	uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
	if (isRing3) {
		flags |= PAGE_USER;
	}

	paging_map_pages(task->pml4v, virt_stack_start, phys_stack_start, stack_page_count, flags);

	printd(DEBUG_TASK | DEBUG_DETAILED, "task_alloc_guarded_stack: Allocated %lu byte %s stack at virt=0x%lx (phys=0x%lx), guards: 0x%lx-0x%lx and 0x%lx-0x%lx\n",
		stackSize, isRing3 ? "user" : "kernel", virt_stack_start, phys_stack_start,
		virt_base, virt_stack_start,
		virt_stack_start + stackSize, virt_base + aligned_size);

	return (void*)virt_stack_start;
}

void task_idle_loop()
{
	while (1==1)
	{
        __asm__("sti\nhlt\n");
	}
}

static void task_enqueue_dead_child(task_t *child)
{
	task_t *parent = child->parentTask;

	if (parent == NULL) {
		return;
	}

	child->deadChildNext = NULL;
	if (parent->deadChildTail != NULL) {
		parent->deadChildTail->deadChildNext = child;
	} else {
		parent->deadChildHead = child;
	}
	parent->deadChildTail = child;

	if (parent->waitingForChild) {
		parent->waitingForChild = false;
		// Wake the parent to re-check its children. The wake is unconditional
		// on ANY child death; task_wait's own scan decides whether THIS death
		// matches what the parent is waiting for.
		//
		// THE BACKSTOP IS NOT OURS TO CANCEL (fixed 2026-08-09; this cost a
		// shell). Until today these three lines cleared the parent's SIGSLEEP
		// and its deadline right here, BEFORE attempting the wake, reasoning
		// that a woken parent still carrying SIGSLEEP would be parked straight
		// back to ISLEEP. True — but the wake it handed off to only lands on a
		// thread that has ALREADY parked, and sigaction(SIGSLEEP) does not
		// park: it sets the flag and asks for a scheduler pass, so there is a
		// wide window where the parent is mid-park and the wake is a silent
		// no-op. Cancel the backstop inside that window and the parent lands in
		// ISLEEP with no flag and no deadline — nothing left to fire, asleep
		// forever, holding a corpse nobody can bury (kworker only walks
		// deadChild lists reachable from a live parent). Symptom: `tail -f`
		// looked frozen; it had exited, and husk never woke to say so. Same
		// bug, same day, failed the elf_loader post-boot test under periodic —
		// that test waits on a child too. Periodic just splits the window far
		// more often than tickless does; it is not a periodic-only bug.
		//
		// scheduler_wake_isleep_task now delegates to the helper that has had
		// this right all along (scheduler_wake_isleep_thread_locked): the
		// ISLEEP test, the SIGSLEEP cancel and the relink are ONE atomic act
		// under the queue lock, and a thread that has not parked yet is left
		// completely untouched — so its backstop survives to fire a tick later
		// and re-run the scan. Worst case the parent notices one second late
		// instead of never, which is precisely what a backstop is for.
		scheduler_wake_isleep_task(parent);
	}
}

static void task_remove_dead_child(task_t *parent, task_t *child)
{
	task_t *prev = NULL;
	task_t *curr = parent ? parent->deadChildHead : NULL;

	while (curr != NULL) {
		if (curr == child) {
			if (prev != NULL) {
				prev->deadChildNext = curr->deadChildNext;
			} else {
				parent->deadChildHead = curr->deadChildNext;
			}

			if (parent->deadChildTail == curr) {
				parent->deadChildTail = prev;
			}

			curr->deadChildNext = NULL;
			return;
		}

		prev = curr;
		curr = curr->deadChildNext;
	}
}

// ── The undertaker (kworker context ONLY) ───────────────────────────────────
//
// Ruling 2026-08-06: kworker is the undertaker, and it BURIES ONLY THE
// COLLECTED — a corpse whose exit status someone has claimed (retValCollected
// via wait/reap, autoReap by decree, or an orphan whose parent is dead or
// gone). An uncollected child with a living parent stays a visible zombie
// FOREVER, on purpose: that is the 1971 contract, a zombie IS an unclaimed
// exit status, and hiding it would hide the parent's bug.
//
// Burial is TWO-PHASE, one kworker pass apart:
//   pass N:   unlink the corpse from the deadChild list AND the kTaskList
//             spine (scheduler_remove_task) and park it on kBurialList.
//             From this instant no walker can newly reach it.
//   pass N+1: task_destroy() actually frees everything.
// The gap is a grace period for LOCKLESS kTaskList walkers (procfs follows
// ->next with no lock, by design — see scheduler_submit_new_task's publish
// comment): a walker standing ON the corpse when it is unlinked can still
// follow its intact ->next out; by the time the memory is freed a full
// kworker period later, any such walk has long finished. A reader parked
// mid-walk for 2+ seconds is already a wedged reader with bigger problems.
// (The grown-up fix — snapshot-under-lock or generation counts — is booked
// in DEBTS.md; this discipline is correct until /proc grows teeth.)

// Corpses between phase 1 and phase 2, linked through task->burialNext.
// kworker-only (single consumer, single producer, same thread) — no lock.
static task_t *kBurialList = NULL;

// Every thread must be fully retired before burial: exited AND off the run
// queues (ZOMBIE still counts — task_destroy dequeues it — but RUNNING or
// RUNNABLE means a sibling is still walking to the trampoline; the corpse
// keeps until the next sweep). "Exit means exit" kills siblings, but each
// dies at its own next scheduling boundary — the undertaker must not race
// the last one down.
static bool task_threads_all_retired(task_t *t)
{
	for (thread_t *th = t->threads; th != NULL; th = th->taskNext) {
		if (!th->exited)
			return false;
		if (th->threadState != THREAD_STATE_ZOMBIE &&
		    th->threadState != THREAD_STATE_NONE)
			return false;
	}
	return true;
}

// Free one guarded stack given its usable-base VA (esp0BaseV/esp3BaseV).
// The whole thing — guards included — is ONE allocator extent
// (task_alloc_guarded_stack): resolve the first USABLE page's physical
// through the corpse's own tables, step back over the leading guard pages,
// and free_memory() releases the entire extent (and HHDM-unmaps it — the
// use-after-free tripwire now guards freed stacks too). No task-VA unmap:
// nothing will ever load this CR3 again, and the page-table pages themselves
// are pool-owned (bump allocator, can't free yet — the booked deferral).
static void task_free_guarded_stack(task_t *t, uintptr_t stackBaseV)
{
	if (stackBaseV == 0 || t->pml4v == NULL)
		return;
	uintptr_t phys = paging_walk_paging_table((pt_entry_t *)t->pml4v, stackBaseV);
	if (phys == 0 || phys == 0xbadbadba)
		return;
	free_memory((phys & ~(uintptr_t)0xFFF) -
	            (THREAD_STACK_GUARD_PAGE_COUNT * PAGE_SIZE));
}

// THE ORPHANAGE (the night's second lesson, 2026-08-06): when a task is
// buried, any children it left behind — LIVING and DEAD — are re-parented to
// kKernelTask, the eternal parent. This is 1971's exact mechanism (orphans
// go to PID 1) rediscovered empirically: the first burial run left ONE
// permanent zombie, cwd_test's own child, whose parent was buried without
// waiting on it. The corpse wasn't just leaked — it was UNREACHABLE, because
// the sweep only walks living parents' deadChild lists, and its list died
// with its parent (plus its parentTask pointer dangled at freed memory).
// Since ktask never waits — it has no wait loop and never will — orphans are
// simultaneously marked autoReap: collected by decree. That decree IS init's
// wait-loop, translated: Unix buries orphans by making PID 1 wait for them;
// os64 buries them by declaring the wait unnecessary. Same funeral, less
// ceremony.
static void task_reparent_orphans(task_t *dying)
{
	// Live children first: after this, a child that exits mid-burial
	// enqueues itself on ktask's deadChild list, not the corpse's.
	for (task_t *t = kTaskList; t != NULL && t != (task_t *)NO_TASK; t = t->next) {
		if (t->parentTask == dying) {
			t->parentTask = kKernelTask;
			t->autoReap = true;
		}
	}

	// Then the dead: splice every unwaited grandcorpse onto ktask's list so
	// the sweep can reach it again. (task_enqueue_dead_child routes by
	// parentTask, so reparent-then-enqueue is the whole move.)
	while (dying->deadChildHead != NULL) {
		task_t *c = dying->deadChildHead;
		dying->deadChildHead = c->deadChildNext;
		c->deadChildNext = NULL;
		c->parentTask = kKernelTask;
		c->autoReap = true;
		task_enqueue_dead_child(c);
	}
	dying->deadChildTail = NULL;
}

// Phase 2: the actual burial. Caller (kworker sweep) guarantees the corpse
// is collected, all threads retired, and it has been OFF every public list
// for a full kworker pass. Frees, in the cleanup notes' order, everything a
// task exclusively owns TODAY:
//   - per-thread: zombie-queue dequeue, kernel+user stacks (the 1MB+64KB
//     that made every command a leak), syscall scratch, TID, thread_t
//   - task: path, cwd, static elf_image_t (+ its still-open backing file —
//     which also releases ext2's open-inode refcount, so rm stops being
//     haunted by dead tasks' binaries), the struct itself
// DEFERRED, documented, booked (docs/task_cleanup_notes.md is the map):
//   - VMA backing pages + vma structs (CoW/fork sharing needs refcounts
//     before freeing is safe), argv/env blobs (env is CoW-inherited by
//     children), shared_objects dlist nodes, mmaps dlist
//   - page-table pages (the paging pool is a bump allocator — pool free is
//     its own slice)
// A dynamic task's elf points INTO the shared_object cache (task.c sets
// task->elf = main_so->image) — never freed here; the cache owns it.
static void task_destroy(task_t *t)
{
	printd(DEBUG_TASK, "task_destroy: burying task 0x%08x (%s)\n",
	       t->taskID, t->exename);

	// Defensive second pass of the orphanage: a child racing its own exit on
	// another core during the grace pass may have captured the old parentTask
	// pointer and enqueued itself on this corpse AFTER phase 1's splice. Any
	// such straggler gets rescued here, at the last instant the corpse's
	// deadChild list is still readable.
	task_reparent_orphans(t);

	thread_t *th = t->threads;
	while (th != NULL) {
		thread_t *next = th->taskNext;
		scheduler_reap_zombie_thread(th);   // no-op if already NONE
		task_free_guarded_stack(t, th->esp0BaseV);
		task_free_guarded_stack(t, th->esp3BaseV);
		kfree(th->syscallIOScratch);
		mark_TID_unused((uint32_t)th->threadID);
		kfree(th);
		th = next;
	}
	t->threads = NULL;

	kfree(t->path);
	kfree(t->cwd);

	// Static ELF only: the loader kept the file open for file-backed demand
	// paging, and with every thread retired no fault can ever need it again.
	elf_image_t *image = (elf_image_t *)t->elf;
	if (image != NULL && !image->is_dynamic) {
		vfs_file_t *file = image->file;
		if (file != NULL) {
			vfs_file_operations_t *fops = file->fops;
			if (fops == NULL && file->owner != NULL)
				fops = ((vfs_filesystem_t *)file->owner)->fops;
			if (fops != NULL && fops->close != NULL)
				fops->close(file);
		}
		// elf_image_free's NULL-table no-op contract is real now — kfree
		// grew its NULL guard the night this call first ran on a healthy
		// static image (2026-08-06; see kfree's comment for the story).
		elf_image_free(image);
	}

	kfree(t);
}

int task_reap_eligible_zombies(size_t max_to_reap)
{
	size_t reaped = 0;

	// Phase 2 FIRST: bury whatever last pass unlinked. These corpses have
	// been invisible to every walker for a full kworker period.
	task_t *corpse = kBurialList;
	kBurialList = NULL;
	while (corpse != NULL) {
		task_t *next = corpse->burialNext;
		task_destroy(corpse);
		reaped++;
		corpse = next;
	}

	// Phase 1: find newly collected corpses, unlink them from the deadChild
	// list and the kTaskList spine, park them for next pass.
	task_t *task = kTaskList;
	while (task != NO_TASK && task != NULL && reaped < max_to_reap) {
		task_t *child = task->deadChildHead;
		while (child != NULL && reaped < max_to_reap) {
			task_t *next_child = child->deadChildNext;
			bool collected = child->retValCollected || child->autoReap ||
			                 task->exited || child->parentTask == NULL;

			if (collected && task_threads_all_retired(child)) {
				printd(DEBUG_TASK | DEBUG_DETAILED,
					"task_reap_eligible_zombies: unlinking child task 0x%08x (%s), parent=0x%08x (%s), collected=%u autoReap=%u parentExited=%u\n",
					child->taskID,
					child->exename,
					task->taskID,
					task->exename,
					child->retValCollected,
					child->autoReap,
					task->exited);
				task_remove_dead_child(task, child);
				// The orphanage runs BEFORE the corpse leaves the public
				// lists: its children (live and dead) move to ktask now, so
				// no walker ever holds a parentTask aimed at a buried task.
				task_reparent_orphans(child);
				scheduler_remove_task(child);
				child->burialNext = kBurialList;
				kBurialList = child;
				// AN UNLINK IS WORK (fixed 2026-08-09; kworker.c always said
				// so — "buried/unlinked", both phases score 1 — but this
				// counter only ever saw phase 2). Without it a pass that
				// found nothing but FRESH corpses returned zero, kworker read
				// that as an idle pass, and slept its full 2s backstop nap
				// BETWEEN the two phases of its own burial. Burial latency
				// was therefore two naps, not one, and `watch -n 3 ps` could
				// still see the previous run's corpse — which is exactly how
				// this was caught. Counting here also gives max_to_reap an
				// honest job: a pass now does at most that many burial
				// OPERATIONS of either kind, alternating unlink and free
				// batches under load while never once sleeping on work.
				reaped++;
			}

			child = next_child;
		}
		task = task->next;
	}

	return (int)reaped;
}

// Kernel-context tail of task_exit(). It runs on the kernel interrupt stack with
// kKernelPML4 loaded, AFTER task_exit() has switched RSP/CR3. It's a SEPARATE
// function on purpose: task_exit() CALLs it right after the switch, so the call
// pushes onto the already-switched kernel stack and this function's prologue puts
// its rbp there — meaning every local below is accessed through a frame that IS
// mapped in kKernelPML4. Doing this work inline in task_exit() would (at -O0)
// access locals through the old task-stack rbp, which is not mapped after the
// CR3 switch. noinline is mandatory (inlining would defeat the whole point).
// Never returns.
// ── THE STACK-COLLISION FIX (2026-08-12): teardown split from the last breath ─
//
// task_exit used to switch onto the CURRENT CORE's kernel interrupt stack
// FIRST and then run the entire teardown there — siblings, tty, and
// handle_close_all with its honest disk I/O. A blocking close SLEEPS the
// thread mid-teardown; the wake requeues it RUNNABLE; and whichever core
// dispatches it next resumes the teardown STILL STANDING ON THE ORIGINAL
// CORE'S INTERRUPT STACK — while that core hands the very same stack to its
// next exiting task. Two live contexts, one fixed stack, each stomping the
// other's frames at deterministic offsets.
//
// That collision was THE stack poisoner (hunted 2026-08-11/12): top's %s
// pointer arriving as 3-then-4, mpAcctSettleAll's loop index reading -29874,
// the "kCPUInfo BAR poisoning" that was never the table, the 0x7ec6xxxx
// wrapped-write fatals — all one bug. Convicted by frozen-guest forensics on
// Chris's farm: CPU5 caught live, executing this very teardown with RSP
// inside CORE 0's interrupt stack while CLS[5] named its own, and the
// stomped settle frame six lines below it on the same page.
//
// The cure is structural: teardown is ORDINARY SYSCALL WORK and now runs as
// exactly that — on the thread's own kernel stack (which lives in the task's
// address space and travels with the thread through any sleep/wake/migration,
// like every other syscall's), under the task's CR3 (the shared upper half
// carries every kernel structure teardown touches). Only the LAST BREATH —
// zombie mark, scheduler trigger, halt — needs to outlive the task's address
// space, and THAT is all the per-core interrupt stack carries now: a handful
// of non-blocking, interrupts-disabled instructions with no window to
// migrate in and nothing worth stomping.

/// @brief Phase 1 of dying: everything blockable, in ordinary syscall context.
///
/// Runs on the calling thread's OWN kernel stack under the TASK's CR3 — it
/// may sleep in the VFS and wake on another core like any read() does, and
/// that is now safe by construction. Returns normally; task_exit then takes
/// the last breath. (This is the body that used to be task_exit_finish,
/// minus the finale.)
static void __attribute__((noinline)) task_exit_teardown(void)
{
	core_local_storage_t *cls = get_core_local_storage();

	thread_t *thread = cls ? cls->currentThread : NULL;
	task_t *task = cls ? cls->task : NULL;

	// task->retVal was already written by task_exit_with_retval (asm) before
	// any C code ran or any stack switch occurred.  We just propagate it to
	// thread->retVal here.
	//
	// `exiting`, NOT `exited` — and this ordering is the whole fix (2026-08-09,
	// Chris ran it to ground). `exited` is what the scheduler's take-off-CPU
	// branch reads to file a thread under ZOMBIE, and a zombie is never
	// scheduled again. Setting it HERE announced "safe to bury me" while this
	// thread still had the entire task teardown in front of it: siblings to
	// mark, every handle to close (VFS closes, disk I/O — a wide window), and
	// only THEN `task->exited = true`. One timer tick anywhere in that stretch
	// and the scheduler zombied the thread mid-teardown; it never ran again,
	// so task->exited stayed false FOREVER. The task was dead to the scheduler
	// and alive to everyone waiting on it.
	//
	// Symptoms this produced: elf_loader "task did not exit within 5 seconds"
	// on ~30% of SCHED=periodic boots, permanent Z corpses in `ps`, and a
	// debugger that could never step into task_terminate_sibling_threads
	// because the thread was already zombied before it got there. Invisible
	// under tickless for the reason everything else is: an AP that never
	// preempts runs this path to completion atomically. It is NOT a periodic
	// bug — periodic just rolls the dice 100 times a second.
	//
	// `exiting` carries the other half of the old flag's meaning: "already on
	// the way out", which is what the forced-syscall redirect and the sibling
	// sweep actually want to know. `exited` is now set exactly once, in the
	// breath before scheduler_trigger, matching the pattern SYSCALL_THREAD_EXIT
	// already got right ("MARK, don't move" — syscall.c).
	if (thread) {
		thread->exiting = true;
		thread->retVal = task ? task->retVal : 0;
	}

	// ALREADY DYING? Then this is a sibling arriving through the exit
	// trampoline, not the task's first death.
	//
	// The scheduler's forced-syscall redirect points a doomed thread at
	// TASK_EXIT_TRAMPOLINE_VIRT, which calls the TASK exit syscall — so
	// killing a four-worker hog sends five threads down this path, not one.
	// Without this guard each of them would close the handle table again and
	// re-enqueue an already-enqueued dead child, which is how a zombie list
	// gets corrupted. A thread that finds its task already dead has exactly
	// one job left: mark itself and get off the CPU.
	if (task != NULL && task->exited)
	{
		printd(DEBUG_TASK | DEBUG_THREAD,
		       "task_exit: thread 0x%08lx arrived after %s already died — retiring the thread only\n",
		       thread ? thread->threadID : 0, task->exename);
		// Nothing left to tear down — return to task_exit, whose last breath
		// marks `exited` and gets off the CPU. (This used to trigger and halt
		// right here; the finale is one shared doorway now.)
		return;
	}

	if (task) {
		// SIBLINGS FIRST, before the handles go. "Exit means exit" (Chris's
		// ruling, 2026-08-02): when a task dies its threads die with it, and
		// they must be TOLD before their handles are pulled out from under
		// them — a worker mid-read on a pipe whose end just closed underneath
		// it is a race, and telling it to die first makes the ordering
		// honest. They will not all be gone by the time we return; each dies
		// at its own next boundary, in its own context, which is the whole
		// design (see task_terminate_sibling_threads).
		task_terminate_sibling_threads(task, thread);

		// Release every handle this task still holds — BEFORE it is enqueued as
		// a dead child. For a pipe end this is the refcount that decides EOF /
		// EPIPE, so death must give the ends back: a task that dies (or crashes)
		// still holding the write end of a pipe would leave its reader blocked
		// forever on an EOF that can never come. Dying is just another way of
		// closing your handles.
		printd(DEBUG_TASK, "task_exit: %s releasing its handles\n", task->exename);
		handle_close_all(task);

		task->exited = true;

		// If the departed was a terminal's seated shell, the terminal goes
		// dormant and posts its summons (tty.c) — the next keystroke there
		// raises a fresh husk. Checked by tty.c against the SEAT (t->shell),
		// so an ordinary child dying on a terminal changes nothing.
		tty_shell_departed(task);

		task_enqueue_dead_child(task);
	}

	// Teardown complete. `exited` is NOT set here anymore — it is the last
	// breath's job (task_exit_last_breath), the one-way door armed only after
	// this thread has left the task's stack for good.
}

/// @brief Phase 2 of dying: the last breath, on the per-core interrupt stack.
///
/// Runs with interrupts DISABLED, under the kernel CR3, and does exactly
/// three non-blocking things: arm the one-way door (`exited` — the
/// take-off-CPU branch in scheduler.c reads it and a zombie is never
/// scheduled again), trigger the scheduler, and halt until it takes the
/// corpse. NOTHING blockable is permitted on this stack — the stack belongs
/// to the CORE, not the thread, and a context that sleeps here wakes on some
/// other core still holding it. That was the stack poisoner; see the fix
/// note above task_exit_teardown.
static void __attribute__((noreturn, noinline)) task_exit_last_breath(void)
{
	// GS-based, valid regardless of RSP/CR3; frame is now on the kernel stack.
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *thread = cls ? cls->currentThread : NULL;

	// RETIRED — and not one instruction sooner. Every consumer of this task's
	// death has been served by the teardown (siblings told, handles closed,
	// task->exited published, corpse enqueued), so there is genuinely nothing
	// left for this thread to run and the scheduler may file it under ZOMBIE.
	if (thread)
		thread->exited = true;

	// Enter the scheduler via its normal APIC-IPI path; it sees the exited
	// flag set above and moves this thread to the zombie queue.  A dead
	// thread is never rescheduled, so trigger's wait-loop (and the belt-and-
	// suspenders hlt loop below) should never actually run to completion.
	// The sti in the loop is the FIRST moment interrupts come back on — with
	// the door already armed, the scheduler's next visit is the burial.
	scheduler_trigger(cls);

	while (1==1)
	{
		__asm__("sti\nhlt\n");
	}
}

void task_exit(void)
{
	// PHASE 1 — the whole blockable teardown, in ordinary syscall context on
	// this thread's own kernel stack. It may sleep in the VFS and resume on
	// any core; its stack travels with it, like every other syscall's.
	task_exit_teardown();

	// PHASE 2 — leave the task behind. Interrupts OFF before we so much as
	// READ which core we are on: a preemption between choosing this core's
	// interrupt stack and standing on it would let the thread migrate and
	// stand on the WRONG core's stack — a one-instruction revival of the very
	// bug this split fixes. They stay off until the last breath's final
	// sti/hlt, by which point the one-way door is armed.
	__asm__ volatile("cli");

	core_local_storage_t *cls = get_core_local_storage();

	// We're on the task-side stack with the task's CR3 loaded; that stack
	// lives at a task-local lower-half VA that is NOT mapped in kKernelPML4.
	// Read the kernel interrupt stack top from CLS (via GS, valid under any
	// CR3) while the task CR3 is still loaded, and 16-align it so the `call`
	// below keeps the SysV stack alignment (rsp%16==8 at the callee's entry).
	uintptr_t kernel_rsp = (cls->kernel_interrupt_stack_top - 16) & ~(uintptr_t)0xF;

	// Switch RSP to the kernel stack, switch CR3 to kKernelPML4, then IMMEDIATELY
	// call the continuation — one asm block, NOTHING in between. This is the crux:
	// at -O0 rbp still points at the old task stack after the switch, so any C
	// statement here would read/write an unmapped (or wrong) physical page. The
	// `call` touches no C local — it pushes the return address onto the
	// already-switched kernel stack, and task_exit_last_breath() builds its frame
	// there. The callee is passed as an operand (indirect call) so we don't lean
	// on an assembler symbol name. Never returns.
	__asm__ volatile(
		"mov rsp, %0\n\t"
		"mov cr3, %1\n\t"
		"call %2\n\t"
		:
		: "r"(kernel_rsp), "r"((uint64_t)kKernelPML4), "r"(task_exit_last_breath)
		: "memory");

	__builtin_unreachable();
}

// Backstop sleep for task_wait: a child exit normally wakes the parent
// immediately (task_enqueue_dead_child), but the backstop guarantees liveness
// if that wake is ever lost to the classic check-then-sleep race — the parent
// re-checks its dead children within a second regardless. (Same discipline as
// console_read.)
#define TASK_WAIT_BACKSTOP_TICKS TICKS_PER_SECOND

// Find the first dead child matching targetPid (0 = any) on the parent's
// list, or NULL if none match. This used to be a POP — collectors unlinked
// the corpse themselves — but that orphaned every collected corpse from the
// reap machinery: off the deadChild list, kworker's sweep could never see it
// again, and its task struct sat on kTaskList forever (the 49-zombie
// graveyard, 2026-08-06). Now collectors only LOOK and mark; the corpse
// stays enqueued, and the undertaker (task_reap_eligible_zombies, kworker
// context, the only burial site) does all unlinking itself.
static task_t *task_find_dead_child(task_t *parent, uint64_t targetPid)
{
	for (task_t *child = parent->deadChildHead; child != NULL; child = child->deadChildNext)
	{
		// A corpse can be collected exactly ONCE. It stays on this list until
		// kworker's next sweep unlinks it (up to ~2s), and without this skip a
		// shell's every-prompt reap poll would report the same "[1] Done" at
		// every prompt until the undertaker caught up.
		if (child->retValCollected)
			continue;
		if (targetPid == 0 || child->taskID == targetPid)
			return child;
	}
	return NULL;
}

// Find a LIVE child matching targetPid (0 = any), or NULL. Two callers, two
// jobs: task_wait uses NULL-ness to fail fast when the caller asks for a child
// that doesn't exist / was already reaped, and uses the task itself to hand
// the console over (the child being waited on IS the foreground task).
static task_t *task_find_live_child(task_t *parent, uint64_t targetPid)
{
	for (task_t *t = kTaskList; t != NULL && t != (task_t*)NO_TASK; t = t->next)
		if (t->parentTask == parent && !t->exited)
			if (targetPid == 0 || t->taskID == targetPid)
				return t;
	return NULL;
}

uint64_t task_wait(task_t* parentTask, uint64_t targetPid, uint64_t* exitCode)
{
	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = parentTask ? parentTask : (cls ? cls->task : NULL);

	if (parent == NULL || parent->threads == NULL) {
		return 0;
	}

	// The console changes hands HERE — the foreground task is by definition
	// "the task the controlling shell is currently blocked waiting on." Keyed
	// on wait, not spawn, so a future backgrounded (&) child never takes the
	// console. Restored to the shell on EVERY return path below: a Ctrl+C at
	// the prompt after this wait must find the shell foreground again (where
	// it is a harmless line-kill byte), never a stale pointer at a dead child.
	//
	// THE console is now THIS SHELL'S TERMINAL (task_tty): husk-on-tty2
	// handing its console to a child moves tty2's foreground pointer and
	// nobody else's — the Ctrl+C you type on tty1 cannot reach across.
	tty_t *console = task_tty(parent);
	bool movesConsole = parent->controllingShell;
	if (movesConsole) {
		task_t *fg = task_find_live_child(parent, targetPid);
		if (fg != NULL)
			console->fgTask = fg;
	}

	while (1==1)
	{
		// Check FIRST: an already-dead matching child returns immediately, no
		// sleep (the "don't wait if the child already ended" rule).
		task_t *child = task_find_dead_child(parent, targetPid);
		if (child != NULL) {
			// Copy EVERYTHING out before certifying: the moment
			// retValCollected goes true, kworker's next sweep may unlink and
			// free this struct — the certificate must be our LAST touch.
			// (The thread dequeue that used to happen here moved into
			// task_destroy: burial is one act, at one site, in one context.)
			uint64_t endedPid = child->taskID;
			if (exitCode != NULL) {
				*exitCode = child->retVal;
			}
			child->retValCollected = true;
			if (movesConsole)
				console->fgTask = parent;
			return endedPid;
		}

		// No dead match. If there is no matching LIVE child either, there is
		// nothing to wait for — fail rather than sleep forever.
		if (task_find_live_child(parent, targetPid) == NULL) {
			if (movesConsole)
				console->fgTask = parent;
			return 0;
		}

		// Park until a child exits (woken by task_enqueue_dead_child) or the
		// backstop fires; then loop and re-check. SIGSLEEP parks atomically.
		parent->waitingForChild = true;
		sigaction(SIGSLEEP, NULL, kTicksSinceStart + TASK_WAIT_BACKSTOP_TICKS, parent->threads);
		parent->waitingForChild = false;
	}
}

// The non-blocking half of task_wait (contract in task.h). Deliberately shares
// task_wait's check-first block and NOTHING else: no live-child probe, because
// "children exist but none are dead" is a normal answer here rather than a
// reason to sleep; and no foreground movement, because reaping a
// background corpse does not hand anyone the console.
uint64_t task_reap_any_dead(task_t* parentTask, uint64_t* exitCode)
{
	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = parentTask ? parentTask : (cls ? cls->task : NULL);

	if (parent == NULL)
		return 0;

	// targetPid 0 = "the first of ANY child to have ended", the same wildcard
	// task_wait uses — a shell polling for finished jobs does not care which.
	task_t *child = task_find_dead_child(parent, 0);
	if (child == NULL)
		return 0;

	// Same copy-then-certify discipline as task_wait: after the certificate,
	// this struct belongs to the undertaker.
	uint64_t endedPid = child->taskID;
	if (exitCode != NULL)
		*exitCode = child->retVal;
	child->retValCollected = true;

	return endedPid;
}

task_t* task_initialize(task_t* parentTask, bool kernelTask, bool idleTask, uint64_t pinnedAPICId)
{
    printd(DEBUG_TASK,"task_initialize: Initializing task\n");

	task_t* newTask = kmalloc_aligned(sizeof(task_t));
    printd(DEBUG_TASK,"task_initialize: Malloc'd 0x%016x for new task\n",newTask);

    // (Idle tasks used to inherit their parent's PML4 here — see the address
    // space note below for why that had to stop.)
    newTask->parentTask = parentTask;
    newTask->priority = TASK_DEFAULT_PRIORITY;

	newTask->mmaps = kmalloc(sizeof(dlist_t));
	if (newTask->mmaps) {
		dlist_init(newTask->mmaps);
	}
	// Lazily created on first use by task_map_shared_object — most tasks
	// never touch dynamic linking, so most never allocate this at all.
	newTask->shared_objects = NULL;

	// ── ONE TASK, ONE ADDRESS SPACE ─────────────────────────────────────────
	// ktask (the first kernel task) uses kKernelPML4 directly, because it IS
	// the kernel's own context. EVERY other task — kernel, idle, or ring 3 —
	// gets its OWN PML4 with the upper half shared.
	//
	// Idle tasks used to be an exception: they inherited ktask's PML4, on the
	// reasoning that a thread which only ever runs `hlt` hardly needs an
	// address space of its own. That made ktask and all N idle tasks share ONE
	// address space, and task_create hands every task it builds a set of
	// FIXED lower-half virtual addresses — TASK_ARGV_VIRT above all. Nine
	// tasks mapping their own private blob at the same fixed VA in the same
	// page table means eight of those mappings are silently destroyed; the
	// last idle task created wins, and every one of the nine then reads ITS
	// argv. (Chris found this on 2026-07-25 the first morning /proc existed:
	// `cat /proc/32/cmdline` on ktask answered "/idle7". Harmless only by
	// luck — kernel tasks load no ELF, so task_setup_entry never runs and
	// nothing ever READ the clobbered mapping.)
	//
	// The half-fix was already in the tree and is worth understanding before
	// touching this: task_alloc_aligned and task_reserve_task_virt route
	// shared-PML4 tasks to a SHARED VA counter, precisely so their stacks and
	// arenas could not collide the same way. That patch works and covers
	// nothing that is a hardcoded constant rather than a counter draw — which
	// is exactly what TASK_ARGV_VIRT, TASK_ENV_VIRT and
	// TASK_EXIT_TRAMPOLINE_VIRT are. Rather than teach three more constants to
	// dodge, remove the sharing: with one address space per task the fixed VAs
	// are unambiguous by construction, and any future per-task fixed address
	// is safe without anybody having to remember this comment.
	//
	// Cost: one PML4 page per idle task, and a CR3 reload when a core switches
	// to its idle thread — which scheduler.S already elides when the value is
	// unchanged (its restore path compares before loading), so this only ever
	// costs a reload that genuinely crosses address spaces.
	if (kKernelTask == NULL && kernelTask)
	{
		// This is ktask - use the kernel PML4 directly
		newTask->pml4v = (uint64_t*)kKernelPML4v;
		newTask->pml4 = (uint64_t*)kKernelPML4;
		newTask->taskMemoryNextVirt = KERNEL_TASK_MEMORY_BASE;
		printd(DEBUG_TASK | DEBUG_DETAILED, "task_initialize: ktask using kKernelPML4 directly\n");
	}
	else
	{
		// Allocate new PML4 for this task
		newTask->pml4v = (uintptr_t*)get_paging_table_pageV();
		newTask->pml4 = (uintptr_t*)((uintptr_t)newTask->pml4v & ~(kHHDMOffset));

		// Clear the new PML4
		memset(newTask->pml4v, 0, PAGE_SIZE);

		// Copy upper-half PML4 entries (256-511) from kKernelPML4
		// This shares the kernel page table structures (not the data, just the pointers)
		uintptr_t* kernelPML4 = (uintptr_t*)kKernelPML4v;
		for (int i = 256; i < 512; i++) {
			newTask->pml4v[i] = kernelPML4[i];
		}

		newTask->taskMemoryNextVirt = kernelTask ? KERNEL_TASK_MEMORY_BASE : USER_TASK_MEMORY_BASE;
		printd(DEBUG_TASK | DEBUG_DETAILED, "task_initialize: Allocated new PML4 at 0x%lx for %s%s task (shared upper-half)\n",
			newTask->pml4, idleTask ? "idle " : "", kernelTask ? "kernel" : "user");
	}

	newTask->threads = createThread((void*)newTask, kernelTask);
	newTask->threads->idleThread = idleTask;
	newTask->threads->mp_apic = pinnedAPICId;
	printd(DEBUG_TASK | DEBUG_DETAILED,
		"task_initialize: thread 0x%08x affinity set to %s0x%08lx\n",
		newTask->threads->threadID,
		pinnedAPICId == THREAD_NO_AFFINITY ? "THREAD_NO_AFFINITY/" : "",
		pinnedAPICId);
	newTask->taskID = newTask->threads->threadID;
	newTask->exited = false;
	newTask->autoReap = false;

	// Note: TASK_STRUCT_VADDR mapping removed - it was unused
	// task_t lives in kernel heap (HHDM space) and doesn't need fixed virtual mapping

	return newTask;
}

// Maps every segment of a loaded shared_object_t (main executable or
// library, both go through the same registry — see shared_object.c) into
// `task` as demand-paged VMAs — no physical pages are mapped here at all.
// A task that never calls into a given page of a library never pays for it;
// the first real touch (from ANY task using this object) faults, and
// shared_object_resolve_page (called from simple_exceptions.c) resolves and
// caches that page for every task after it. See MAP_SHARED_LIBRARY in
// memory/vma.h for how the fault handler recognizes these VMAs.
//
// Writable segments get `vma->cow = true`, same as before: the physical
// page a fault resolves to may be shared with other tasks, so the first
// WRITE (a completely separate trigger from the first READ/resolve) must
// still go through the *existing, unmodified* CoW fault handler to
// privatize a copy. Nothing about that handler needed to change for this.
//
// Returns false (mapping nothing) if `so` is already in this task's
// shared_objects list — the dependency closure below is a graph, not a
// tree, so the same object can be reached twice (diamond dependencies, or
// a dependency cycle) and must only be mapped once per task.
static bool task_map_shared_object(task_t *task, shared_object_t *so)
{
	if (task->shared_objects == NULL) {
		task->shared_objects = kmalloc(sizeof(dlist_t));
		if (task->shared_objects == NULL) {
			panic("task_map_shared_object: failed to allocate shared_objects list for %s", so->path);
		}
		dlist_init(task->shared_objects);
	}

	// Already mapped into this task? (dedup + cycle guard for the closure
	// walk below — checked BEFORE creating VMAs so a revisit maps nothing.)
	for (dlist_node_t *node = task->shared_objects->head; node != NULL; node = node->next) {
		if ((shared_object_t *)node->data == so) {
			return false;
		}
	}

	for (size_t i = 0; i < so->seg_count; i++) {
		elf_segment_range_t *seg = &so->segs[i];
		uintptr_t virt = so->load_bias + seg->vaddr_off;
		bool writable = (seg->prot & PROT_WRITE) != 0;

		vma_t *vma = vma_create(virt, virt + seg->pages * PAGE_SIZE, seg->prot,
		                         MAP_SHARED_LIBRARY, (void *)so, 0);
		if (vma == NULL) {
			panic("task_map_shared_object: failed to create VMA for %s segment %lu", so->path, i);
		}
		vma->cow = writable;
		vma_add(task, vma);
	}

	dlist_add(task->shared_objects, so);
	return true;
}

// Maps `so` and its ENTIRE dependency closure (so->deps, recursively) into
// `task`. Mapping the whole closure — not just the direct DT_NEEDED list —
// is what upholds the invariant shared_object.c's scoped resolver depends
// on: any address a cached, relocated page can reference lives inside the
// owning object's dependency closure, so every task sharing that page must
// have that closure mapped. task_map_shared_object's already-mapped check
// terminates diamonds and cycles.
static void task_map_shared_object_closure(task_t *task, shared_object_t *so)
{
	if (!task_map_shared_object(task, so)) {
		return;  // already mapped — its deps were mapped along with it
	}
	for (size_t i = 0; i < so->dep_count; i++) {
		task_map_shared_object_closure(task, so->deps[i]);
	}
}

// Loads a dynamically-linked executable: maps the executable itself and
// its full dependency closure (each object loaded at most once system-wide,
// physically shared across every task that needs it — see
// shared_object_load_or_get, which also loads DT_NEEDED dependencies
// recursively) as demand-paged VMAs. No relocations are applied here — that
// happens lazily, per page, the first time any task's page fault touches
// that page (shared_object_resolve_page), resolving symbols against each
// object's own dependency scope. Mutually exclusive with
// elf_load_from_path's static path — task_create picks one or the other up
// front via elf_is_dynamic().
static void elf_resolve_dynamic_dependencies(task_t *task, const char *path)
{
	shared_object_t *main_so = shared_object_load_or_get(path);
	if (main_so == NULL) {
		// Covers open/parse/allocation failure AND a non-ET_DYN image — a
		// dynamically-linked non-PIE (ET_EXEC) binary would need e_entry
		// and its p_vaddr values treated as already-absolute rather than
		// load_bias-relative, which the shared fixed-address window can't
		// express; shared_object_load_or_get rejects those for every image
		// (main executable and libraries alike).
		panic("elf_resolve_dynamic_dependencies: failed to load %s (missing, malformed, or not ET_DYN)", path);
	}

	task_map_shared_object_closure(task, main_so);

	task->elf = main_so->image;
	// ET_DYN: e_entry is load_bias-relative, same as every other address in
	// the image (unlike ET_EXEC, where it would already be absolute).
	task->loadBias = main_so->load_bias;
	task->entryPoint = main_so->load_bias + main_so->image->ehdr.e_entry;
	if (task->threads != NULL) {
		task->threads->regs.RIP = task->entryPoint;
	}
}

// GDB symbol-autoload notch.  utility/os64_symbols.gdb plants a SILENT
// breakpoint on debug_task_loaded(), reads the two globals below, runs
// add-symbol-file for the matching binary at the right offset, and resumes
// without stopping — so every program's debug info appears in GDB the moment
// the kernel loads it.
//
// CORRECTION (this comment used to claim the autoloader "replaces the old-OS
// trick of giving every executable a unique link address, with all binaries
// free to share the same link address").  THAT WAS WRONG, and pipelines proved
// it: `hello | upper` runs two programs AT ONCE, and when both link at 0x400000
// they occupy the same linear addresses — GDB has one global symbol table keyed
// by address, so it cannot tell them apart.  The autoloader could only cope by
// unloading one app's symbols to load the other's, which is useless when you
// need to step across a pipeline.
//
// So the unique-link-address trick is BACK, and it was right all along: every
// app now links at its own base (userland/tools/app_bases.py, derived from a
// hash of the app's name), and this autoloader ACCUMULATES symbols instead of
// swapping them.  The proper fix — PIE with a loader-assigned per-TASK bias,
// which also handles the same program running twice in one pipeline — is in
// DEBTS; the bias plumbing below is already in place for it.
//
// The info travels through GLOBALS (kernel .bss — mapped identically in every
// address space, trivially readable by the gdbstub) rather than function
// arguments: the first version passed the kmalloc'd path pointer as an arg
// and GDB's frame-argument read failed with "Cannot access memory" — frame
// timing, CR3, and heap-mapping subtleties all conspire against argument
// parsing, and none of them apply to a fixed-address kernel array.
// noinline + the asm sliver keep any -O level from eliding the call or the
// symbol.  Runs (and costs a string copy) whether or not a debugger is
// attached.
char kDebugTaskLoadedPath[TASK_MAX_PATH_LEN];
uint64_t kDebugTaskLoadedBias;
void __attribute__((noinline)) debug_task_loaded(void)
{
	__asm__ volatile("" :: "r"(kDebugTaskLoadedPath), "r"(&kDebugTaskLoadedBias) : "memory");
}

// Wire up the ring-3 EXIT path for a user task.  Counterpart of the ring0
// branch in createThread() (which seeds task_exit_with_retval directly —
// impossible for ring 3, where returning into kernel text faults).
//
// Two steps, both prescribed by the 32-bit OS's proven design (processExit):
//   1. Copy the position-independent trampoline template (task_exit_asm.S)
//      into a fresh page and map it into the task at TASK_EXIT_TRAMPOLINE_VIRT,
//      user-visible but READ-ONLY (PAGE_USER without PAGE_WRITE, executable) —
//      the program can run its exit path but not scribble on it.
//   2. Seed the trampoline's VA as the initial return address at [regs.RSP]
//      (createThread left that qword slot for us).  A _start that plainly
//      `ret`s pops it and lands in the trampoline at CPL 3, which converts
//      RAX into an exit syscall — same retVal contract as ring0 tasks.
//
// The seed is written through the HHDM alias of the stack's physical page
// (same idiom as the ring0 seeding in createThread) because the user stack VA
// is only mapped in the task's own PML4, not the kernel's.
extern const char user_exit_trampoline_template[];
extern const char user_exit_trampoline_template_end[];
static void task_setup_ring3_exit_path(task_t *task)
{
	if (task->threads == NULL)
		return;

	size_t template_bytes = (size_t)(user_exit_trampoline_template_end - user_exit_trampoline_template);

	// kmalloc_aligned hands back a zeroed, page-aligned HHDM address; the byte
	// copy is tiny (a handful of instructions) so one page is plenty.
	void *trampoline_page = kmalloc_aligned(PAGE_SIZE);
	memcpy(trampoline_page, user_exit_trampoline_template, template_bytes);

	uintptr_t trampoline_phys = (uintptr_t)trampoline_page - kHHDMOffset;
	paging_map_pages(task->pml4v, TASK_EXIT_TRAMPOLINE_VIRT, trampoline_phys, 1,
	                 PAGE_PRESENT | PAGE_USER);

	uintptr_t phys_rsp = paging_walk_paging_table((pt_entry_t*)task->pml4v, task->threads->regs.RSP);
	if (phys_rsp && phys_rsp != 0xbadbadba) {
		*(uintptr_t *)(phys_rsp | kHHDMOffset) = (uintptr_t)TASK_EXIT_TRAMPOLINE_VIRT;
	} else {
		panic("task_setup_ring3_exit_path: user stack VA 0x%016lx not mapped in task PML4 for %s\n",
		      task->threads->regs.RSP, task->exename);
	}
}

// Wire up the user-space entry registers after the ELF is loaded.
// Called by task_create (and in future by task_exec) once elf_load_from_path
// has set regs.RIP and task->entryPoint.
static void task_setup_entry(task_t *task)
{
	if (task->threads == NULL)
		return;

	// argc → RDI
	task->threads->regs.RDI = (uint64_t)task->argc;

	// argv → RSI  (already mapped at TASK_ARGV_VIRT by task_create)
	task->threads->regs.RSI = (task->argc > 0) ? TASK_ARGV_VIRT : 0;

	// env → RDX  (map env page(s) read-only into task's address space first)
	if (task->env != NULL) {
		uintptr_t env_phys = (uintptr_t)task->env - kHHDMOffset;
		paging_map_pages(task->pml4v, TASK_ENV_VIRT, env_phys,
		                 task->env->page_count,
		                 PAGE_PRESENT | PAGE_USER);
		task->threads->regs.RDX = TASK_ENV_VIRT;
	} else {
		task->threads->regs.RDX = 0;
	}
}

task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID)
{
	uintptr_t mapPages;
	// The kernel's built-in tasks are recognized by the START of their path —
	// they carry no ELF image, so a match means "skip the loader, this task's
	// code is already compiled into the kernel."
	//
	// ANCHORED PREFIX, and both halves of that phrase were learned the hard
	// way on 2026-08-01:
	//
	//   * These were strnstr() SUBSTRING searches, which match ANYWHERE in
	//     the path — and "/bin/logd" contains "/logd". So the first userland
	//     program ever named `logd` was silently mistaken for the kernel
	//     daemon: no ELF loaded, no entry point, and the scheduler's iretq
	//     into the resulting garbage selectors surfaced as a #GP naming a
	//     task that had never run an instruction. The same trap was armed
	//     for /bin/idle, /bin/kworker, /bin/guicomp, /bin/gbounce, /bin/gkeys.
	//
	//   * But an EXACT compare is too strict, because these names carry
	//     suffixes: the idle tasks are "/idle0".."/idleN" and the kworker is
	//     "/kworker1". Exact matching stopped recognizing them, task_create
	//     tried to load them as ELF files, returned NULL, and the caller
	//     dereferenced it — a NULL page fault three lines into the boot.
	//
	// Anchored prefix matching is the shape that was actually intended all
	// along: it must match at position 0, and it may match a family.
	bool isIdleTask    = (path != NULL && strncmp(path, "/idle", 5) == 0);
	bool isLogdTask    = (path != NULL && strncmp(path, "/logd", 5) == 0);
	bool isKWorkerTask = (path != NULL && strncmp(path, "/kworker", 8) == 0);
	bool isGuiCompTask = (path != NULL && strncmp(path, "/guicomp", 8) == 0);
	bool isGBounceTask = (path != NULL && strncmp(path, "/gbounce", 8) == 0);
	bool isGKeysTask   = (path != NULL && strncmp(path, "/gkeys", 6) == 0);
	// Set when we actually load an ELF image below, so we know to latch the ELF
	// entry registers (argc/argv/env) later — AFTER those fields are populated.
	bool loadedElfProgram = false;

	// CAN WE EVEN RUN THIS? Ask BEFORE task_initialize allocates a thing.
	//
	// A path we can't load is not a kernel error — it is just "no". This used to
	// PANIC further down ("task_create: Failed to load ELF"), which meant RING 3
	// COULD KILL THE KERNEL WITH A TYPO: one fat-fingered filename at the husk
	// prompt took the whole OS down. Spawning a file that exists but isn't a
	// program (/partition_info) did it too.
	//
	// Checking here, before anything is allocated, is what makes the failure
	// clean: there is no task-teardown path in this tree to unwind with (see
	// DEBTS), so the only way to fail without leaking is to fail before we
	// build. spawn() now returns -1 and husk prints "cannot run <path>".
	if (!isIdleTask && !isLogdTask && !isKWorkerTask && !isGuiCompTask &&
	    !isGBounceTask && !isGKeysTask && kRootFilesystem != NULL)
	{
		if (!elf_can_load(path))
		{
			printd(DEBUG_TASK, "task_create: cannot load '%s' — not spawning\n", path);
			return NULL;
		}
	}

	task_t* newTask = task_initialize(parentTaskPtr, isKernelTask, isIdleTask, pinnedAPICID);

    //Copy the path (parameter) value from the parentTask's memory.
    newTask->path=kmalloc(TASK_MAX_PATH_LEN); 
	strncpy(newTask->path,path,TASK_MAX_PATH_LEN);

	    printd(DEBUG_TASK,"task_create: Creating %s task for %s\n",isKernelTask?"kernel":"user",newTask->path);
	printd(DEBUG_TASK | DEBUG_DETAILED,
		"task_create: path=%s pinnedAPICID=%s0x%08lx idle=%u logd=%u kworker=%u guicomp=%u\n",
		newTask->path,
		pinnedAPICID == THREAD_NO_AFFINITY ? "THREAD_NO_AFFINITY/" : "",
		pinnedAPICID,
		isIdleTask,
		isLogdTask,
		isKWorkerTask,
		isGuiCompTask);

    // exename is the basename of the path: walk to the LAST slash, then step
    // past it so we store "husk", not "/husk". (A path with no slash at all
    // leaves slash2 pointing at the whole path, which is already the name.)
    char *slash=newTask->path, *slash2=newTask->path;
    while (slash!=NULL)
    {
        slash = strstr(slash2+1, "/");
        if (slash)
            slash2 = slash;
    }
    if (*slash2 == '/')
        slash2++;
    strcpy(newTask->exename, slash2);
	printd(DEBUG_TASK, "task_create: Executable name is %s\n", newTask->exename);

	if (isIdleTask)
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&task_idle_loop;
	}

	if (isLogdTask)
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&logd_thread;
	}

	if (isKWorkerTask)
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&kworker_thread;
	}

	if (isGuiCompTask)
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&guicomp_thread;
	}

	if (isGBounceTask)
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&gbounce_thread;
	}

	if (isGKeysTask)
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&gkeys_thread;
	}

	if (!isIdleTask && !isLogdTask && !isKWorkerTask && !isGuiCompTask && !isGBounceTask && !isGKeysTask && kRootFilesystem != NULL)
	{
		if (elf_is_dynamic(newTask->path)) {
			elf_resolve_dynamic_dependencies(newTask, newTask->path);
		} else if (elf_load_from_path(newTask, newTask->path) != 0) {
			// The header already validated (elf_can_load, above), so reaching
			// here means the load failed MID-WAY — a truncated image, a bad
			// program header, an allocation failure. Still not worth killing the
			// OS over: return failure and let the caller say "cannot run".
			//
			// NOTE: this leaks newTask (struct + PML4 + stacks). There is no
			// task teardown path in the tree yet — DEBTS row filed. A leak on a
			// corrupt-binary edge case beats a panic on it, and the common cases
			// (typo, not-a-program) never get this far.
			printd(DEBUG_TASK, "task_create: ELF load failed for %s after its header validated "
				"(truncated or malformed image?) — not spawning\n", newTask->path);
			return NULL;
		}
		// NOTE: entry registers (RDI/RSI/RDX) are latched later, after argc/argv
		// are built and mapped and env is inherited. Calling task_setup_entry()
		// here would read a still-zeroed argc/argv/env and hand the program
		// argc=0, argv=NULL, env=NULL.
		loadedElfProgram = true;
	}

	gmtime((time_t*)&kSystemCurrentTime,&newTask->startTime);

	//Initialize the heap at 0 bytes
    newTask->heapStart=TASK_HEAP_START;
    newTask->heapEnd=TASK_HEAP_START;

	if (parentTaskPtr != NULL)
    {
       newTask->parentTask=parentTaskPtr;
       newTask->stdin=parentTaskPtr->stdin;
       newTask->stdout=parentTaskPtr->stdout;
       newTask->stderr=parentTaskPtr->stderr;
       // The controlling terminal rides the family line: a shell's children
       // work its terminal (read its ring, print to its grid, answer its
       // Ctrl+C). NULL inherits as NULL = the system console (task_tty).
       // tty_seat_shell overrides this for the shells themselves.
       newTask->tty=parentTaskPtr->tty;
       //Initialize the current working directory to parentTask's cwd
       newTask->cwd=(char*)kmalloc(PAGE_SIZE);
       if (parentTaskPtr!=NULL && parentTaskPtr->cwd)
           //Initialize the current working directory to parentTask's cwd
           strncpy(newTask->cwd, parentTaskPtr->cwd, TASK_MAX_PATH_LEN);
	}
	else
	{
        //Initialize the current working directory and set it to '/'
       newTask->cwd=(char*)kmalloc(PAGE_SIZE);
       strcpy(newTask->cwd,"/");
       newTask->kernelTask=isKernelTask;
       newTask->stdin=STDIN;
       newTask->stdout=STDOUT;
       newTask->stderr=STDERR;
	}

	// INVARIANT: every task's cwd is a valid absolute path, no exceptions.
	// ktask is created with a DUMMY stack-local parent whose cwd is NULL, so
	// the inheritance branch above skipped the copy and left ktask's cwd an
	// empty string — which every task in the system then inherited, since
	// ktask is everyone's ancestor (getcwd returned "" — caught by cwd_test
	// step 1 on the cwd slice's very first boot). Rootless tasks live at /.
	if (newTask->cwd != NULL && newTask->cwd[0] == '\0')
		strcpy(newTask->cwd, "/");

	// Every task is born with a fresh handle table: 0/1/2 wired to the console.
	//
	// NOTE we do NOT blanket-inherit the parent's handles the way Unix does.
	// A child gets the console, plus whatever spawn() explicitly redirects into
	// slots 0/1/2 — nothing else. That is simpler AND safer: a child can never
	// accidentally inherit some unrelated pipe end and keep it open, which is
	// one of the classic ways a pipeline hangs. What the child gets is exactly
	// what the parent asked for.
	handle_table_init(newTask);

	//Argument handling.
	//Every task has at least argv[0] (its own path), so if the caller passed no
	//arguments we synthesize argc=1 with argv[0]=path.  We build a single blob:
	//   [ (argc+1) pointer slots, NULL-terminated ][ argc fixed-size string slots ]
	//and map it at TASK_ARGV_VIRT.  Two things matter for correctness:
	//  1. Each string is copied into the blob's OWN slots (never left pointing at
	//     the caller's argv memory, which the old code did — corrupting the caller
	//     and handing the program dangling pointers).
	//  2. The pointer slots hold TASK-space addresses (TASK_ARGV_VIRT + offset), not
	//     kernel addresses, so the program sees a self-consistent argv inside its
	//     own address space once the blob is mapped.
	int effectiveArgc = (argc > 0) ? argc : 1;
	newTask->argc = effectiveArgc;

	size_t argvPtrBytes  = (size_t)(effectiveArgc + 1) * sizeof(char*);
	size_t argvStrBytes  = (size_t)effectiveArgc * TASK_MAX_PATH_LEN;
	size_t argvBlobBytes = argvPtrBytes + argvStrBytes;

	newTask->argv = (char**)kmalloc_aligned(argvBlobBytes);
	char *argvStrBase = (char*)newTask->argv + argvPtrBytes;   // kernel view of the string area
	for (int cnt = 0; cnt < effectiveArgc; cnt++)
	{
		//Source string: caller-provided argv[cnt], or the path for the implicit argv[0].
		const char *src = (argc > 0) ? argv[cnt] : path;
		char *dst = argvStrBase + ((size_t)cnt * TASK_MAX_PATH_LEN);
		strncpy(dst, src, TASK_MAX_PATH_LEN);
		dst[TASK_MAX_PATH_LEN - 1] = '\0';                     // strncpy won't NUL-terminate an over-long src
		//Store the address the PROGRAM will use (its own TASK_ARGV_VIRT view).
		newTask->argv[cnt] = (char*)(uintptr_t)(TASK_ARGV_VIRT + argvPtrBytes + ((size_t)cnt * TASK_MAX_PATH_LEN));
	}
	newTask->argv[effectiveArgc] = NULL;                       // argv[argc] == NULL (convention)

	//Map the whole argv blob at the "standard" argument address.
	//kmalloc_aligned() returns an HHDM (upper-half virtual) address; paging_map_pages
	//needs the underlying PHYSICAL address, so convert with -kHHDMOffset (same as the
	//env mapping does in task_setup_entry).  Passing the HHDM address directly stuffs
	//non-canonical high bits into the PTE and faults with a reserved-bit #PF (0x8) the
	//moment the program dereferences argv.
	mapPages = (argvBlobBytes + PAGE_SIZE - 1) / PAGE_SIZE;
	uintptr_t argvPhys = (uintptr_t)newTask->argv - kHHDMOffset;
	paging_map_pages(newTask->pml4v, TASK_ARGV_VIRT, argvPhys, mapPages, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

	newTask->kernelTask=isKernelTask;

	// Inherit the parent's environment.  env_inherit makes a full independent copy
	// so parent and child can diverge freely.  True CoW (sharing the physical page
	// until first write) is a future optimisation.
	newTask->env = env_inherit(parentTaskPtr->env);

	// Now that argc/argv are built and mapped (TASK_ARGV_VIRT) and env is
	// inherited, latch the ELF entry registers: RDI=argc, RSI=argv, RDX=env.
	// Must run AFTER the argument/env setup above.
	if (loadedElfProgram) {
		task_setup_entry(newTask);

		// Ring-3 tasks also get their exit path wired here (ring-0 ELF tasks
		// were already seeded with task_exit_with_retval in createThread —
		// mapping the trampoline for them would overwrite that seed).
		if (!isKernelTask) {
			task_setup_ring3_exit_path(newTask);
		}

		// Tell an attached GDB (if any) which program image just landed and
		// where, so it can auto-load the matching symbol file.  Stage the
		// info in the debug globals first — see debug_task_loaded's comment.
		strncpy(kDebugTaskLoadedPath, newTask->path, TASK_MAX_PATH_LEN);
		kDebugTaskLoadedPath[TASK_MAX_PATH_LEN - 1] = '\0';
		kDebugTaskLoadedBias = newTask->loadBias;
		debug_task_loaded();
	}

	return newTask;
}
