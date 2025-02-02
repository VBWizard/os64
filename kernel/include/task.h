#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "dlist.h"
#include "thread.h"
#include "time.h"

#define TASK_MAX_EXIT_HANDLERS 10
#define TASK_DEFAULT_PRIORITY 0
#define TASK_STRUCT_VADDR 0xFFFF8000FFFFF000
#define TASK_HEAP_START 0x70000000
#define TASK_HEAP_END   0x00007FFFFFFFFFFF
#define TASK_ARGV_VIRT 0x6f000000
//Virtual address of the environment pointers
#define TASK_ENVP_VIRT 0x6f010000
#define TASK_ENV_VIRT 0x6f006000
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

    typedef struct
    {
		//The task identifier.  This will be the same as the first threadID assigned to the task
		uint64_t taskID;
		bool exited;
        char exename[128];
        thread_t* threads;
        void* elf;
        char* path;
        uint64_t retVal;
        //signals_t signals;
        uint64_t heapStart, heapEnd;
        short priority;           //-20=highest, 20=lowest
        void* exitHandler[TASK_MAX_EXIT_HANDLERS];
        void* parentTask;
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
        //Paged address of the environment pointers
		char** mappedEnvp;
		//Real address of the envionment pointers
        char** realEnvp;
		//Paged address of the environment values
        char* mappedEnv;
		//Real address of the environment values
        char* realEnv;
		uint64_t envPSize, envSize;
        bool justForked;
        uint32_t forkChildCR3;
        uint32_t childNumber;
        uint32_t lastChildNumber;
        bool foreground, stdinRedirected, stdoutRedirected, stderrRedirected;
        uintptr_t *stackInitialPage;
        uint32_t minorFaults, majorFaults, cSwitches;
		uint64_t* pml4, *pml4v;
		void *prev, *next;
    } task_t;

	task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID);
#endif