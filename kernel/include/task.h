#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "dlist.h"
#include "thread.h"
#include "time.h"
#include "env.h"

#define TASK_MAX_EXIT_HANDLERS 10
#define TASK_DEFAULT_PRIORITY 0
// TASK_STRUCT_VADDR removed - task_t now lives in kernel heap, no fixed mapping needed
#define TASK_HEAP_START 0x70000000
#define TASK_HEAP_END   0x00007FFFFFFFFFFF
#define TASK_ARGV_VIRT 0x6f000000
//Virtual address of the environment pointers
#define TASK_ENVP_VIRT 0x6f010000
#define TASK_ENV_VIRT 0x6f006000
// Task-specific memory allocation base addresses (lower half)
#define USER_TASK_MEMORY_BASE   0x10000000  // 256MB - for user task allocations
#define KERNEL_TASK_MEMORY_BASE 0x40000000  // 1GB - for kernel task allocations
#define STDIN (void*)0
#define STDOUT (void*)1
#define STDERR (void*)2
#define TASK_MAX_ARG_LEN 512
#define TASK_ENVIRONMENT_MAX_ENTRIES 1024
#define TASK_ENVIRONMENT_MAX_SIZE TASK_ENVIRONMENT_MAX_ENTRIES * TASK_MAX_ARG_LEN
#define TASK_ENVIRONMENT_DATA_OFFSET (TASK_ENVIRONMENT_MAX_ENTRIES * sizeof(uintptr_t))
#define TASK_MAX_PATH_LEN 128

	struct timeval {
		uint64_t	tv_sec;		/* seconds */
		uint64_t	tv_usec;	/* microseconds */
	};

	struct rusage {
		struct timeval ru_utime; /* user CPU time used */
		struct timeval ru_stime; /* system CPU time used */
		int64_t ru_maxrss;       /* maximum resident set size in (kb) */
		int64_t ru_ixrss;        /* shared memory size (integral kb CLK_TCK) */
		int64_t ru_idrss;        /* unshared data size (integral kb CLK_TCK) */
		int64_t ru_isrss;        /* unshared stack size (integral kb CLK_TCK) */
		int64_t ru_minflt;       /* page reclaims */
		int64_t ru_majflt;       /* page faults */
		int64_t ru_nswap;        /* swaps */
		int64_t ru_inblock;      /* block input operations */
		int64_t ru_oublock;      /* block output operations */
		int64_t ru_msgsnd;       /* IPC messages sent */
		int64_t ru_msgrcv;       /* IPC messages received */
		int64_t ru_nsignals;     /* signals received */
		int64_t ru_nvcsw;        /* voluntary context switches */
		int64_t ru_nivcsw;       /* involuntary context switches */
	};

    //NOTE: If you need access to any of the struct members, add a #DEFINE to asm-offsets.c and
    //      a constant will be created in asm-offsets.c at compile time.
    typedef struct task
    {
		//The task identifier.  This will be the same as the first threadID assigned to the task
		uint64_t taskID;
		volatile bool exited;
        char exename[128];
        thread_t* threads;
        void* elf;
        char* path;
        volatile uint64_t retVal;
        //signals_t signals;
        uint64_t heapStart, heapEnd;
        short priority;           //-20=highest, 20=lowest
        void* exitHandler[TASK_MAX_EXIT_HANDLERS];
        struct task* parentTask;
        struct task* deadChildHead;
        struct task* deadChildTail;
        struct task* deadChildNext;
        bool waitingForChild;
        bool autoReap;
        bool kernelTask;
        struct tm startTime, endTime;
        uint64_t entryPoint;
        int argc;
        char** argv;
        struct rusage usage;
        void* stdin, *stdout, *stderr;        //standard input/output/error pointers
        dlist_t* mmaps;
        int errno;
        char* cwd;                              //Current working directory for the process
        void* startHandler[TASK_MAX_EXIT_HANDLERS];
        int startHandlerPtr;
        envpage_t *env;    // flat key/val environment; inherited CoW by child tasks
        bool justForked;
        uint32_t forkChildCR3;
        uint32_t childNumber;
        uint32_t lastChildNumber;
        bool foreground, stdinRedirected, stdoutRedirected, stderrRedirected;
        uintptr_t *stackInitialPage;
        uint32_t minorFaults, majorFaults, cSwitches;
		uint64_t* pml4, *pml4v;
		uintptr_t taskMemoryNextVirt;  // Next available virtual address for task-specific allocations
		dlist_t* shared_objects;       // shared_object_t* this task depends on (dynamic linking, bookkeeping only — see shared_object.c)
		void *prev, *next;
    } task_t;

		task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID);
	void task_exit(void);
	void task_exit_with_retval(void);   // asm stub: captures RAX into task->retVal then calls task_exit
	task_t* task_wait(task_t* parentTask, uint64_t* exitCode);
	int task_reap_eligible_zombies(size_t max_to_reap);
	void* task_alloc_aligned(task_t* task, size_t size);
	void* task_alloc_guarded_stack(task_t* task, size_t stackSize, bool isRing3);
	#endif
