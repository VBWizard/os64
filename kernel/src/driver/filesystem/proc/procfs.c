// procfs.c — /proc: processes as files. Design and lineage: PROC.md.
//
// os64's first filesystem with no disk under it. FAT and ext2 answer a read by
// fetching sectors; this one answers by rendering the scheduler's task list as
// text. The VFS above it cannot tell the difference, which is the entire point
// of having a VFS — vfs_resolve_mount routes "/proc/7/status" here by pure
// string matching, and syscall_open/read/readdir call the same dops/fops they
// call for a file on the NVMe disk.
//
// The namespace (see PROC.md for why each name is what it is):
//
//   /proc/                      one entry per live task, named by decimal ID
//   /proc/<id>/status           state, parent, timing, fault counts
//   /proc/<id>/cmdline          argv, one argument per line
//   /proc/<id>/cwd              the task's current directory
//   /proc/<id>/handles          the handle table, one row per open handle
//   /proc/<id>/maps             the address space, one row per VMA
//   /proc/<id>/ctl              WRITE: control.  READ: the vocabulary.
//   /proc/<id>/thread/<tid>/status
//
// Every file is TEXT, "key<TAB>value", one record per line — Plan 9's tradition
// rather than Solaris's binary structs, because `cat` and `ls` already exist and
// a report is not an ABI.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/filesystem/proc/procfs.h"
#include "driver/filesystem/vfs/vfs.h"
#include "driver/filesystem/vfs/synthfs.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "strings/strings.h"
#include "sprintf.h"
#include "serial_logging.h"
#include "scheduler.h"
#include "task.h"
#include "tty.h"   // task_tty — the /proc tty + foreground fields
#include "thread.h"
#include "signals.h"
#include "handle.h"
#include "memory/vma.h"
#include "memory/paging.h"
#include "memory/allocator.h"   // allocator_copy_from_task_va — the fault-proof task read
#include "CONFIG.h"
#include "BasicRenderer.h"   // printf — the mount line belongs on the glass too
#include "smp.h"             // core_local_storage_t (/proc/self's identity read)
#include "smp_core.h"        // mpAcctSettleAll — the status files' freshness contract
#include "os64/heap.h"       // os64_heap_report_t — what ring 3 publishes for /proc/<id>/heap

extern uint64_t  kCPUCyclesPerSecond;  // boot-calibrated: the cycles→µs exchange rate

extern uint64_t kTicksSinceStart;
extern uintptr_t kHHDMOffset;

// A task's VMA list and handle table are walked WITHOUT a lock (os64 has no
// per-task lock to take). A corrupted or concurrently-spliced list must not
// become an infinite loop in the kernel, so every walk is bounded. See the
// "un-synchronized snapshot" note in PROC.md.
#define PROC_MAX_LIST_WALK 512

// The growable text buffer, the path-component parsers, and the snapshot
// file-handle life cycle all live in synthfs now (synthfs.h) — extracted
// 2026-08-08 when sysfs became the second synthetic filesystem. What remains
// here is what only procfs can know: the path grammar, the task lookup, the
// generators, and ctl.

// ── Path parsing ────────────────────────────────────────────────────────────
// Paths arrive fs-local (the mount prefix is already stripped by
// vfs_resolve_mount), always absolute: "/", "/7", "/7/status", "/7/thread",
// "/7/thread/7", "/7/thread/7/status".

typedef enum
{
	PROC_NODE_INVALID = 0,
	PROC_NODE_ROOT,          // /
	PROC_NODE_TASK,          // /<id>
	PROC_NODE_TASK_FILE,     // /<id>/<name>
	PROC_NODE_THREADDIR,     // /<id>/thread
	PROC_NODE_THREAD,        // /<id>/thread/<tid>
	PROC_NODE_THREAD_FILE,   // /<id>/thread/<tid>/<name>
	// /cores lived here 2026-08-07..08-12, then moved to /sys/cpu/<n>/time:
	// a machine fact never belonged at a process-tree address (the exact
	// silting Linux spent a decade regretting — see sysfs.c's header).
} proc_node_type_t;

#define PROC_NAME_MAX 32

typedef struct
{
	proc_node_type_t type;
	uint64_t taskID;
	uint64_t threadID;
	char     name[PROC_NAME_MAX];   // the leaf file name, for the *_FILE types
} proc_path_t;

// The file names a task directory offers, in listing order. The order is the
// order `ls /proc/7` prints, so it is arranged most-useful-first rather than
// alphabetically.
static const char *kProcTaskFiles[] = { "status", "cmdline", "cwd", "handles", "maps", "heap", "tty", "ctl" };
#define PROC_TASK_FILE_COUNT (sizeof(kProcTaskFiles) / sizeof(kProcTaskFiles[0]))

static const char *kProcThreadFiles[] = { "status" };
#define PROC_THREAD_FILE_COUNT (sizeof(kProcThreadFiles) / sizeof(kProcThreadFiles[0]))

// Classify an fs-local path. Purely syntactic, with ONE deliberate
// exception: "self" resolves to the calling task's ID (below) — whether the
// task actually exists is still a separate question, answered by
// proc_find_task at open time.
static void proc_parse_path(const char *path, proc_path_t *out)
{
	char comp[PROC_NAME_MAX];
	size_t pos = 0;

	memset(out, 0, sizeof(*out));
	out->type = PROC_NODE_INVALID;

	if (path == NULL)
		return;

	// Component 1: the task ID (absent = the /proc root itself).
	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_ROOT;
		return;
	}
	// "/proc/self" — the caller's own entry, by name. Linux needs a magic
	// per-opener symlink for this; os64 needs six lines, because this parse
	// already runs in the OPENER's syscall context and identity is one CLS
	// read away. Open-time semantics, embraced: `cat /proc/self/status`
	// names CAT, not your shell — the opener is the self, which is exactly
	// why husk's $$ (expansion-time, the shell's own pid frozen into the
	// argv) is the other spelling and both exist. Deliberately ABSENT from
	// the root listing: `ls /proc` is a census of real tasks, and an alias
	// that answers differently for every asker has no business in a census.
	if (strcmp(comp, "self") == 0)
	{
		core_local_storage_t *cls = get_core_local_storage();
		if (cls == NULL || cls->task == NULL)
			return;   // no identity, no entry (early boot has no self)
		out->taskID = cls->task->taskID;
	}
	else if (!synth_parse_u64(comp, &out->taskID))
		return;   // "/proc/notanumber" — no such entry

	// Component 2: a task file, or the "thread" subdirectory.
	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_TASK;
		return;
	}
	if (strcmp(comp, "thread") != 0)
	{
		if (!synth_name_in(comp, kProcTaskFiles, PROC_TASK_FILE_COUNT))
			return;
		strncpy(out->name, comp, PROC_NAME_MAX - 1);
		// A trailing component after a file name means the path names
		// something inside a file, which is not a thing.
		if (synth_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = PROC_NODE_TASK_FILE;
		return;
	}

	// Component 3: the thread ID.
	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_THREADDIR;
		return;
	}
	if (!synth_parse_u64(comp, &out->threadID))
		return;

	// Component 4: a thread file.
	if (!synth_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_THREAD;
		return;
	}
	if (!synth_name_in(comp, kProcThreadFiles, PROC_THREAD_FILE_COUNT))
		return;
	strncpy(out->name, comp, PROC_NAME_MAX - 1);
	if (synth_next_component(path, &pos, comp, sizeof(comp)))
		return;
	out->type = PROC_NODE_THREAD_FILE;
}

// ── Task lookup ─────────────────────────────────────────────────────────────

// Is this task something /proc should show?
//
// kTaskList is APPEND-ONLY — nothing has ever unlinked from it, so it holds
// every task the system has ever created, including long-dead reaped ones.
// That is a pre-existing leak, not procfs's to fix, but /proc must not put
// the corpses on display: a reaped task is not a process. The distinction is
// available without extra bookkeeping — scheduler_reap_zombie_thread sets the
// thread's state to NONE when it reaps, so:
//
//   not exited                      → live task, show it
//   exited, thread still ZOMBIE     → a real zombie awaiting its parent, show it
//   exited, thread NONE             → reaped and gone, hide it
static bool proc_task_is_visible(task_t *t)
{
	if (t == NULL || t == (task_t *)NO_TASK)
		return false;
	if (!t->exited)
		return true;
	return (t->threads != NULL && t->threads->threadState == THREAD_STATE_ZOMBIE);
}

static task_t *proc_first_task(void)
{
	task_t *t = kTaskList;
	return (t == NULL || t == (task_t *)NO_TASK) ? NULL : t;
}

static task_t *proc_next_task(task_t *t)
{
	if (t == NULL)
		return NULL;
	task_t *n = (task_t *)t->next;
	return (n == NULL || n == (task_t *)NO_TASK) ? NULL : n;
}

static task_t *proc_find_task(uint64_t taskID)
{
	for (task_t *t = proc_first_task(); t != NULL; t = proc_next_task(t))
		if (t->taskID == taskID && proc_task_is_visible(t))
			return t;
	return NULL;
}

// A task's threads. thread_t's prev/next are the SCHEDULER QUEUE links —
// following them walks whatever queue a thread currently sits in, not the
// task's threads — so the chain here is `taskNext`, the per-task list added
// when ring-3 threads landed (2026-08-02). This function's older comment
// predicted exactly that ("when real multi-threading lands, task_t needs its
// own thread chain and this is the one function that has to learn about it"),
// and it was right: teaching this one walk is what made every thread in a
// task visible to /proc, and therefore to top.
static thread_t *proc_task_thread(task_t *t, uint64_t threadID)
{
	if (t == NULL)
		return NULL;
	for (thread_t *th = t->threads; th != NULL; th = th->taskNext)
		if (th->threadID == threadID)
			return th;
	return NULL;
}

// How many threads this task owns, and their combined CPU time.
//
// A process's CPU is the SUM of its threads — that is what every other
// system reports (Linux's /proc/<pid>/stat aggregates the whole thread
// group; times() and getrusage(RUSAGE_SELF) do the same), and it is why a
// six-thread program legitimately reads 600%. Per-thread detail lives one
// level down in /<id>/thread/<tid>, so a reader can have either view.
//
// Without this walk the numbers lie in a particularly confusing way: the
// only visible thread would be the FIRST one, which in a typical worker
// program is the one parked in join doing nothing — so a task burning six
// cores reports 0%. (`hog &` did exactly that, 2026-08-02.)
static uint32_t proc_task_thread_count(task_t *t)
{
	uint32_t n = 0;
	for (thread_t *th = (t ? t->threads : NULL); th != NULL; th = th->taskNext)
		n++;
	return n;
}

static uint64_t proc_task_run_cycles(task_t *t)
{
	uint64_t cycles = 0;
	for (thread_t *th = (t ? t->threads : NULL); th != NULL; th = th->taskNext)
		cycles += th->runCycles;
	return cycles;
}

// The old `ticks` counter, summed the same way runtime_us is — the two
// CPU-time fields in the status file must describe the same set of threads
// or they contradict each other under load.
static uint64_t proc_task_run_ticks(task_t *t)
{
	uint64_t ticks = 0;
	for (thread_t *th = (t ? t->threads : NULL); th != NULL; th = th->taskNext)
		ticks += th->totalRunTicks;
	return ticks;
}

// Which thread SPEAKS for the task: the most alive one. The task status
// file already sums CPU across all threads (the doctrine above), so the
// per-thread fields alongside it (state, core) must describe the same
// program — not whichever thread happens to sit first in the list. hog
// proved the point on 2026-08-07: its launch thread sleeps forever on the
// core it was born on while its worker burns a different core flat out,
// and the first-thread report dressed a 100%-CPU task as "isleep, core 1".
// Ranking: running > runnable > usleep (imminent work, Linux's D) >
// isleep (parked) > stopped > zombie > none. First thread at the highest
// rank wins a tie — stable, and ties above "runnable" are momentary.
static int proc_state_rank(eThreadState s)
{
	switch (s)
	{
		case THREAD_STATE_RUNNING:  return 6;
		case THREAD_STATE_RUNNABLE: return 5;
		case THREAD_STATE_USLEEP:   return 4;
		case THREAD_STATE_ISLEEP:   return 3;
		case THREAD_STATE_STOPPED:  return 2;
		case THREAD_STATE_ZOMBIE:   return 1;
		default:                    return 0;
	}
}

static thread_t *proc_task_liveliest_thread(task_t *t)
{
	thread_t *best = NULL;
	int bestRank = -1;
	for (thread_t *th = (t ? t->threads : NULL); th != NULL; th = th->taskNext)
	{
		int rank = proc_state_rank(th->threadState);
		if (rank > bestRank)
		{
			best = th;
			bestRank = rank;
		}
	}
	return best;
}

// Copy a NUL-terminated string OUT OF A TASK'S ADDRESS SPACE into `out`.
//
// Necessary because a task_t field being kernel-readable does not make what it
// POINTS AT kernel-readable. task->argv is the trap: the pointer ARRAY is a
// kmalloc'd kernel object, but every pointer inside it is a TASK virtual
// address (TASK_ARGV_VIRT + offset — see task_create), because the program has
// to see a self-consistent argv in its own address space. Dereferencing one
// from kernel context does not fault; it reads whatever the vestigial low
// identity window happens to map at that address, which during bring-up meant
// husk's cmdline reporting "/idle7" — another task's argv blob entirely.
//
// So: walk the TASK'S OWN page tables to get the physical page, then read
// through the HHDM alias, which is the technique CLAUDE.md prescribes for
// exactly this. Byte at a time — the strings are short (TASK_MAX_PATH_LEN),
// and per-byte translation makes a string that straddles a page boundary
// correct for free instead of correct by luck.
static bool proc_copy_task_string(task_t *task, uintptr_t task_va,
                                  char *out, size_t outlen)
{
	size_t n = 0;

	if (out == NULL || outlen == 0)
		return false;
	out[0] = '\0';
	if (task == NULL || task->pml4v == NULL || task_va == 0)
		return false;

	// Page-sized chunks through the fault-proof reader (PR #26, round seven
	// — this copier had the same walk-vs-burial window as its heap sibling
	// since birth, booked in DEBTS and paid the day the ruling arrived):
	// copy what a page offers, scan for the terminator, stop at NUL or at
	// the first unreadable page — truncated but honest, as it always was.
	while (n + 1 < outlen)
	{
		uintptr_t va = task_va + n;
		size_t want = PAGE_SIZE - (size_t)(va & 0xFFF);
		if (want > outlen - 1 - n)
			want = outlen - 1 - n;

		if (!allocator_copy_from_task_va(task->pml4v, va, out + n, want))
			break;   // unmapped or mid-burial — the string ends here

		size_t k = 0;
		while (k < want && out[n + k] != '\0')
			k++;
		n += k;
		if (k < want)
			break;   // found the NUL inside this chunk
	}
	out[n] = '\0';
	return n > 0;
}

static const char *proc_state_name(eThreadState s)
{
	switch (s)
	{
		case THREAD_STATE_NONE:     return "none";
		case THREAD_STATE_RUNNING:  return "running";
		case THREAD_STATE_RUNNABLE: return "runnable";
		case THREAD_STATE_STOPPED:  return "stopped";
		case THREAD_STATE_USLEEP:   return "usleep";     // uninterruptable
		case THREAD_STATE_ISLEEP:   return "isleep";     // interruptable
		case THREAD_STATE_ZOMBIE:   return "zombie";
		default:                    return "unknown";
	}
}

static const char *proc_handle_type_name(handle_type_t t)
{
	switch (t)
	{
		case HANDLE_CONSOLE_IN:  return "console-in";
		case HANDLE_CONSOLE_OUT: return "console-out";
		case HANDLE_CONSOLE_ERR: return "console-err";
		case HANDLE_PIPE_READ:   return "pipe-read";
		case HANDLE_PIPE_WRITE:  return "pipe-write";
		case HANDLE_FILE:        return "file";
		case HANDLE_DIR:         return "dir";
		default:                 return "none";
	}
}

// ── The file generators ─────────────────────────────────────────────────────
// Each fills a synth_text_t with the whole file. They run once, at open, under
// kKernelPML4 (open_do's kernel context) — so kernel structures are readable
// and nothing here may touch a user pointer.

// TSC cycles → microseconds, at the read boundary. Raw cycles never leave
// the kernel (the ABI speaks TIME — same doctrine as sleep's milliseconds):
// userland gets µs, and the TSC rate stays a kernel implementation detail.
static uint64_t proc_cycles_to_us(uint64_t cycles)
{
	uint64_t per_us = kCPUCyclesPerSecond / 1000000;
	return per_us ? cycles / per_us : 0;
}

// The per-core CPU-time ledger lived here as proc_gen_cores until 2026-08-12;
// it is sys_gen_cpu_time now (sysfs.c), one file per core under /sys/cpu.

static void proc_gen_task_status(synth_text_t *t, task_t *task)
{
	// The liveliest thread represents the task (see the helper's doctrine) —
	// its state and core answer "what is this PROGRAM doing, and where",
	// consistent with the summed runtime_us/ticks below. Per-thread truth
	// lives in /<id>/thread/<tid>/status for anyone who wants the roster.
	thread_t *th = proc_task_liveliest_thread(task);

	// Same settle as /sys/cpu/<n>/time (rate-limited to once a tick):
	// runtime_us below must not be a scheduler-pass stale on a monopolized core.
	mpAcctSettleAll();

	synth_text_addf(t, "task\t%lu\n", task->taskID);
	synth_text_addf(t, "name\t%s\n", task->exename[0] ? task->exename : "(none)");
	synth_text_addf(t, "state\t%s\n", th ? proc_state_name(th->threadState) : "none");
	synth_text_addf(t, "parent\t%lu\n",
	           task->parentTask ? task->parentTask->taskID : 0);
	synth_text_addf(t, "kernel\t%s\n", task->kernelTask ? "yes" : "no");
	// The console-ownership bits SIGINT.md introduced, per-terminal since the
	// VT slice. Worth surfacing: "which task would Ctrl+C hit, and on which
	// terminal" is exactly the sort of question /proc exists to answer
	// without a debugger. tty is 1-based here because that is what the
	// Alt+F keys say (task_tty resolves the no-terminal default to tty1).
	synth_text_addf(t, "tty\t%u\n", task_tty(task)->index + 1);
	synth_text_addf(t, "foreground\t%s\n",
	           (task_tty(task)->fgTask == task) ? "yes" : "no");
	synth_text_addf(t, "shell\t%s\n", task->controllingShell ? "yes" : "no");
	synth_text_addf(t, "threads\t%u\n", proc_task_thread_count(task));
	synth_text_addf(t, "heap\t%p-%p\n", (void *)task->heapStart, (void *)task->heapEnd);
	synth_text_addf(t, "entry\t%p\n", (void *)task->entryPoint);
	if (task->loadBias)
		synth_text_addf(t, "bias\t%p\n", (void *)task->loadBias);
	synth_text_addf(t, "faults\t%u minor %u major\n", task->minorFaults, task->majorFaults);
	synth_text_addf(t, "switches\t%u\n", task->cSwitches);
	// Where the task's thread last ran — the answer to "whose plate did
	// this time come off", without which per-core arithmetic is guesswork.
	if (th != NULL)
		synth_text_addf(t, "core\t%u\n", th->lastRunApicID);
	// Real CPU time, charged at context-switch boundaries (thread.h has the
	// doctrine). `ticks` below is the old sampling counter — kept because
	// removing a field is an ABI event, but runtime_us is the honest one.
	//
	// SUMMED ACROSS THE TASK'S THREADS, which is what makes this number mean
	// "what this program cost" rather than "what its first thread cost" —
	// and on a threaded program those differ wildly, because the first
	// thread is usually the one waiting.
	synth_text_addf(t, "runtime_us\t%lu\n", proc_cycles_to_us(proc_task_run_cycles(task)));
	if (th != NULL)
		synth_text_addf(t, "ticks\t%lu\n", proc_task_run_ticks(task));
	// A live task has no exit status; saying "-" beats printing a zero that
	// looks like "exited successfully".
	if (task->exited)
		synth_text_addf(t, "exit\t%lu\n", task->retVal);
	else
		synth_text_addf(t, "exit\t-\n");
}

static void proc_gen_cmdline(synth_text_t *t, task_t *task)
{
	// One argument per LINE, not NUL-separated (PROC.md): the unambiguity is
	// the same and `cat` renders it correctly, which Linux's version does not.
	if (task->argv == NULL || task->argc <= 0)
	{
		// argv[0] is not always populated for kernel-launched tasks; the
		// executable name is still the honest answer to "what is this".
		if (task->exename[0])
			synth_text_addf(t, "%s\n", task->exename);
		return;
	}
	for (int i = 0; i < task->argc && i < PROC_MAX_LIST_WALK; i++)
	{
		// argv[i] is a TASK virtual address, not a kernel one — it must be
		// translated, never dereferenced. See proc_copy_task_string.
		char arg[TASK_MAX_PATH_LEN];
		if (proc_copy_task_string(task, (uintptr_t)task->argv[i], arg, sizeof(arg)))
			synth_text_addf(t, "%s\n", arg);
	}
}

static void proc_gen_cwd(synth_text_t *t, task_t *task)
{
	synth_text_addf(t, "%s\n", (task->cwd != NULL) ? task->cwd : "/");
}

// /proc/<pid>/tty — the CONTROLLING TERMINAL's geometry and state, reached
// through /proc/self so a program (or a human at a shell) asks about ITS
// terminal without knowing any handle. This is os64's answer to Unix's
// TIOCGWINSZ ioctl (4.3BSD's winsize-on-the-fd), reshaped as a file per the
// house doctrine: reads of state are files, actions are ctl verbs, only
// hot-path or binary-shaped things earn syscalls — and a program reads its
// geometry once at startup or per redraw, never in a loop. Same key<TAB>value
// grammar as status, so the same five-line parser reads it. Generated fresh
// per open: the day GUI terminal windows resize, this file is already right.
static void proc_gen_tty(synth_text_t *t, task_t *task)
{
	tty_t *tty = task_tty(task);

	synth_text_addf(t, "tty\t%u\n", tty->index + 1);   // 1-based, as Alt+F says
	synth_text_addf(t, "rows\t%u\n", tty->rows);
	synth_text_addf(t, "cols\t%u\n", tty->cols);
	// Whether the glass is currently showing this terminal — an app can skip
	// expensive redraw work while nobody is looking (and a human debugging
	// "why is my output not on screen" gets the answer in one cat).
	synth_text_addf(t, "focused\t%s\n", (kTTYFocused == tty) ? "yes" : "no");
	synth_text_addf(t, "state\t%s\n", (tty->state == TTY_LIVE) ? "live" : "dormant");
	// How much history Shift+PgUp can reach right now — a pager that knows
	// the terminal already holds N lines can choose not to repeat them.
	synth_text_addf(t, "scrollback\t%u\n", tty->hist_lines);
	synth_text_addf(t, "fg_task\t%lu\n",
	           tty->fgTask ? ((task_t *)tty->fgTask)->taskID : 0);
}

static void proc_gen_handles(synth_text_t *t, task_t *task)
{
	// "handle<TAB>type<TAB>detail" — the detail column is whatever the tag
	// makes meaningful, and is simply absent for the console tags (they
	// reference no object at all; see handle.h).
	for (int i = 0; i < TASK_MAX_HANDLES; i++)
	{
		handle_t *h = &task->handles[i];
		if (h->type == HANDLE_NONE)
			continue;

		if (h->type == HANDLE_FILE && h->object != NULL)
		{
			vfs_file_t *f = (vfs_file_t *)h->object;
			// f_path is the fs-local TAIL the mount router handed the driver,
			// so it has lost its mount prefix — "/bin/ls" here could be on any
			// mounted filesystem. Printing what we have beats printing nothing.
			synth_text_addf(t, "%d\t%s\t%s\n", i, proc_handle_type_name(h->type),
			           (f->f_path != NULL) ? f->f_path : "(unnamed)");
		}
		else if (h->type == HANDLE_DIR && h->object != NULL)
		{
			vfs_directory_t *d = (vfs_directory_t *)h->object;
			synth_text_addf(t, "%d\t%s\t%s\n", i, proc_handle_type_name(h->type),
			           (d->f_path != NULL) ? d->f_path : "(unnamed)");
		}
		else if (h->type == HANDLE_PIPE_READ || h->type == HANDLE_PIPE_WRITE)
		{
			// The pipe's identity is its object pointer — two tasks showing the
			// same value are the two ends of one pipeline, which is exactly what
			// you want to see when a pipeline hangs.
			synth_text_addf(t, "%d\t%s\t%p\n", i, proc_handle_type_name(h->type), h->object);
		}
		else
		{
			synth_text_addf(t, "%d\t%s\n", i, proc_handle_type_name(h->type));
		}
	}
}

static void proc_gen_maps(synth_text_t *t, task_t *task)
{
	// "start-end<TAB>prot<TAB>flags" — the address space as a table. NOT the
	// memory itself: that is what the name `mem` is reserved for, the day a
	// debugger wants Killian's original file back (PROC.md).
	if (task->mmaps == NULL)
		return;

	int guard = 0;
	for (dlist_node_t *n = task->mmaps->head;
	     n != NULL && guard < PROC_MAX_LIST_WALK;
	     n = n->next, guard++)
	{
		vma_t *v = (vma_t *)n->data;
		if (v == NULL)
			continue;

		char prot[4];
		prot[0] = (v->prot & PROT_READ)  ? 'r' : '-';
		prot[1] = (v->prot & PROT_WRITE) ? 'w' : '-';
		prot[2] = (v->prot & PROT_EXEC)  ? 'x' : '-';
		prot[3] = '\0';

		synth_text_addf(t, "%p-%p\t%s\t%s%s%s\n",
		           (void *)v->start, (void *)v->end, prot,
		           (v->flags & MAP_SHARED) ? "shared" : "private",
		           v->cow ? ",cow" : "",
		           (v->flags & MAP_SHARED_LIBRARY) ? ",lib" : "");
	}
}

// Copy `len` bytes OUT OF A TASK'S ADDRESS SPACE — the fixed-length sibling
// of proc_copy_task_string. Per-page, because a struct that straddles a page
// boundary must be correct for free rather than by luck, and because an
// untouched (demand-paged) page must end the copy honestly.
//
// The walk AND the copy both live inside allocator_copy_from_task_va now
// (PR #26, rounds two and seven): a concurrent os64_unmap could fault the
// copy, and a concurrent phase-2 BURIAL could fault the walk itself — the
// arena's recycled table pages hold garbage entries that point anywhere, and
// following one through the HHDM is a ring-0 fault one hop removed. Every
// page, table or leaf, is liveness-verified under the allocator's lock
// before it is touched. A user program must not be able to panic the kernel
// by unmapping — or by dying — while we read what it told us to read.
static bool proc_copy_task_bytes(task_t *task, uintptr_t task_va,
                                 void *out, size_t len)
{
	uint8_t *dst = (uint8_t *)out;
	size_t done = 0;

	if (out == NULL || len == 0 || task == NULL || task->pml4v == NULL || task_va == 0)
		return false;

	while (done < len)
	{
		uintptr_t va = task_va + done;
		size_t offset = (size_t)(va & 0xFFF);
		size_t chunk = 0x1000 - offset;
		if (chunk > len - done)
			chunk = len - done;

		if (!allocator_copy_from_task_va(task->pml4v, va, dst + done, chunk))
			return false;   // unmapped, freed, or buried — "unreadable" is honest
		done += chunk;
	}
	return true;
}

// /proc/<id>/heap — the userland allocator's own numbers, rendered by the
// kernel (SYSCALL_HEAP_REPORT, 2026-08-15).
//
// THE ONLY FILE IN /proc WHOSE CONTENT COMES FROM RING 3. A heap's shape —
// how many blocks are live, how fragmented the free space is, how many
// regions have gone home to the kernel — is known only to the allocator that
// owns it, and that allocator is libos64's malloc. So malloc publishes the
// address of one struct at startup and the kernel renders the file: procfs
// keeps the pen, the format, and the key<TAB>value grammar every other file
// here uses, and the application writes not one line of it.
//
// A program with no libos64 heap (a raw fixture, a foreign binary) says so
// plainly rather than pretending to have an empty heap.
static void proc_gen_heap(synth_text_t *t, task_t *task)
{
	os64_heap_report_t r;

	if (task->heapReportVirt == 0)
	{
		synth_text_addf(t, "heap\tnone\n");
		synth_text_addf(t, "why\tthis task never registered one (no libos64 malloc)\n");
		return;
	}

	// SEQLOCK READ (PR #26, Codex's catch): the copy is byte-sequential and
	// `generation` sits at offset 16, long before the counters — so a single
	// pass can capture an EVEN generation and then counters a writer is
	// already mutating: a mixed snapshot stamped "torn no", complete with a
	// false "audit BROKEN". The classic cure: copy TWICE and accept only when
	// both generations match and are even — equality across the second copy's
	// generation read brackets the first copy's entire window. A heap busy
	// enough to defeat four attempts is reported torn, never guessed at.
	os64_heap_report_t check;
	bool stable = false;
	for (int attempt = 0; attempt < 4 && !stable; attempt++)
	{
		if (!proc_copy_task_bytes(task, task->heapReportVirt, &r, sizeof(r)) ||
		    !proc_copy_task_bytes(task, task->heapReportVirt, &check, sizeof(check)))
		{
			synth_text_addf(t, "heap\tunreadable\n");
			synth_text_addf(t, "report_at\t%p\n", (void *)task->heapReportVirt);
			return;
		}
		stable = ((r.generation & 1) == 0) && (check.generation == r.generation);
	}

	if (r.magic != OS64_HEAP_REPORT_MAGIC || r.version != OS64_HEAP_REPORT_VERSION)
	{
		// Registered, but what is there is not (or is no longer) a report this
		// kernel knows how to read. Print what was found instead of numbers
		// invented from it — a file that guesses is worse than one that admits.
		synth_text_addf(t, "heap\tunrecognized\n");
		synth_text_addf(t, "report_at\t%p\n", (void *)task->heapReportVirt);
		synth_text_addf(t, "magic\t%p\n", (void *)r.magic);
		synth_text_addf(t, "version\t%u\n", r.version);
		return;
	}

	// The report is a photograph of a moving thing. `torn no` now means the
	// seqlock read above PROVED the numbers coexisted; anything less honest
	// says so out loud rather than hiding it.
	synth_text_addf(t, "torn\t%s\n", stable ? "no" : "yes");
	synth_text_addf(t, "generation\t%lu\n", r.generation);

	synth_text_addf(t, "regions\t%lu\n", r.regions);
	synth_text_addf(t, "pools\t%lu\n", r.region_pools);
	synth_text_addf(t, "dedicated\t%lu\n", r.region_dedicated);
	synth_text_addf(t, "mapped\t%lu\n", r.bytes_mapped);
	synth_text_addf(t, "live\t%lu\n", r.bytes_live);
	synth_text_addf(t, "free\t%lu\n", r.bytes_free);
	synth_text_addf(t, "overhead\t%lu\n", r.bytes_overhead);
	synth_text_addf(t, "virgin\t%lu\n", r.bytes_virgin);

	// The audit identity, CHECKED here rather than merely printed: every
	// mapped byte must be live, free, overhead, or never-carved. A torn read
	// can break it innocently, so a torn snapshot says so instead of crying
	// wolf. (os64/memory.h does the same for the physical allocator: a report
	// that can catch its own author lying is worth four extra lines.)
	uint64_t accounted = r.bytes_live + r.bytes_free + r.bytes_overhead + r.bytes_virgin;
	if (accounted == r.bytes_mapped)
		synth_text_addf(t, "audit\tok\n");
	else if (!stable)
		synth_text_addf(t, "audit\ttorn (off by %ld)\n",
		                (int64_t)accounted - (int64_t)r.bytes_mapped);
	else
		synth_text_addf(t, "audit\tBROKEN (off by %ld)\n",
		                (int64_t)accounted - (int64_t)r.bytes_mapped);
	synth_text_addf(t, "blocks_live\t%lu\n", r.blocks_live);
	synth_text_addf(t, "blocks_free\t%lu\n", r.blocks_free);
	synth_text_addf(t, "largest_free\t%lu\n", r.largest_free);
	synth_text_addf(t, "high_water\t%lu\n", r.high_water);

	// Fragmentation, pre-computed so nobody does Linux-style column
	// arithmetic on a report (the doctrine os64/memory.h opens with): the
	// percentage of free bytes NOT in the single largest free block. 0% means
	// the free space is one contiguous piece; 90% means it is confetti.
	if (r.bytes_free > 0 && r.bytes_free >= r.largest_free)
		synth_text_addf(t, "fragmentation_pct\t%lu\n",
		                ((r.bytes_free - r.largest_free) * 100) / r.bytes_free);
	else
		synth_text_addf(t, "fragmentation_pct\t0\n");

	synth_text_addf(t, "calls_malloc\t%lu\n", r.calls_malloc);
	synth_text_addf(t, "calls_free\t%lu\n", r.calls_free);
	synth_text_addf(t, "calls_calloc\t%lu\n", r.calls_calloc);
	synth_text_addf(t, "calls_realloc\t%lu\n", r.calls_realloc);
	synth_text_addf(t, "regions_taken\t%lu\n", r.calls_map);
	synth_text_addf(t, "regions_returned\t%lu\n", r.calls_unmap);

	// The live-block histogram, one line per non-empty bucket, keyed by the
	// bucket's floor: "live.64  12" = twelve live blocks whose payload
	// capacity is 64..127 bytes. Empty buckets are omitted — a file nobody
	// has to scroll past zeros to read.
	for (uint32_t i = 0; i < OS64_HEAP_CLASSES; i++)
	{
		if (r.live_by_class[i] == 0)
			continue;
		synth_text_addf(t, "live.%lu\t%lu\n",
		                (uint64_t)1 << (i + OS64_HEAP_CLASS_MIN_SHIFT),
		                r.live_by_class[i]);
	}
}

static void proc_gen_thread_status(synth_text_t *t, task_t *task, thread_t *th)
{
	mpAcctSettleAll();   // same freshness contract as the task status file

	synth_text_addf(t, "thread\t%lu\n", th->threadID);
	synth_text_addf(t, "task\t%lu\n", task->taskID);
	synth_text_addf(t, "state\t%s\n", proc_state_name(th->threadState));
	// mp_apic is the thread's PINNED AFFINITY, not the core it is running on —
	// a distinction worth the extra word, because labelling it "core" printed
	// 18446744073709551615 for every ordinary thread during bring-up (that is
	// THREAD_NO_AFFINITY, which means "any core", not core number 2^64-1).
	// Which core a thread is on RIGHT NOW lives in each core's CLS and would
	// have to be found by scanning them; it joins this file when something
	// needs it.
	if (th->mp_apic == THREAD_NO_AFFINITY)
		synth_text_addf(t, "affinity\tany\n");
	else
		synth_text_addf(t, "affinity\t%lu\n", th->mp_apic);
	synth_text_addf(t, "idle\t%s\n", th->idleThread ? "yes" : "no");
	synth_text_addf(t, "core\t%u\n", th->lastRunApicID);   // last dispatched here
	synth_text_addf(t, "ring\t%u\n", (unsigned)(th->regs.CS & 3));
	synth_text_addf(t, "rip\t%p\n", (void *)th->regs.RIP);
	synth_text_addf(t, "rsp\t%p\n", (void *)th->regs.RSP);
	synth_text_addf(t, "ticks\t%lu\n", th->totalRunTicks);
	synth_text_addf(t, "runtime_us\t%lu\n", proc_cycles_to_us(th->runCycles));
	synth_text_addf(t, "signals\t%p\n", (void *)th->signals.sigind);
}

// ── ctl ─────────────────────────────────────────────────────────────────────

// The vocabulary, in one table, used for BOTH directions: the parser matches
// against it and a read of ctl prints it. A control surface that describes
// itself cannot drift out of sync with its documentation.
typedef struct
{
	const char *word;
	uint32_t    signal;    // the pending bit a write raises
	const char *effect;    // what a reader is told it does
} proc_ctl_verb_t;

static const proc_ctl_verb_t kProcCtlVerbs[] = {
	{ "kill",      SIGKILL, "terminate the task (uncatchable), exit 137" },
	{ "interrupt", SIGINT,  "terminate the task (catchable later), exit 130" },
};
#define PROC_CTL_VERB_COUNT (sizeof(kProcCtlVerbs) / sizeof(kProcCtlVerbs[0]))

static void proc_gen_ctl(synth_text_t *t, task_t *task)
{
	(void)task;
	// "word<TAB>effect" — same tabular shape as every other file here, so the
	// same five-line parser reads it.
	for (size_t i = 0; i < PROC_CTL_VERB_COUNT; i++)
		synth_text_addf(t, "%s\t%s\n", kProcCtlVerbs[i].word, kProcCtlVerbs[i].effect);
}

// A ctl write NEVER touches a scheduler queue — it ORs a pending-signal bit
// and returns, exactly as the keyboard IRQ does for Ctrl+C. The queue surgery
// and the reaping happen where they are already safe: at the per-core
// scheduler checkpoint under kSchedulerSwitchTasksLock, and at the victim's
// own syscall boundary. See PROC.md and SIGINT.md — Ctrl+C paid for all of
// this machinery three commits ago, and ctl simply inherits it.
//
// Returns the number of bytes consumed (the whole write, on success), or -1
// for a word this kernel does not know.
static int proc_ctl_command(task_t *task, const char *word, size_t consumed)
{
	for (size_t i = 0; i < PROC_CTL_VERB_COUNT; i++)
	{
		if (strcmp(word, kProcCtlVerbs[i].word) != 0)
			continue;

		if (task->threads == NULL)
			return -1;   // nothing to signal

		// Refuse to signal the kernel's own tasks. A `ctl kill` aimed at the
		// idle task or ktask would take the machine down, and "the filesystem
		// let me shoot the kernel" is not a tinkering experience anyone wants.
		if (task->kernelTask)
		{
			printd(DEBUG_SYSCALL, "proc: refusing '%s' on kernel task %lu (%s)\n",
			       word, task->taskID, task->exename);
			return -1;
		}

		// Every thread, not just the first: a ctl verb aimed at a task is
		// aimed at the whole task. Killing only the main thread left the
		// workers running (and four cores hot) while the task showed as a
		// zombie — see task_signal_all_threads.
		task_signal_all_threads(task, kProcCtlVerbs[i].signal);
		printd(DEBUG_SYSCALL | DEBUG_TASK, "proc: ctl '%s' -> task %lu (%s), %u thread%s signalled\n",
		       word, task->taskID, task->exename,
		       proc_task_thread_count(task),
		       proc_task_thread_count(task) == 1 ? "" : "s");
		return (int)consumed;
	}
	return -1;
}

// ── The open file object ────────────────────────────────────────────────────

typedef struct
{
	synth_snapshot_t snap;  // MUST be first — the generic fops see only this
	                        // head, and close frees the whole struct by it
	bool     is_ctl;        // writes to this handle are commands
	uint64_t taskID;        // ctl's target, re-resolved at write time
} proc_file_handle_t;

static int proc_open(vfs_file_t **vfs_file, const char *path, const char *mode,
                     vfs_filesystem_t *vfs_fs)
{
	proc_path_t pp;

	// /proc is read-only except for ctl, and even ctl is not "written to" in
	// the create/truncate/append sense — it is commanded. Rejecting the write
	// modes here, at the boundary, beats a write that silently goes nowhere.
	if (mode == NULL || mode[1] != '\0' || (mode[0] != 'r' && mode[0] != 'w'))
		return -1;

	proc_parse_path(path, &pp);
	if (pp.type != PROC_NODE_TASK_FILE && pp.type != PROC_NODE_THREAD_FILE)
		return -1;   // directories go through dops; everything else is not a file

	task_t *task = proc_find_task(pp.taskID);
	thread_t *th = NULL;
	if (task == NULL)
		return -1;

	if (pp.type == PROC_NODE_THREAD_FILE)
	{
		th = proc_task_thread(task, pp.threadID);
		if (th == NULL)
			return -1;
	}

	bool is_ctl = (pp.type == PROC_NODE_TASK_FILE && strcmp(pp.name, "ctl") == 0);
	if (mode[0] == 'w' && !is_ctl)
		return -1;   // only ctl accepts a write, ever

	// Generate the whole file NOW, into a snapshot (PROC.md: an internally
	// consistent file beats a fresh one — you can never read the first half of
	// one task's status and the second half of its successor's).
	synth_text_t text;
	if (!synth_text_init(&text, 512))
		return -1;

	if (pp.type == PROC_NODE_THREAD_FILE)
	{
		proc_gen_thread_status(&text, task, th);
	}
	else if (strcmp(pp.name, "status") == 0)   proc_gen_task_status(&text, task);
	else if (strcmp(pp.name, "cmdline") == 0)  proc_gen_cmdline(&text, task);
	else if (strcmp(pp.name, "cwd") == 0)      proc_gen_cwd(&text, task);
	else if (strcmp(pp.name, "handles") == 0)  proc_gen_handles(&text, task);
	else if (strcmp(pp.name, "maps") == 0)     proc_gen_maps(&text, task);
	else if (strcmp(pp.name, "heap") == 0)     proc_gen_heap(&text, task);
	else if (strcmp(pp.name, "tty") == 0)      proc_gen_tty(&text, task);
	else if (is_ctl)                           proc_gen_ctl(&text, task);
	else
	{
		kfree(text.buf);
		return -1;
	}

	// The snapshot handle + vfs_file wiring is synthfs's job; the two fields
	// past the snapshot head are ours.
	proc_file_handle_t *h = synth_snapshot_publish(vfs_file, &text, path, vfs_fs,
	                                               sizeof(proc_file_handle_t),
	                                               FILETYPE_PROCFILE);
	                                               // ^ the enum's oldest fossil, finally true
	if (h == NULL)
		return -1;

	h->is_ctl = is_ctl;
	h->taskID = pp.taskID;
	return 0;
}

// Writing to a /proc file is a COMMAND, not a store. Only ctl accepts one.
//
// The buffer arrives as whatever the writer sent — `echo kill` sends "kill\n",
// a program might send "kill" with no newline, husk might one day send several.
// So: take the first whitespace-delimited word, act on it, and report the whole
// write as consumed. A partial write would make `echo` retry the tail, which is
// exactly the wrong thing for a command.
static int proc_write(vfs_file_t *vfs_file, const void *buffer, size_t size)
{
	proc_file_handle_t *h = (proc_file_handle_t *)vfs_file->handle;
	char word[PROC_NAME_MAX];
	size_t i = 0, n = 0;

	if (!h->is_ctl || buffer == NULL || size == 0)
		return -1;

	const char *b = (const char *)buffer;

	while (i < size && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r'))
		i++;
	while (i < size && b[i] != ' ' && b[i] != '\t' && b[i] != '\n' && b[i] != '\r')
	{
		if (n + 1 < sizeof(word))
			word[n++] = b[i];
		i++;
	}
	word[n] = '\0';

	if (n == 0)
		return (int)size;   // whitespace only — consumed, nothing commanded

	// Re-resolve the target NOW rather than trusting a task_t* captured at
	// open: between `open` and `write` the task may have exited and been
	// reaped, and a stale pointer into freed memory is precisely the wild
	// dereference the lazy HHDM exists to catch. An ID cannot go stale.
	task_t *task = proc_find_task(h->taskID);
	if (task == NULL)
		return -1;   // the task is gone — the command has no target

	return proc_ctl_command(task, word, size);
}

// ── Directory operations ────────────────────────────────────────────────────

typedef struct
{
	proc_path_t path;
	// The listing cursor. For fixed name tables it is an index; for the /proc
	// root it is "the last task ID handed out", which is stable across the
	// list growing underneath us (kTaskList only ever appends) in a way an
	// index is not.
	int      index;
	uint64_t lastTaskID;
	bool     started;
} proc_dir_handle_t;

static int proc_open_dir(vfs_directory_t **vfs_dir, const char *path,
                         vfs_filesystem_t *vfs_fs)
{
	proc_path_t pp;
	proc_parse_path(path, &pp);

	if (pp.type != PROC_NODE_ROOT && pp.type != PROC_NODE_TASK &&
	    pp.type != PROC_NODE_THREADDIR && pp.type != PROC_NODE_THREAD)
		return -1;

	// A task directory only exists while its task does.
	if (pp.type != PROC_NODE_ROOT)
	{
		task_t *task = proc_find_task(pp.taskID);
		if (task == NULL)
			return -1;
		if ((pp.type == PROC_NODE_THREAD) &&
		    proc_task_thread(task, pp.threadID) == NULL)
			return -1;
	}

	proc_dir_handle_t *h = kmalloc(sizeof(proc_dir_handle_t));
	*vfs_dir = kmalloc(sizeof(vfs_directory_t));
	if (h == NULL || *vfs_dir == NULL)
	{
		if (h) kfree(h);
		if (*vfs_dir) kfree(*vfs_dir);
		*vfs_dir = NULL;
		return -1;
	}

	h->path = pp;
	h->index = 0;
	h->lastTaskID = 0;
	h->started = false;

	(*vfs_dir)->handle = h;
	(*vfs_dir)->f_path = (char *)path;   // same lifetime contract as files
	(*vfs_dir)->dops   = vfs_fs->dops;
	(*vfs_dir)->owner  = vfs_fs;
	return 0;
}

static int proc_read_dir(vfs_directory_t *vfs_dir, os64_dirent_t *entry)
{
	proc_dir_handle_t *h = (proc_dir_handle_t *)vfs_dir->handle;

	memset(entry, 0, sizeof(*entry));

	switch (h->path.type)
	{
		case PROC_NODE_ROOT:
		{
			// Walk kTaskList for the lowest visible task ID greater than the
			// last one we handed out. Cursor-by-ID rather than by index: the
			// list can grow between calls (a spawn), and an index would then
			// repeat or skip an entry. IDs only ever increase, so this is
			// stable — and it also sorts the listing, which an index does not.
			task_t *best = NULL;
			for (task_t *t = proc_first_task(); t != NULL; t = proc_next_task(t))
			{
				if (!proc_task_is_visible(t))
					continue;
				if (h->started && t->taskID <= h->lastTaskID)
					continue;
				if (best == NULL || t->taskID < best->taskID)
					best = t;
			}
			if (best == NULL)
				return 0;   // end of directory

			h->lastTaskID = best->taskID;
			h->started = true;
			entry->flags = OS64_DE_DIR;
			snprintf(entry->name, OS64_DIRENT_NAME_MAX, "%lu", best->taskID);
			return 1;
		}

		case PROC_NODE_TASK:
		{
			// The fixed file table, then the "thread" subdirectory last.
			if (h->index < (int)PROC_TASK_FILE_COUNT)
			{
				strncpy(entry->name, kProcTaskFiles[h->index], OS64_DIRENT_NAME_MAX);
				// Size 0 for every /proc file: the content does not exist until
				// something opens it, so any number here would be a guess. `ls`
				// prints 0; `cat` reads until EOF and never consults a size.
				entry->size = 0;
				h->index++;
				return 1;
			}
			if (h->index == (int)PROC_TASK_FILE_COUNT)
			{
				strncpy(entry->name, "thread", OS64_DIRENT_NAME_MAX);
				entry->flags = OS64_DE_DIR;
				h->index++;
				return 1;
			}
			return 0;
		}

		case PROC_NODE_THREADDIR:
		{
			// Every thread the task owns, walked through taskNext (the
			// per-task chain; prev/next belong to the run queues). h->index
			// is the caller's position, so each readdir call advances one
			// link — the list is short by nature and this keeps the handle
			// stateless beyond a counter.
			//
			// A thread that exits between two readdir calls simply stops
			// appearing; the walk re-runs from the head each time rather
			// than caching a pointer that could be freed underneath it.
			task_t *task = proc_find_task(h->path.taskID);
			if (task == NULL)
				return 0;
			thread_t *th = task->threads;
			for (int skip = 0; skip < h->index && th != NULL; skip++)
				th = th->taskNext;
			if (th == NULL)
				return 0;
			entry->flags = OS64_DE_DIR;
			snprintf(entry->name, OS64_DIRENT_NAME_MAX, "%lu", th->threadID);
			h->index++;
			return 1;
		}

		case PROC_NODE_THREAD:
		{
			if (h->index >= (int)PROC_THREAD_FILE_COUNT)
				return 0;
			strncpy(entry->name, kProcThreadFiles[h->index], OS64_DIRENT_NAME_MAX);
			entry->size = 0;
			h->index++;
			return 1;
		}

		default:
			return -1;
	}
}

static int proc_close_dir(vfs_directory_t *vfs_dir)
{
	if (vfs_dir->handle != NULL)
		kfree(vfs_dir->handle);
	kfree(vfs_dir);
	return 0;
}

// stat is readdir for exactly one name (vfs.h): fill the same os64_dirent_t
// for whatever the path names, file or directory.
static int proc_stat(const char *path, os64_dirent_t *entry, vfs_filesystem_t *vfs_fs)
{
	proc_path_t pp;
	(void)vfs_fs;

	proc_parse_path(path, &pp);
	memset(entry, 0, sizeof(*entry));

	switch (pp.type)
	{
		case PROC_NODE_ROOT:
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "proc", OS64_DIRENT_NAME_MAX);
			return 0;

		case PROC_NODE_TASK:
			if (proc_find_task(pp.taskID) == NULL)
				return -1;
			entry->flags = OS64_DE_DIR;
			snprintf(entry->name, OS64_DIRENT_NAME_MAX, "%lu", pp.taskID);
			return 0;

		case PROC_NODE_THREADDIR:
			if (proc_find_task(pp.taskID) == NULL)
				return -1;
			entry->flags = OS64_DE_DIR;
			strncpy(entry->name, "thread", OS64_DIRENT_NAME_MAX);
			return 0;

		case PROC_NODE_THREAD:
		{
			task_t *task = proc_find_task(pp.taskID);
			if (task == NULL || proc_task_thread(task, pp.threadID) == NULL)
				return -1;
			entry->flags = OS64_DE_DIR;
			snprintf(entry->name, OS64_DIRENT_NAME_MAX, "%lu", pp.threadID);
			return 0;
		}

		case PROC_NODE_TASK_FILE:
		case PROC_NODE_THREAD_FILE:
		{
			task_t *task = proc_find_task(pp.taskID);
			if (task == NULL)
				return -1;
			if (pp.type == PROC_NODE_THREAD_FILE &&
			    proc_task_thread(task, pp.threadID) == NULL)
				return -1;
			// Size 0, for the same reason readdir reports 0 — see above.
			strncpy(entry->name, pp.name, OS64_DIRENT_NAME_MAX);
			return 0;
		}

		default:
			return -1;
	}
}

vfs_file_operations_t proc_fops = {
	.open  = proc_open,
	.read  = synth_snapshot_read,    // the generic snapshot fops (synthfs.h):
	.write = proc_write,             // only open and the ctl write are ours
	.seek  = synth_snapshot_seek,
	.tell  = synth_snapshot_tell,
	.close = synth_snapshot_close,
};

vfs_directory_operations_t proc_dops = {
	.open  = proc_open_dir,
	.read  = proc_read_dir,
	.close = proc_close_dir,
	.stat  = proc_stat,
};

// ── Mounting ────────────────────────────────────────────────────────────────

void procfs_mount(void)
{
	// The whole mount dance (why kRegisterFilesystem cannot be used, why bops
	// stays NULL, why the GUID stays zero) lives in synthfs_mount now — its
	// comments carried the doctrine with it.
	synthfs_mount("/proc", &proc_fops, &proc_dops, "processes as files");
}
