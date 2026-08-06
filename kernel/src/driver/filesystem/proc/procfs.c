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
//   /proc/cores                 CPU time per core: total/busy/idle/sched µs
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
#include "kmalloc.h"
#include "memcpy.h"
#include "memset.h"
#include "strings/strings.h"
#include "sprintf.h"
#include "serial_logging.h"
#include "scheduler.h"
#include "task.h"
#include "thread.h"
#include "signals.h"
#include "handle.h"
#include "memory/vma.h"
#include "memory/paging.h"
#include "CONFIG.h"
#include "BasicRenderer.h"   // printf — the mount line belongs on the glass too
#include "smp.h"             // core_local_storage_t + kMPCoreCount (/proc/cores)
#include "smp_core.h"        // get_core_local_storage_for_core

extern task_t   *kIdleTasks[];         // per-core idle tasks — their runCycles IS idle time
extern uint64_t  kCPUCyclesPerSecond;  // boot-calibrated: the cycles→µs exchange rate

extern uint64_t kTicksSinceStart;
extern uintptr_t kHHDMOffset;

// A task's VMA list and handle table are walked WITHOUT a lock (os64 has no
// per-task lock to take). A corrupted or concurrently-spliced list must not
// become an infinite loop in the kernel, so every walk is bounded. See the
// "un-synchronized snapshot" note in PROC.md.
#define PROC_MAX_LIST_WALK 512

// ── The growable text buffer ────────────────────────────────────────────────
// Every /proc file is built by appending formatted lines into one of these,
// then handed to the file handle as a fixed snapshot. Growth is
// allocate-copy-free (kmalloc has no realloc), doubling each time — a status
// file is a few hundred bytes and never reallocates; `maps` on a big task
// might do it once.

typedef struct
{
	char  *buf;
	size_t len;   // bytes used, not counting the NUL
	size_t cap;   // bytes allocated
	bool   oom;   // a growth failed — the text is truncated, and says so
} proc_text_t;

static bool ptext_init(proc_text_t *t, size_t cap)
{
	t->buf = kmalloc(cap);
	t->len = 0;
	t->cap = cap;
	t->oom = false;
	return t->buf != NULL;
}

static bool ptext_grow(proc_text_t *t, size_t needed)
{
	size_t newcap = t->cap ? t->cap : 256;
	while (newcap < needed)
		newcap *= 2;

	char *nb = kmalloc(newcap);
	if (nb == NULL)
	{
		t->oom = true;
		return false;
	}
	memcpy(nb, t->buf, t->len);
	kfree(t->buf);
	t->buf = nb;
	t->cap = newcap;
	return true;
}

// Append one formatted line. Deliberately renders into a bounded stack line
// buffer first: snprintf tells us the true length, so the grow decision is
// made with the real number rather than a guess.
static void ptext_addf(proc_text_t *t, const char *fmt, ...)
{
	char line[512];
	va_list args;

	if (t->buf == NULL)
		return;

	va_start(args, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (n < 0)
		return;
	size_t want = (size_t)n;
	if (want >= sizeof(line))
		want = sizeof(line) - 1;   // truncated at the line buffer; still valid text

	if (t->len + want + 1 > t->cap && !ptext_grow(t, t->len + want + 1))
		return;

	memcpy(t->buf + t->len, line, want);
	t->len += want;
	t->buf[t->len] = '\0';
}

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
	PROC_NODE_CORES_FILE,    // /cores — the machine's CPU-time ledger
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
static const char *kProcTaskFiles[] = { "status", "cmdline", "cwd", "handles", "maps", "ctl" };
#define PROC_TASK_FILE_COUNT (sizeof(kProcTaskFiles) / sizeof(kProcTaskFiles[0]))

static const char *kProcThreadFiles[] = { "status" };
#define PROC_THREAD_FILE_COUNT (sizeof(kProcThreadFiles) / sizeof(kProcThreadFiles[0]))

// Pull one '/'-delimited component out of `path` starting at *pos. Returns
// false at end of string. The component is copied (bounded) into `out`.
static bool proc_next_component(const char *path, size_t *pos, char *out, size_t outlen)
{
	while (path[*pos] == '/')
		(*pos)++;
	if (path[*pos] == '\0')
		return false;

	size_t n = 0;
	while (path[*pos] != '\0' && path[*pos] != '/')
	{
		if (n + 1 < outlen)
			out[n++] = path[*pos];
		(*pos)++;
	}
	out[n] = '\0';
	return true;
}

// Strict decimal parse — the whole component must be digits. "7x" is not task
// 7, it is a name that does not exist, and saying so is cheaper than the
// confusion of a permissive parse.
static bool proc_parse_u64(const char *s, uint64_t *out)
{
	if (s[0] == '\0')
		return false;

	uint64_t v = 0;
	for (size_t i = 0; s[i] != '\0'; i++)
	{
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10 + (uint64_t)(s[i] - '0');
	}
	*out = v;
	return true;
}

static bool proc_name_in(const char *name, const char **table, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (strcmp(name, table[i]) == 0)
			return true;
	return false;
}

// Classify an fs-local path. Purely syntactic — whether the task actually
// exists is a separate question, answered by proc_find_task at open time.
static void proc_parse_path(const char *path, proc_path_t *out)
{
	char comp[PROC_NAME_MAX];
	size_t pos = 0;

	memset(out, 0, sizeof(*out));
	out->type = PROC_NODE_INVALID;

	if (path == NULL)
		return;

	// Component 1: the task ID (absent = the /proc root itself).
	if (!proc_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_ROOT;
		return;
	}
	// The one non-numeric name at the root: the per-core CPU-time ledger.
	// Checked before the numeric parse so it can never shadow a task ID.
	if (strcmp(comp, "cores") == 0)
	{
		strncpy(out->name, comp, PROC_NAME_MAX - 1);
		// Nothing lives inside a file.
		if (proc_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = PROC_NODE_CORES_FILE;
		return;
	}

	if (!proc_parse_u64(comp, &out->taskID))
		return;   // "/proc/notanumber" — no such entry

	// Component 2: a task file, or the "thread" subdirectory.
	if (!proc_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_TASK;
		return;
	}
	if (strcmp(comp, "thread") != 0)
	{
		if (!proc_name_in(comp, kProcTaskFiles, PROC_TASK_FILE_COUNT))
			return;
		strncpy(out->name, comp, PROC_NAME_MAX - 1);
		// A trailing component after a file name means the path names
		// something inside a file, which is not a thing.
		if (proc_next_component(path, &pos, comp, sizeof(comp)))
			return;
		out->type = PROC_NODE_TASK_FILE;
		return;
	}

	// Component 3: the thread ID.
	if (!proc_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_THREADDIR;
		return;
	}
	if (!proc_parse_u64(comp, &out->threadID))
		return;

	// Component 4: a thread file.
	if (!proc_next_component(path, &pos, comp, sizeof(comp)))
	{
		out->type = PROC_NODE_THREAD;
		return;
	}
	if (!proc_name_in(comp, kProcThreadFiles, PROC_THREAD_FILE_COUNT))
		return;
	strncpy(out->name, comp, PROC_NAME_MAX - 1);
	if (proc_next_component(path, &pos, comp, sizeof(comp)))
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

	while (n + 1 < outlen)
	{
		uintptr_t phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, task_va + n);
		if (phys == 0 || phys == 0xbadbadba)
			break;   // unmapped — the string ends here, truncated but honest

		char c = *(const volatile char *)(phys | kHHDMOffset);
		if (c == '\0')
			break;
		out[n++] = c;
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
// Each fills a proc_text_t with the whole file. They run once, at open, under
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

// /proc/cores — the machine's CPU-time ledger, one row per core. Header
// line first, then tab-separated columns, so a reader can parse by name
// today and survive new columns tomorrow.
//
// busy is DERIVED (total - idle - sched): the accounting charges threads,
// the idle thread among them, and the scheduler's own passes; what the
// three don't explain is genuinely unaccounted (early boot, ISR time —
// documented v1 honesty). All values are written only by each core's own
// scheduler pass — reading them cross-core here is safe (worst case one
// slice stale); SUBTRACTING a remote TSC from a local rdtsc would not be,
// which is why total comes from the core's own two stamps, never from
// "now".
static void proc_gen_cores(proc_text_t *t)
{
	// Settle-on-read: every core charges its in-flight span (locally, own
	// TSC) before we render — so the books are never staler than this IPI
	// round-trip, and tickless mode's lumpy settlement can't staircase a reader.
	// Rate-limited inside (once per tick), so a top refresh pays once.
	mpAcctSettleAll();

	ptext_addf(t, "core\ttotal_us\tbusy_us\tidle_us\tsched_us\n");
	for (uint8_t i = 0; i < kMPCoreCount; i++)
	{
		core_local_storage_t *cls = get_core_local_storage_for_core(i);
		if (cls == NULL)
			continue;

		uint64_t total = (cls->acctLastDispatchTSC > cls->acctZeroTSC)
		                 ? cls->acctLastDispatchTSC - cls->acctZeroTSC : 0;
		uint64_t sched = cls->acctSchedCycles;
		uint64_t idle  = 0;
		if (kIdleTasks[i] != NULL && kIdleTasks[i]->threads != NULL)
			idle = kIdleTasks[i]->threads->runCycles;

		uint64_t accounted = idle + sched;
		uint64_t busy = (total > accounted) ? total - accounted : 0;

		ptext_addf(t, "%u\t%lu\t%lu\t%lu\t%lu\n", (unsigned)i,
		           proc_cycles_to_us(total), proc_cycles_to_us(busy),
		           proc_cycles_to_us(idle),  proc_cycles_to_us(sched));
	}
}

static void proc_gen_task_status(proc_text_t *t, task_t *task)
{
	thread_t *th = task->threads;

	// Same settle as /proc/cores (rate-limited to once a tick): runtime_us
	// below must not be a scheduler-pass stale on a monopolized core.
	mpAcctSettleAll();

	ptext_addf(t, "task\t%lu\n", task->taskID);
	ptext_addf(t, "name\t%s\n", task->exename[0] ? task->exename : "(none)");
	ptext_addf(t, "state\t%s\n", th ? proc_state_name(th->threadState) : "none");
	ptext_addf(t, "parent\t%lu\n",
	           task->parentTask ? task->parentTask->taskID : 0);
	ptext_addf(t, "kernel\t%s\n", task->kernelTask ? "yes" : "no");
	// The two console-ownership bits SIGINT.md introduced. Worth surfacing:
	// "which task would Ctrl+C hit" is exactly the sort of question /proc
	// exists to answer without a debugger.
	ptext_addf(t, "foreground\t%s\n", (kForegroundTask == task) ? "yes" : "no");
	ptext_addf(t, "shell\t%s\n", task->controllingShell ? "yes" : "no");
	ptext_addf(t, "threads\t%u\n", proc_task_thread_count(task));
	ptext_addf(t, "heap\t%p-%p\n", (void *)task->heapStart, (void *)task->heapEnd);
	ptext_addf(t, "entry\t%p\n", (void *)task->entryPoint);
	if (task->loadBias)
		ptext_addf(t, "bias\t%p\n", (void *)task->loadBias);
	ptext_addf(t, "faults\t%u minor %u major\n", task->minorFaults, task->majorFaults);
	ptext_addf(t, "switches\t%u\n", task->cSwitches);
	// Where the task's thread last ran — the answer to "whose plate did
	// this time come off", without which per-core arithmetic is guesswork.
	if (th != NULL)
		ptext_addf(t, "core\t%u\n", th->lastRunApicID);
	// Real CPU time, charged at context-switch boundaries (thread.h has the
	// doctrine). `ticks` below is the old sampling counter — kept because
	// removing a field is an ABI event, but runtime_us is the honest one.
	//
	// SUMMED ACROSS THE TASK'S THREADS, which is what makes this number mean
	// "what this program cost" rather than "what its first thread cost" —
	// and on a threaded program those differ wildly, because the first
	// thread is usually the one waiting.
	ptext_addf(t, "runtime_us\t%lu\n", proc_cycles_to_us(proc_task_run_cycles(task)));
	if (th != NULL)
		ptext_addf(t, "ticks\t%lu\n", th->totalRunTicks);
	// A live task has no exit status; saying "-" beats printing a zero that
	// looks like "exited successfully".
	if (task->exited)
		ptext_addf(t, "exit\t%lu\n", task->retVal);
	else
		ptext_addf(t, "exit\t-\n");
}

static void proc_gen_cmdline(proc_text_t *t, task_t *task)
{
	// One argument per LINE, not NUL-separated (PROC.md): the unambiguity is
	// the same and `cat` renders it correctly, which Linux's version does not.
	if (task->argv == NULL || task->argc <= 0)
	{
		// argv[0] is not always populated for kernel-launched tasks; the
		// executable name is still the honest answer to "what is this".
		if (task->exename[0])
			ptext_addf(t, "%s\n", task->exename);
		return;
	}
	for (int i = 0; i < task->argc && i < PROC_MAX_LIST_WALK; i++)
	{
		// argv[i] is a TASK virtual address, not a kernel one — it must be
		// translated, never dereferenced. See proc_copy_task_string.
		char arg[TASK_MAX_PATH_LEN];
		if (proc_copy_task_string(task, (uintptr_t)task->argv[i], arg, sizeof(arg)))
			ptext_addf(t, "%s\n", arg);
	}
}

static void proc_gen_cwd(proc_text_t *t, task_t *task)
{
	ptext_addf(t, "%s\n", (task->cwd != NULL) ? task->cwd : "/");
}

static void proc_gen_handles(proc_text_t *t, task_t *task)
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
			ptext_addf(t, "%d\t%s\t%s\n", i, proc_handle_type_name(h->type),
			           (f->f_path != NULL) ? f->f_path : "(unnamed)");
		}
		else if (h->type == HANDLE_DIR && h->object != NULL)
		{
			vfs_directory_t *d = (vfs_directory_t *)h->object;
			ptext_addf(t, "%d\t%s\t%s\n", i, proc_handle_type_name(h->type),
			           (d->f_path != NULL) ? d->f_path : "(unnamed)");
		}
		else if (h->type == HANDLE_PIPE_READ || h->type == HANDLE_PIPE_WRITE)
		{
			// The pipe's identity is its object pointer — two tasks showing the
			// same value are the two ends of one pipeline, which is exactly what
			// you want to see when a pipeline hangs.
			ptext_addf(t, "%d\t%s\t%p\n", i, proc_handle_type_name(h->type), h->object);
		}
		else
		{
			ptext_addf(t, "%d\t%s\n", i, proc_handle_type_name(h->type));
		}
	}
}

static void proc_gen_maps(proc_text_t *t, task_t *task)
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

		ptext_addf(t, "%p-%p\t%s\t%s%s%s\n",
		           (void *)v->start, (void *)v->end, prot,
		           (v->flags & MAP_SHARED) ? "shared" : "private",
		           v->cow ? ",cow" : "",
		           (v->flags & MAP_SHARED_LIBRARY) ? ",lib" : "");
	}
}

static void proc_gen_thread_status(proc_text_t *t, task_t *task, thread_t *th)
{
	mpAcctSettleAll();   // same freshness contract as the task status file

	ptext_addf(t, "thread\t%lu\n", th->threadID);
	ptext_addf(t, "task\t%lu\n", task->taskID);
	ptext_addf(t, "state\t%s\n", proc_state_name(th->threadState));
	// mp_apic is the thread's PINNED AFFINITY, not the core it is running on —
	// a distinction worth the extra word, because labelling it "core" printed
	// 18446744073709551615 for every ordinary thread during bring-up (that is
	// THREAD_NO_AFFINITY, which means "any core", not core number 2^64-1).
	// Which core a thread is on RIGHT NOW lives in each core's CLS and would
	// have to be found by scanning them; it joins this file when something
	// needs it.
	if (th->mp_apic == THREAD_NO_AFFINITY)
		ptext_addf(t, "affinity\tany\n");
	else
		ptext_addf(t, "affinity\t%lu\n", th->mp_apic);
	ptext_addf(t, "idle\t%s\n", th->idleThread ? "yes" : "no");
	ptext_addf(t, "core\t%u\n", th->lastRunApicID);   // last dispatched here
	ptext_addf(t, "ring\t%u\n", (unsigned)(th->regs.CS & 3));
	ptext_addf(t, "rip\t%p\n", (void *)th->regs.RIP);
	ptext_addf(t, "rsp\t%p\n", (void *)th->regs.RSP);
	ptext_addf(t, "ticks\t%lu\n", th->totalRunTicks);
	ptext_addf(t, "runtime_us\t%lu\n", proc_cycles_to_us(th->runCycles));
	ptext_addf(t, "signals\t%p\n", (void *)th->signals.sigind);
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

static void proc_gen_ctl(proc_text_t *t, task_t *task)
{
	(void)task;
	// "word<TAB>effect" — same tabular shape as every other file here, so the
	// same five-line parser reads it.
	for (size_t i = 0; i < PROC_CTL_VERB_COUNT; i++)
		ptext_addf(t, "%s\t%s\n", kProcCtlVerbs[i].word, kProcCtlVerbs[i].effect);
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
	char    *data;      // the snapshot (NULL only if generation failed outright)
	size_t   size;
	size_t   pos;
	bool     is_ctl;    // writes to this handle are commands
	uint64_t taskID;    // ctl's target, re-resolved at write time
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
	if (pp.type != PROC_NODE_TASK_FILE && pp.type != PROC_NODE_THREAD_FILE &&
	    pp.type != PROC_NODE_CORES_FILE)
		return -1;   // directories go through dops; everything else is not a file

	// /cores belongs to the machine, not to any task — no lookup to do.
	task_t *task = NULL;
	thread_t *th = NULL;
	if (pp.type != PROC_NODE_CORES_FILE)
	{
		task = proc_find_task(pp.taskID);
		if (task == NULL)
			return -1;

		if (pp.type == PROC_NODE_THREAD_FILE)
		{
			th = proc_task_thread(task, pp.threadID);
			if (th == NULL)
				return -1;
		}
	}

	bool is_ctl = (pp.type == PROC_NODE_TASK_FILE && strcmp(pp.name, "ctl") == 0);
	if (mode[0] == 'w' && !is_ctl)
		return -1;   // only ctl accepts a write, ever

	// Generate the whole file NOW, into a snapshot (PROC.md: an internally
	// consistent file beats a fresh one — you can never read the first half of
	// one task's status and the second half of its successor's).
	proc_text_t text;
	if (!ptext_init(&text, 512))
		return -1;

	if (pp.type == PROC_NODE_CORES_FILE)
	{
		proc_gen_cores(&text);
	}
	else if (pp.type == PROC_NODE_THREAD_FILE)
	{
		proc_gen_thread_status(&text, task, th);
	}
	else if (strcmp(pp.name, "status") == 0)   proc_gen_task_status(&text, task);
	else if (strcmp(pp.name, "cmdline") == 0)  proc_gen_cmdline(&text, task);
	else if (strcmp(pp.name, "cwd") == 0)      proc_gen_cwd(&text, task);
	else if (strcmp(pp.name, "handles") == 0)  proc_gen_handles(&text, task);
	else if (strcmp(pp.name, "maps") == 0)     proc_gen_maps(&text, task);
	else if (is_ctl)                           proc_gen_ctl(&text, task);
	else
	{
		kfree(text.buf);
		return -1;
	}

	if (text.oom)
		ptext_addf(&text, "(truncated: out of memory)\n");

	proc_file_handle_t *h = kmalloc(sizeof(proc_file_handle_t));
	*vfs_file = kmalloc(sizeof(vfs_file_t));
	if (h == NULL || *vfs_file == NULL)
	{
		if (h) kfree(h);
		if (*vfs_file) kfree(*vfs_file);
		if (text.buf) kfree(text.buf);
		*vfs_file = NULL;
		return -1;
	}

	h->data   = text.buf;
	h->size   = text.len;
	h->pos    = 0;
	h->is_ctl = is_ctl;
	h->taskID = pp.taskID;

	(*vfs_file)->filetype = FILETYPE_PROCFILE;   // the enum's oldest fossil, finally true
	(*vfs_file)->handle   = h;
	(*vfs_file)->f_path   = (char *)path;   // caller's pointer, caller's lifetime
	                                        // (the handle closer frees it — vfs.h)
	(*vfs_file)->fops     = vfs_fs->fops;
	(*vfs_file)->owner    = vfs_fs;
	return 0;
}

static int proc_read(vfs_file_t *vfs_file, void *buffer, size_t size)
{
	proc_file_handle_t *h = (proc_file_handle_t *)vfs_file->handle;

	if (h->data == NULL || h->pos >= h->size)
		return 0;   // end of file — 0, like every other read in os64
	if (size > h->size - h->pos)
		size = h->size - h->pos;

	memcpy(buffer, h->data + h->pos, size);
	h->pos += size;
	return (int)size;
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

static int proc_seek(vfs_file_t *vfs_file, long offset, int whence)
{
	proc_file_handle_t *h = (proc_file_handle_t *)vfs_file->handle;
	int64_t base;

	switch (whence)
	{
		case SEEK_SET: base = 0; break;
		case SEEK_CUR: base = (int64_t)h->pos; break;
		case SEEK_END: base = (int64_t)h->size; break;
		default: return -1;
	}
	int64_t target = base + offset;
	if (target < 0)
		return -1;
	h->pos = (size_t)target;   // past-end is legal; reads there return 0
	return 0;
}

static int proc_tell(vfs_file_t *vfs_file)
{
	return (int)((proc_file_handle_t *)vfs_file->handle)->pos;
}

static int proc_close(vfs_file_t *vfs_file)
{
	proc_file_handle_t *h = (proc_file_handle_t *)vfs_file->handle;

	if (h != NULL)
	{
		if (h->data != NULL)
			kfree(h->data);
		kfree(h);
	}
	kfree(vfs_file);
	return 0;
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
			// The machine's own file first, then the tasks — `ls /proc`
			// leads with the ledger. index doubles as the "cores emitted"
			// flag; the task walk below uses the ID cursor, not index.
			if (h->index == 0)
			{
				h->index = 1;
				strncpy(entry->name, "cores", OS64_DIRENT_NAME_MAX);
				entry->size = 0;
				return 1;
			}

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

		case PROC_NODE_CORES_FILE:
			strncpy(entry->name, "cores", OS64_DIRENT_NAME_MAX);
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
	.read  = proc_read,
	.write = proc_write,
	.seek  = proc_seek,
	.tell  = proc_tell,
	.close = proc_close,
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
	// kRegisterFilesystem cannot be used: it reaches through
	// device->block_device->ops to copy block operations, and there is no
	// block device here. So procfs builds its own vfs_filesystem_t and claims
	// its prefix directly — which is all "mounting" has ever meant in this
	// kernel (see the mount-table note in vfs.h).
	if (kMountCount >= VFS_MAX_MOUNTS)
	{
		printd(DEBUG_BOOT, "BOOT: mount table full — /proc not mounted\n");
		return;
	}

	vfs_filesystem_t *fs = kmalloc(sizeof(vfs_filesystem_t));
	if (fs == NULL)
	{
		printd(DEBUG_BOOT, "BOOT: out of memory — /proc not mounted\n");
		return;
	}

	fs->fops = kmalloc(sizeof(vfs_file_operations_t));
	fs->dops = kmalloc(sizeof(vfs_directory_operations_t));
	if (fs->fops == NULL || fs->dops == NULL)
	{
		if (fs->fops) kfree(fs->fops);
		if (fs->dops) kfree(fs->dops);
		kfree(fs);
		printd(DEBUG_BOOT, "BOOT: out of memory — /proc not mounted\n");
		return;
	}
	memcpy(fs->fops, &proc_fops, sizeof(vfs_file_operations_t));
	memcpy(fs->dops, &proc_dops, sizeof(vfs_directory_operations_t));

	// bops stays NULL — nothing under this filesystem ever reads a sector, and
	// a NULL there is the honest statement of that. (Everything else in the
	// struct is left zeroed by the allocator: no superblock, no block device,
	// no partition number, because none of those things exist here.)

	vfs_mount_entry_t *m = &kMountTable[kMountCount];
	strncpy(m->prefix, "/proc", VFS_MOUNT_PREFIX_MAX - 1);
	m->prefix[VFS_MOUNT_PREFIX_MAX - 1] = '\0';
	m->prefix_len = strlen(m->prefix);
	// part_guid stays all-zero: there is no partition. Mounting AFTER the
	// auto-mount sweep keeps that zero out of the sweep's dedupe comparisons,
	// where it could otherwise match a partition whose GUID failed to read.
	memset(m->part_guid, 0, sizeof(m->part_guid));
	m->fs = fs;
	kMountCount++;

	printd(DEBUG_BOOT, "BOOT: mounted /proc (synthetic — processes as files)\n");
	printf("mounted proc at /proc\n");
}
