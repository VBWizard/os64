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
#include "elf_loader.h"
#include "shared_object.h"
#include "memory/vma.h"
#include "sprintf.h"
#include "allocator.h"
#include "memset.h"
#include "kworker.h"

extern volatile uint64_t kSystemCurrentTime;
extern task_t* kKernelTask;
extern uintptr_t kKernelPML4;

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

	// Tasks sharing kKernelPML4 must use a shared counter; per-task counters all
	// start at KERNEL_TASK_MEMORY_BASE and would map at the same virtual address.
	uintptr_t *counter = (task->pml4v == (uint64_t*)kKernelPML4v)
		? &kKernelTaskMemoryNextVirt
		: &task->taskMemoryNextVirt;

	uintptr_t virt_base = *counter;
	*counter += aligned_size;

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
	core_local_storage_t *cls = get_core_local_storage();

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

int task_reap_eligible_zombies(size_t max_to_reap)
{
	task_t *task = kTaskList;
	size_t reaped = 0;

	while (task != NO_TASK && task != NULL && reaped < max_to_reap) {
		task_t *child = task->deadChildHead;
		while (child != NULL && reaped < max_to_reap) {
			task_t *next_child = child->deadChildNext;
			bool eligible = child->autoReap || task->exited || child->parentTask == NULL;

			if (eligible) {
				printd(DEBUG_TASK | DEBUG_DETAILED,
					"task_reap_eligible_zombies: reaping child task 0x%08x (%s), parent=0x%08x (%s), autoReap=%u, parentExited=%u\n",
					child->taskID,
					child->exename,
					task->taskID,
					task->exename,
					child->autoReap,
					task->exited);
				task_remove_dead_child(task, child);
				if (child->threads != NULL) {
					scheduler_reap_zombie_thread(child->threads);
				}
				reaped++;
			}

			child = next_child;
		}
		task = task->next;
	}

	return (int)reaped;
}

void task_exit(void)
{
	core_local_storage_t *cls = get_core_local_storage();

	// CRITICAL: Switch to kernel context before doing anything!
	// We're currently running on task's stack with task's CR3 loaded
	// Need to switch to kernel stack and kKernelPML4 to safely access kernel structures

	// Save kernel stack pointer (from CLS while it's still valid)
	uintptr_t kernel_rsp = cls->kernel_interrupt_stack_top - 16;

	// Switch to kernel interrupt stack
	__asm__ volatile("mov rsp, %0" : : "r"(kernel_rsp));

	// Switch to kKernelPML4
	__asm__ volatile("mov cr3, %0" : : "r"((uint64_t)kKernelPML4) : "memory");

	// CRITICAL: Reload CLS pointer after stack/CR3 switch!
	// The previous 'cls' variable was on the task's stack and is now invalid.
	// get_core_local_storage() reads from GS:0, which is always valid.
	cls = get_core_local_storage();

	// Now we're in kernel context - safe to access kernel structures
	thread_t *thread = cls ? cls->currentThread : NULL;
	task_t *task = cls ? cls->task : NULL;

	// task->retVal was already written by task_exit_with_retval (asm) before
	// any C code ran or any stack switch occurred.  We just propagate it to
	// thread->retVal and mark both as exited.
	if (thread) {
		thread->exited = true;
		thread->retVal = task ? task->retVal : 0;
	}

	if (task) {
		task->exited = true;
		task_enqueue_dead_child(task);
	}

	scheduler_yield(cls);

	while (1==1)
	{
		__asm__("sti\nhlt\n");
	}
}

task_t* task_wait(task_t* parentTask, uint64_t* exitCode)
{
	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = parentTask ? parentTask : (cls ? cls->task : NULL);

	if (parent == NULL || parent->threads == NULL) {
		return NULL;
	}

	while (1==1)
	{
		task_t *child = parent->deadChildHead;
		if (child != NULL) {
			parent->deadChildHead = child->deadChildNext;
			if (parent->deadChildHead == NULL) {
				parent->deadChildTail = NULL;
			}
			child->deadChildNext = NULL;
			if (exitCode != NULL) {
				*exitCode = child->retVal;
			}
			if (child->threads != NULL) {
				scheduler_reap_zombie_thread(child->threads);
			}
			return child;
		}

		parent->waitingForChild = true;
		scheduler_change_thread_queue(parent->threads, THREAD_STATE_ISLEEP);
		scheduler_yield(cls);
		parent->waitingForChild = false;
	}
}

task_t* task_initialize(task_t* parentTask, bool kernelTask, bool idleTask, uint64_t pinnedAPICId)
{
    printd(DEBUG_TASK,"task_initialize: Initializing task\n");

	task_t* newTask = kmalloc_aligned(sizeof(task_t));
    printd(DEBUG_TASK,"task_initialize: Malloc'd 0x%016x for process\n",newTask);

    if (idleTask)
    {
        newTask->pml4v = parentTask->pml4v;
        newTask->pml4 = parentTask->pml4;
    }
    
    newTask->parentTask = parentTask;
    newTask->priority = TASK_DEFAULT_PRIORITY;

	newTask->mmaps = kmalloc(sizeof(dlist_t));
	if (newTask->mmaps) {
		dlist_init(newTask->mmaps);
	}
	// Lazily created on first use by task_map_shared_object — most tasks
	// never touch dynamic linking, so most never allocate this at all.
	newTask->shared_objects = NULL;

	// Special case: ktask (the main kernel task) uses kKernelPML4 directly
	// All other tasks get their own PML4 with shared upper-half page tables
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
        if (!idleTask)
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
        }
		newTask->taskMemoryNextVirt = kernelTask ? KERNEL_TASK_MEMORY_BASE : USER_TASK_MEMORY_BASE;
		printd(DEBUG_TASK | DEBUG_DETAILED, "task_initialize: Allocated new PML4 at 0x%lx for %s task (shared upper-half)\n",
			newTask->pml4, kernelTask ? "kernel" : "user");
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
	task->entryPoint = main_so->load_bias + main_so->image->ehdr.e_entry;
	if (task->threads != NULL) {
		task->threads->regs.RIP = task->entryPoint;
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
	bool isIdleTask = strnstr(path, "/idle",10);
	bool isLogdTask = strnstr(path, "/logd",10);
	bool isKWorkerTask = strnstr(path, "/kworker",10);
	// Set when we actually load an ELF image below, so we know to latch the ELF
	// entry registers (argc/argv/env) later — AFTER those fields are populated.
	bool loadedElfProgram = false;
	task_t* newTask = task_initialize(parentTaskPtr, isKernelTask, isIdleTask, pinnedAPICID);

    //Copy the path (parameter) value from the parentTask's memory.
    newTask->path=kmalloc(TASK_MAX_PATH_LEN); 
	strncpy(newTask->path,path,TASK_MAX_PATH_LEN);

	    printd(DEBUG_TASK,"task_create: Creating %s task for %s\n",isKernelTask?"kernel":"user",newTask->path);
	printd(DEBUG_TASK | DEBUG_DETAILED,
		"task_create: path=%s pinnedAPICID=%s0x%08lx idle=%u logd=%u kworker=%u\n",
		newTask->path,
		pinnedAPICID == THREAD_NO_AFFINITY ? "THREAD_NO_AFFINITY/" : "",
		pinnedAPICID,
		isIdleTask,
		isLogdTask,
		isKWorkerTask);

    char *slash=newTask->path, *slash2=newTask->path;
    while (slash!=NULL)
    {
        slash = strstr(slash2+1, "/");
        if (slash)
            slash2 = slash;
    }
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

	if (!isIdleTask && !isLogdTask && !isKWorkerTask && kRootFilesystem != NULL)
	{
		if (elf_is_dynamic(newTask->path)) {
			elf_resolve_dynamic_dependencies(newTask, newTask->path);
		} else if (elf_load_from_path(newTask, newTask->path) != 0) {
			panic("task_create: Failed to load ELF for task %s\n", newTask->path);
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
	}

	return newTask;
}
