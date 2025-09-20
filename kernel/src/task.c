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

extern volatile uint64_t kSystemCurrentTime;

void task_idle_loop()
{
	core_local_storage_t *cls = get_core_local_storage();

	while (1==1)
	{
        __asm__("sti\nhlt\n");
	}
}

task_t* task_initialize(task_t* parentTask, bool kernelTask, bool idleTask, uint64_t pinnedAPICId)
{
    printd(DEBUG_TASK,"task_initialize: Initializing task\n");

	task_t* newTask = kmalloc_aligned(sizeof(task_t));
    printd(DEBUG_TASK,"task_initialize: Malloc'd 0x%016x for process\n",newTask);

	newTask->parentTask = parentTask;
	newTask->priority = TASK_DEFAULT_PRIORITY;

	newTask->mmaps = kmalloc(sizeof(dlist_t));
	if (newTask->mmaps) {
		dlist_init(newTask->mmaps);
	}
	if (kernelTask)
	{
		newTask->pml4v = (uint64_t*)kKernelPML4v;
		newTask->pml4 = (uint64_t*)kKernelPML4;
	}
	else
	{
		newTask->pml4v = (uintptr_t*)get_paging_table_pageV();  
		newTask->pml4 = (uintptr_t*)((uintptr_t)newTask->pml4v & ~(kHHDMOffset));
	}
	newTask->threads = createThread((void*)newTask, kernelTask);
	newTask->threads->idleThread = idleTask;
	if (idleTask)
		newTask->threads->mp_apic = pinnedAPICId;
	else
		newTask->threads->mp_apic = 0xffffffffffffffff;
	newTask->taskID = newTask->threads->threadID;
	newTask->exited = false;
    printd(DEBUG_TASK,"task_initialize: Mapping the task_t struct into the task, v=0x%08x, p=0x%08x\n",TASK_STRUCT_VADDR,newTask);
	uint32_t mapPages = sizeof(task_t) / PAGE_SIZE;
	if (sizeof(task_t) % PAGE_SIZE)
		mapPages++;
	paging_map_pages(newTask->pml4v, TASK_STRUCT_VADDR, (uintptr_t)newTask, mapPages, PAGE_PRESENT | PAGE_WRITE);

	return newTask;
}

task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID)
{
	uintptr_t mapPages;
	bool isIdleTask = strnstr(path, "/idle",10);
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

	if (strnstr(path, "/logd",10))
	{
		newTask->threads->regs.CS = GDT_KERNEL_CODE_ENTRY << 3;
		newTask->threads->regs.RIP = (uint64_t)&logd_thread;
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
