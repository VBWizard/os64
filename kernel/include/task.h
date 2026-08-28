#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "dlist.h"
#include "thread.h"
#include "spinlock.h"   // spinlock_t — the per-task signal-delivery lock
#include "time.h"
#include "env.h"
#include "handle.h"

#define TASK_MAX_EXIT_HANDLERS 10
#define TASK_DEFAULT_PRIORITY 0
// TASK_STRUCT_VADDR removed - task_t now lives in kernel heap, no fixed mapping needed
#define TASK_HEAP_START 0x70000000
// The heap's ceiling stops one window short of the top of the canonical lower
// half: the 512GB above it is the SHARED LIBRARY window (TASK_SHLIB_VIRT_BASE,
// shared_object.h), carved off the top here 2026-08-22 so that two independent
// VA allocators — the heap's bump pointer (syscall.c) and the shared-object
// registry's — provably cannot meet. The heap loses 0.4% of a range it has
// never used more than a few megabytes of; see shared_object.h for why the
// libraries had to leave the low 2GB (they were sharing it with app_bases.py).
#define TASK_HEAP_END   0x00007EFFFFFFFFFF
// ── THE FIXED TASK-VA BLOCK (re-laid 2026-08-13 for wildcards) ──────────────
//
// Three blobs live at addresses the ABI promises: argv, the environment, and
// the ring-3 exit trampoline. They sit between the app link-base window
// (userland/tools/app_bases.py hands out bases from 0x400000 up to exactly
// TASK_ARGV_VIRT — argv cannot grow DOWNWARD) and TASK_HEAP_START, which
// leaves 16MB. Almost none of it was being used.
//
// (Until 2026-08-22 the SHARED LIBRARY window ended here too, overlapping the
// app window — see shared_object.h. The libraries have moved to the top of the
// user address space; everything below 0x6f000000 is the apps' alone now.)
//
// The old layout put ENV only 0x6000 (24KB) above ARGV, which was ample while
// spawn allowed 32 arguments of 128 bytes. Raising that ceiling for shell
// globbing (`cat /tmp/*` must not die on the 33rd file) makes 24KB far too
// tight — and NOTHING CHECKED IT. A blob that outgrew the gap would have been
// mapped straight over TASK_ENV_VIRT by paging_map_pages, and the child's
// environment would silently become the tail of its own argv. That guard now
// exists (task.c, TASK_ARGV_MAX_BYTES); this layout gives it room to never
// fire.
//
// ARGV STAYS AT 0x6f000000 on purpose: it is the one address a program can
// observe directly (arg_echo asserts on it), so the window grows upward and
// its neighbours move instead.
//
//   0x6f000000  argv blob        1MB window  (strings PACKED, so a two-arg
//                                             command still maps one page)
//   0x6f100000  environment      64KB window (born one page; setenv grows it
//                                             on demand, doubling to this cap)
//   0x6f110000  exit trampoline  one page, read-only + user
//   ...         (~15MB unused)
//   0x70000000  TASK_HEAP_START
#define TASK_ARGV_VIRT 0x6f000000
// The most the argv blob may occupy before it would collide with the
// environment. Enforced in task_create — see the packing comment there.
#define TASK_ARGV_MAX_BYTES 0x100000
#define TASK_ENV_VIRT 0x6f100000
// The env block's growth ceiling — the fixed-VA window between TASK_ENV_VIRT
// and the exit trampoline. ENFORCED since 2026-08-14: env_grow (env.c) caps
// growth here when setenv fills the block, and task_setup_entry panics if a
// bigger block ever reaches its map site (the backstop). Before growth
// existed this constant guarded nothing; now it is the number that makes
// "your environment is full" mean 64KB instead of one page.
#define TASK_ENV_MAX_BYTES 0x10000
//Virtual address of the ring-3 exit trampoline page (read-only+exec, user).
//Seeded as _start's return address so a plain `ret` becomes an exit syscall.
//See task_setup_ring3_exit_path() and the template in task_exit_asm.S.
#define TASK_EXIT_TRAMPOLINE_VIRT 0x6f110000
// The SIGNAL RETURN stub shares that page, 64 bytes in (2026-08-23). One page,
// two templates: the exit trampoline is a handful of instructions and the
// signal stub is three, so a second page would be 4KB spent to avoid an
// offset. 64 is comfortably past the first template and keeps both aligned.
//
// Sharing is not merely thrifty — it is the whole reason signal return is
// cheap here. Historic Unix put this stub on the STACK, which needs an
// executable stack; NX outlawed that (nx_test asserts it kills the program)
// and Linux had to grow a vDSO to escape it. os64 already had a per-task page
// that is PAGE_USER, not PAGE_WRITE, and executable. See SIGNALS.md §5.
#define TASK_SIGRETURN_OFFSET     64
#define TASK_SIGRETURN_VIRT       (TASK_EXIT_TRAMPOLINE_VIRT + TASK_SIGRETURN_OFFSET)
// (TASK_ENVP_VIRT, an "environment pointers" address at 0x6f010000, was
// deleted here 2026-08-13: defined since the first OS, referenced by nothing
// in the kernel or userland, and sitting squarely in the middle of the window
// argv needed. A constant nobody reads is not an ABI, it is furniture.)
// Task-specific memory allocation base addresses (lower half)
#define USER_TASK_MEMORY_BASE   0x10000000  // 256MB - for user task allocations
#define KERNEL_TASK_MEMORY_BASE 0x40000000  // 1GB - for kernel task allocations
#define STDIN (void*)0
#define STDOUT (void*)1
#define STDERR (void*)2
// (TASK_MAX_ARG_LEN and the three TASK_ENVIRONMENT_* constants were deleted
// here 2026-08-14: they described the pre-envpage environment layout — 1024
// pointer slots followed by fixed 512-byte strings — that envpage_t's packed
// key\0val\0 block replaced. Referenced by nothing; same species of furniture
// as TASK_ENVP_VIRT above, removed the same week. The REAL env sizing lives
// in env.h (ENV_DATA_CAPACITY) and TASK_ENV_MAX_BYTES above.)
// Longest single path/argument the kernel will carry, NUL included. Raised
// 128 -> 256 on 2026-08-13 (Chris: "even the path max length makes me kind of
// nervous"). Costs nothing per task now that the argv blob packs its strings
// end to end — this is a CAP, no longer a per-argument reservation. It is
// still the width of the fixed `raw[]`/`path[]` scratch buffers in syscall.c's
// path handlers, which live on an 80KB kernel stack, so doubling them is noise.
#define TASK_MAX_PATH_LEN 256

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

    // The table-page arena (memory/arena.h) — forward-declared so task.h
    // stays include-light; only task.c and paging.c touch the real type.
    struct arena;

    //NOTE: If you need access to any of the struct members, add a #DEFINE to asm-offsets.c and
    //      a constant will be created in asm-offsets.c at compile time.
    typedef struct task
    {
		//The task identifier.  This will be the same as the first threadID assigned to the task
		uint64_t taskID;
		volatile bool exited;
		// THE TEARDOWN CLAIM — exactly one thread runs a task's teardown
		// (Codex #29 rd10). `exited` cannot be that claim, and this is the
		// whole bug: it is published only AFTER handle_close_all(), and
		// task_exit's own comment says that stretch "may sleep in the VFS and
		// resume on any core". Killing a threaded task sends EVERY sibling
		// down task_exit (the redirect points them at the exit trampoline,
		// which calls the TASK exit syscall), so a sibling arriving while the
		// first thread is still inside the VFS reads `exited == false`, walks
		// straight past the guard, and runs the whole teardown a second time.
		// The handle table survives that (the CLOSING sentinel, rd4), but
		// task_enqueue_dead_child does NOT: enqueueing one child twice makes
		// `parent->deadChildTail->deadChildNext = child` write child->next =
		// child, and a self-linked corpse means the undertaker's walk never
		// ends. Claimed with an atomic exchange, so the winner is decided
		// before any of that runs. Zeroed = unclaimed, by the
		// all-allocations-zeroed rule.
		volatile bool tearingDown;
        char exename[128];
        thread_t* threads;
        void* elf;
        char* path;
        volatile uint64_t retVal;
        // THE SIGNAL HANDLERS ARE THE TASK'S, indexed BY SIGNAL NUMBER
        // (2026-08-23, SIGNALS.md §2). They used to live on the thread — the
        // commented-out `signals_t signals;` that stood here for years was
        // somebody standing at this same fork — and the reason they belong
        // here is the 2026-08-02 scar recorded above task_signal_all_threads:
        // a signal aimed at a task is OR'd into EVERY thread. Per-thread
        // handlers would therefore run one SIGTERM once per thread, and an
        // app's "wait, I have unsaved work" firing four times in a
        // four-threaded program is not a policy anybody wants.
        //
        // Per-task also answers what per-thread only raises: a thread created
        // after registration has the handler automatically, there is no
        // inheritance rule to invent, and a handler is plainly a property of
        // the PROGRAM. The cost is one rule, and it is small: the first thread
        // to reach a checkpoint runs the handler and clears the bit
        // task-wide.
        //
        // NULL = no handler = the kernel's default action for that signal.
        // Installed through SYSCALL_SIGNAL_HANDLER (49) and DELIVERED by all
        // three paths in signals.c — the syscall-exit dispatcher (§5), the
        // scheduler's visit to a spinning thread (§10), and the page-fault
        // handler for a caught SIGSEGV (§9). Writes take signalLock below.
        // (This line used to read "Nothing installs one yet; the registration
        // syscall is step 2." Both steps shipped in the same arc that added
        // the lock underneath it, and the note outlived them by a fortnight —
        // which is the whole argument for AGENTS.md § The Comment Is Part Of
        // The Code, recorded here because this is where it caught us.)
        void *sighandler[SIGNAL_COUNT];
        // Serializes SIGNAL DELIVERY across this task's threads (2026-08-24,
        // Codex #29). The aim is a broadcast — every thread carries the
        // pending bit — so without a task-wide claim two threads on two cores
        // both pick the same signal, both build a frame, and both run the
        // "once per task" handler concurrently. This lock makes pick-build-
        // consume atomic per task, in BOTH delivery paths (signals.c). Zeroed
        // = unlocked by the all-allocations-zeroed rule. Lock order: the
        // scheduler path already holds the queue lock when it takes this, and
        // the syscall path takes this alone — no cycle.
        //
        // IT HAS A SECOND JOB, and it is not obvious from the name (2026-08-24,
        // Codex #29 rd8). It is also the barrier that keeps a task's user page
        // ALIVE across a signal-frame write. Delivery reaches a user stack the
        // house way — walk the task's tables for the physical page, store
        // through `phys | kHHDMOffset` (CLAUDE.md) — and that alias is
        // dereferenceable only WHILE THE PAGE IS ALLOCATED. A sibling thread
        // calling unmap() between the resolve and the store frees the page,
        // which HHDM-unmaps the kernel alias, and the store then takes a
        // ring-0 #PF: ring 3 panicking the kernel by racing its own address
        // space. So syscall_unmap takes this lock around each page it frees,
        // and all THREE delivery paths (§5, §9, §10) hold it across their
        // frame writes. A second per-task lock would have been the tidier
        // name, but two of the three sites already held this one and a second
        // lock is a second ordering to get wrong.
        spinlock_t signalLock;
        uint64_t heapStart, heapEnd;
        // WHERE THIS TASK'S malloc PUBLISHES ITS REPORT (SYSCALL_HEAP_REPORT,
        // 2026-08-15). A USER virtual address, valid only under this task's own
        // page tables — the kernel stores it and touches it in exactly one
        // place, procfs's /proc/<id>/heap generator, which reads it the
        // documented way (walk the task's tables, read through the HHDM). 0 =
        // this task has no libos64 heap, which is the honest answer for a raw
        // fixture or a binary from another world. NEVER dereference it directly.
        uintptr_t heapReportVirt;
        short priority;           //-20=highest, 20=lowest
        void* exitHandler[TASK_MAX_EXIT_HANDLERS];
        struct task* parentTask;
        struct task* deadChildHead;
        struct task* deadChildTail;
        struct task* deadChildNext;
        bool waitingForChild;
        // WHICH THREAD IS DOING THE WAITING (2026-08-25). task_wait used to
        // park — and task_enqueue_dead_child used to wake — `threads`, the
        // FIRST thread of the task, on the unstated assumption that the
        // waiter is the main thread. Every waiter was, until the desktop
        // shell put its reaper on a second thread: that thread's wait
        // parked the MAIN thread instead and left the reaper spinning in
        // ring 0 for the shell's whole life (top showed /bin/desktop at 52%
        // of a core with one gterm child). Set by task_wait to the calling
        // thread before it parks, cleared when it returns; read by the
        // dead-child wake under kDeadChildLock, together with the flag
        // above, so the wake can never target a thread that has already
        // stopped waiting. One waiter per task, like the flag it travels with.
        thread_t* waitThread;
        bool autoReap;
        // THE DEATH CERTIFICATE (ruling 2026-08-06: kworker buries only the
        // COLLECTED). Set by whoever consumes the exit status — task_wait /
        // task_reap_any_dead after copying retVal out — and ALWAYS as that
        // consumer's LAST touch of this struct: setting it publishes "this
        // corpse may be freed under you." Direct-poll consumers (the test
        // harness reads exited/retVal off the struct itself) signal the same
        // thing through autoReap after their last read. Until one of the two
        // is set (or the parent is dead), the corpse stays a visible zombie —
        // which is not a leak, it is the 1971 contract: a zombie IS an exit
        // status nobody has claimed yet.
        volatile bool retValCollected;
        // Private link for the undertaker's two-phase burial list (task.c).
        // NOT the deadChild chain and NOT the kTaskList spine: a corpse on
        // THIS list has already been unlinked from both and is invisible to
        // every walker; it waits here exactly one kworker pass (the grace
        // period that protects lockless /proc walkers standing on it) before
        // task_destroy frees it for real.
        struct task* burialNext;
        // The one task Ctrl+C must never kill: husk sitting at its prompt.
        // Tagged by the kernel when it launches the shell (kernel.c). When the
        // controlling shell IS the foreground task, ETX (0x03) stays a DATA
        // byte — husk treats it as line-kill (^C, fresh prompt) — instead of
        // becoming a SIGINT. A flag rather than a global pointer because it
        // generalizes to per-tty controlling shells later (SIGINT.md).
        bool controllingShell;
        // Launched with `&` — a job the shell spawned and will NOT wait on.
        // Consumed by console_read: a background job's read of handle 0
        // returns EOF rather than joining the queue for the keyboard, so it
        // can never silently eat the shell's keystrokes (`cmd &` behaves as
        // `cmd < /dev/null &`). Deliberately NOT the same question as
        // "is this kForegroundTask": in `cat | upper` husk waits on the LAST
        // stage, so `cat` is not the foreground task and gating on that would
        // hand every pipeline's first stage an instant EOF. Writes are
        // untouched — a background job still prints to the screen.
        // Cleared when `fg` arrives, which is why this is a job property and
        // not a different object wired into the child's handle 0: fg must be
        // able to re-attach input without surgery on a running task's table.
        bool backgroundJob;
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
        // How many pages the argv blob occupies at TASK_ARGV_VIRT — kept so
        // /proc/<id>/maps can draw the [argv] row with its true extent
        // (2026-08-22). The env block carries its own page_count; this is
        // the one region whose size was computed, used, and forgotten.
        uint32_t argvPages;
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
		// Every table page of THIS address space — the PML4 and each PDPT/PD/
		// PT — comes from here and dies with it (arena_destroy at burial, see
		// PAGING_ARENA.md). NULL for ktask, whose tables ARE the kernel's and
		// come from the eternal pool. Kernel-side arena_t on purpose, never a
		// task_arena_t: page tables are the one thing ring 3 must never see.
		struct arena *tableArena;
		// The creator's traveling bracket (pta_ = paging_table_arena): while
		// THIS task is building a child, these name the child's pml4/arena so
		// paging_table_arena_for() can route the child's table draws (stacks,
		// argv, env, trampoline) to the child's arena. On the CREATOR because
		// the creator can block (ELF I/O) and resume on another core — a
		// per-core scratch would be left behind; the task struct travels.
		// Set in task_initialize, cleared at task_create's every exit.
		uint64_t *pta_buildingChildPml4v;
		struct arena *pta_buildingChildArena;
		uintptr_t taskMemoryNextVirt;  // Next available virtual address for task-specific allocations
		dlist_t* shared_objects;       // shared_object_t* this task depends on (dynamic linking, bookkeeping only — see shared_object.c)
		// The per-task handle table (handle.h). Handles 0/1/2 are stdin/stdout/
		// stderr and start out wired to the console; the shell redirects them to
		// pipe ends when it builds a pipeline. The legacy void* stdin/stdout/
		// stderr fields above are the OLD (unused) placeholders from the first
		// OS — this array is the real thing.
		handle_t handles[TASK_MAX_HANDLES];
		// The controlling terminal (tty.h): which virtual terminal this task
		// reads, writes, and answers Ctrl+C on. Inherited from the parent at
		// task_create — a shell's children work its terminal, which is the
		// whole 1970s meaning of "controlling" — and NULL means the system
		// console, VT1 (task_tty() resolves; kernel threads created before
		// tty_init simply stay NULL forever and that is correct).
		struct tty *tty;
		void *prev, *next;
    } task_t;

	// (kForegroundTask, 2026-07..2026-08-08, promoted.) The foreground task
	// — "the task the controlling shell is currently blocked waiting on" —
	// lives in tty_t.fgTask now, one per terminal, exactly as its birth
	// comment promised. Everything else about it survived the move: task_wait
	// still moves it (keying the transfer on WAIT, not spawn, so a
	// backgrounded (&) child never takes the console), and the keyboard IRQ
	// path still reads it as a single aligned pointer, atomically, no lock.

	// THE RECLAIM LEDGER (task.c; the 2026-08-13 deferral ledger, PAID
	// 2026-08-15). Cumulative bytes/pages of VMA backing memory the
	// undertaker has freed at burial since boot. Every burial that gives
	// anything back announces it on DEBUG_TASK. The post-boot leak test
	// reads these to prove the free path actually ran, and asserts a
	// spawn→exit→burial cycle's allocator delta is ZERO — every byte a task
	// consumed comes back when it dies. See the long comment at the
	// definition for the deferral's story and why it ended.
	extern uint64_t kTaskVmaReclaimedBytes;
	extern uint64_t kTaskVmaReclaimedPages;
	// Completed burials since boot — the undertaker's census, and the leak
	// test's clock (a cycle is done when the funeral is, not when the task
	// exited). Incremented as task_destroy's very last act.
	extern uint64_t kTaskBurialCount;

		task_t* task_create(char* path, int argc, char** argv, task_t* parentTaskPtr, bool isKernelTask, uint64_t pinnedAPICID);
	void task_exit(void);
	void task_exit_with_retval(void);   // asm stub: captures RAX into task->retVal then calls task_exit
	// Block until a child of parentTask exits, collect it, and return its
	// taskID (its exit code via *exitCode). targetPid > 0 waits for that
	// specific child; targetPid == 0 waits for the first of ANY child to end.
	// Returns immediately if a matching child has already exited; returns 0
	// if no matching child exists at all; returns TASK_WAIT_INTERRUPTED when
	// the CALLING THREAD has a signal pending that ends a park
	// (signal_park_must_end — a terminate, or one a handler will catch),
	// with nothing collected. It is a park like every other blocking call
	// and answers like one (Codex #32): a shell waiting on a child could not
	// run its own SIGWINCH handler until the child died, because this wait
	// re-parked on every wake without asking. The syscall decides what the
	// answer means — OS64_INTERRUPTED to a caller that will catch, the
	// default action otherwise — exactly as sleep and the console read do.
	//
	// RETURNS AN ID, NOT A POINTER — deliberately (the cleanup notes called
	// this the day task_destroy was first sketched): collecting marks the
	// corpse retValCollected, which licenses kworker to FREE it on its next
	// sweep. A returned pointer would be a dangling invitation; an ID is a
	// fact that stays true forever.
	#define TASK_WAIT_INTERRUPTED UINT64_MAX   // no task ever wears this ID
	uint64_t task_wait(task_t* parentTask, uint64_t targetPid, uint64_t* exitCode);
	// task_wait's NON-BLOCKING half: collect the next already-dead child of
	// parentTask and return its taskID (exit code via *exitCode), or 0 if
	// none has died — never sleeps, even when live children exist. This is
	// what lets a shell collect background jobs at its prompt without hanging
	// on the ones still running, and it is why `&` does not leak zombies the
	// way os32's did: REPORTING a finished job and COLLECTING it are the same
	// act (and collecting is what licenses the burial).
	// Does NOT touch kForegroundTask — collecting a background corpse is not
	// a change of who owns the console.
	uint64_t task_reap_any_dead(task_t* parentTask, uint64_t* exitCode);
	int task_reap_eligible_zombies(size_t max_to_reap);
	void* task_alloc_aligned(task_t* task, size_t size);
	void* task_alloc_guarded_stack(task_t* task, size_t stackSize, bool isRing3);
	uintptr_t task_reserve_task_virt(task_t* task, size_t size);

	// OR a signal bit into EVERY thread this task owns — unless the signal's
	// default is IGNORE and the task has no handler for it, in which case
	// nothing is set anywhere (SIGNALS_DEFAULT_IS_IGNORE: ignore means
	// consumed at the raise, not left pending; task_signal_is_ignored).
	//
	// A signal aimed at a task means the task — all of it. Every delivery
	// site used to write `task->threads->signals.sigind |= sig`, which was
	// complete when a task had exactly one thread and became a silent
	// half-measure the day it could have several: `echo kill > /proc/N/ctl`
	// retired the main thread and left four workers burning four cores
	// (found 2026-08-02, audible as fan noise). The workers were never
	// ignoring the signal — nobody had told them.
	//
	// Safe from IRQ context (Ctrl+C's path): a walk of the taskNext chain
	// plus one OR per thread, under the task's signalLock (an irqsave
	// spinlock — publication and consumption claim the same lock so a
	// consumer's clear cannot race a publisher's set), and new threads are
	// fully linked before they are published, so a walker sees either the
	// old chain or the complete new one.
	void task_signal_all_threads(task_t* task, signals signal);
	// The same, plus a scheduling IPI to the cores the threads last ran on —
	// for a sender who is NOT the victim (the hangup sweep, the shutdown
	// ladder). Without the knock, a thread spinning on a tickless AP would
	// carry the mark unread. See the comment on the definition.
	// Answers whether the signal was PUBLISHED — false when the task ignores
	// it (no handler, default IGNORE) and nothing was set or knocked.
	bool task_signal_and_nudge(task_t* task, signals signal);

	// Bring down every thread of a dying task except the one dying. THE
	// single control point for "exit means exit" — see the implementation
	// for why marking alone is not enough on a tickless boot.
	void task_terminate_sibling_threads(task_t* task, thread_t* self);
	#endif
