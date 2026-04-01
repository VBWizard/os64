#include "task.h"
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
#include "allocator.h"
#include "memset.h"

extern volatile uint64_t kSystemCurrentTime;
extern task_t* kKernelTask;
extern uintptr_t kKernelPML4;

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

	// Get virtual address for this allocation
	uintptr_t virt = task->taskMemoryNextVirt;

	// Map pages into task's PML4
	paging_map_pages(task->pml4v, virt, phys, page_count, PAGE_PRESENT | PAGE_WRITE);

	// Update next available address
	task->taskMemoryNextVirt += aligned_size;

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

	// Get virtual address range (includes guards)
	uintptr_t virt_base = task->taskMemoryNextVirt;

	// Reserve entire virtual range (including guard regions)
	task->taskMemoryNextVirt += aligned_size;

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

	if (thread) {
		thread->exited = true;
		thread->retVal = 0;
	}

	if (task) {
		task->exited = true;
		task->retVal = 0;
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
	if (idleTask)
		newTask->threads->mp_apic = pinnedAPICId;
	else
		newTask->threads->mp_apic = 0xffffffffffffffff;
	newTask->taskID = newTask->threads->threadID;
	newTask->exited = false;

	// Note: TASK_STRUCT_VADDR mapping removed - it was unused
	// task_t lives in kernel heap (HHDM space) and doesn't need fixed virtual mapping

	return newTask;
}

task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID)
{
	uintptr_t mapPages;
	bool isIdleTask = strnstr(path, "/idle",10);
	bool isLogdTask = strnstr(path, "/logd",10);
	task_t* newTask = task_initialize(parentTaskPtr, isKernelTask, isIdleTask, pinnedAPICID);

    //Copy the path (parameter) value from the parentTask's memory.
    newTask->path=kmalloc(TASK_MAX_PATH_LEN); 
	strncpy(newTask->path,path,TASK_MAX_PATH_LEN);

    printd(DEBUG_TASK,"task_create: Creating %s task for %s\n",isKernelTask?"kernel":"user",newTask->path);

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

	if (!isIdleTask && !isLogdTask && kRootFilesystem != NULL)
	{
		if (elf_load_task_from_path(newTask, newTask->path) != 0)
			panic("task_create: Failed to load ELF for task %s\n", newTask->path);
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

	//Argument handling
	//If arguments were passed to this method then set the task based on those arguments
	if (argc > 0)
	{
		newTask->argc = argc;
		newTask->argv=(char**)kmalloc_aligned(2*sizeof(char*) + (TASK_MAX_PATH_LEN * argc)); 
		for (int cnt=0;cnt<argc;cnt++)
		{
			newTask->argv[cnt] = (char*)(argv+(sizeof(char*) * cnt) + (TASK_MAX_PATH_LEN * cnt));
			memcpy(newTask->argv[cnt], argv[cnt], TASK_MAX_PATH_LEN);
		}
	}
	else
	{
		//No arguments were passed, but there is always at least 1 argument which is the 
		//path/filename of the program being executed
		newTask->argc = 1;
		newTask->argv=(char**)kmalloc_aligned(2*sizeof(char*) + TASK_MAX_PATH_LEN); 
		newTask->argv[0] = (char*)newTask->argv+sizeof(char*)*2;
		strncpy(newTask->argv[0], path,TASK_MAX_PATH_LEN);
	}
	//Map the created argv at the "standard" argumets memory address
	mapPages = (newTask->argc * TASK_MAX_PATH_LEN) / PAGE_SIZE;
	if (newTask->argc % PAGE_SIZE)
		mapPages++;
	paging_map_pages(newTask->pml4v, TASK_ARGV_VIRT, (uintptr_t)newTask->argv, mapPages,PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

	newTask->kernelTask=isKernelTask;

	//The kernel task's environment will be created manually, and TASK_ENVIRONMENT_SIZE will be allocated to it
	//Every other task will have a parentTask, and we'll map the parentTask's environment to the child.
	//Since the environment pages will be COW, the child can modify it
	//Map the parentTask's environment pointers into the new task
	newTask->mappedEnvp = parentTaskPtr->mappedEnvp;
	newTask->mappedEnv = parentTaskPtr->mappedEnv;
	newTask->realEnvp = parentTaskPtr->realEnvp;
	newTask->envPSize = parentTaskPtr->envPSize;
	newTask->envSize = parentTaskPtr->envSize;

	//Map the parentTask's environment pointers and values into the new task
	//TODO: Make the parentTask's environment COW before mapping it into the child
	mapPages = (newTask->envPSize + newTask->envSize) / PAGE_SIZE;
	if ((newTask->envPSize + newTask->envSize) % PAGE_SIZE)
		mapPages++;
	paging_map_pages(newTask->pml4v, (uintptr_t)newTask->mappedEnvp, (uintptr_t)parentTaskPtr->realEnvp, mapPages,PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

	return newTask;
}
