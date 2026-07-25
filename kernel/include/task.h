#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "dlist.h"
#include "thread.h"
#include "time.h"
#include "env.h"
#include "handle.h"

#define TASK_MAX_EXIT_HANDLERS 10
#define TASK_DEFAULT_PRIORITY 0
// TASK_STRUCT_VADDR removed - task_t now lives in kernel heap, no fixed mapping needed
#define TASK_HEAP_START 0x70000000
#define TASK_HEAP_END   0x00007FFFFFFFFFFF
#define TASK_ARGV_VIRT 0x6f000000
//Virtual address of the environment pointers
#define TASK_ENVP_VIRT 0x6f010000
#define TASK_ENV_VIRT 0x6f006000
//Virtual address of the ring-3 exit trampoline page (read-only+exec, user).
//Seeded as _start's return address so a plain `ret` becomes an exit syscall.
//See task_setup_ring3_exit_path() and the template in task_exit_asm.S.
#define TASK_EXIT_TRAMPOLINE_VIRT 0x6f020000
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
        // The one task Ctrl+C must never kill: husk sitting at its prompt.
        // Tagged by the kernel when it launches the shell (kernel.c). When the
        // controlling shell IS the foreground task, ETX (0x03) stays a DATA
        // byte — husk treats it as line-kill (^C, fresh prompt) — instead of
        // becoming a SIGINT. A flag rather than a global pointer because it
        // generalizes to per-tty controlling shells later (SIGINT.md).
        bool controllingShell;
        bool kernelTask;
        struct tm startTime, endTime;
        uint64_t entryPoint;
        // ELF load bias: 0 for static (ET_EXEC) programs, the randomized/fixed
        // relocation offset for PIE (ET_DYN) ones.  Consumed by the GDB
        // symbol-autoload hook (debug_task_loaded) so .gdbinit can offset the
        // program's debug info to where the image actually landed.
        uint64_t loadBias;
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
		// The per-task handle table (handle.h). Handles 0/1/2 are stdin/stdout/
		// stderr and start out wired to the console; the shell redirects them to
		// pipe ends when it builds a pipeline. The legacy void* stdin/stdout/
		// stderr fields above are the OLD (unused) placeholders from the first
		// OS — this array is the real thing.
		handle_t handles[TASK_MAX_HANDLES];
		void *prev, *next;
    } task_t;

	// The foreground task: the one task the console belongs to RIGHT NOW —
	// by definition, "the task the controlling shell is currently blocked
	// waiting on" (or the shell itself, between commands). One pointer
	// because v1 has one console; becomes a per-tty_t field when virtual
	// terminals arrive, exactly like kConsoleWaiter (console.c). Moved by
	// task_wait — keying the transfer on WAIT, not spawn, means a future
	// backgrounded (&) child correctly never takes the console. Read from
	// the keyboard IRQ path (console_intr_intercept), which is why it is a
	// single aligned pointer: x86-64 reads it atomically, no lock needed.
	extern task_t * volatile kForegroundTask;

		task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID);
	void task_exit(void);
	void task_exit_with_retval(void);   // asm stub: captures RAX into task->retVal then calls task_exit
	// Block until a child of parentTask exits, reap it, return it (its exit
	// code via *exitCode). targetPid > 0 waits for that specific child;
	// targetPid == 0 waits for the first of ANY child to end. Returns
	// immediately if a matching child has already exited; returns NULL if no
	// matching child exists at all.
	task_t* task_wait(task_t* parentTask, uint64_t targetPid, uint64_t* exitCode);
	int task_reap_eligible_zombies(size_t max_to_reap);
	void* task_alloc_aligned(task_t* task, size_t size);
	void* task_alloc_guarded_stack(task_t* task, size_t stackSize, bool isRing3);
	uintptr_t task_reserve_task_virt(task_t* task, size_t size);
	#endif
