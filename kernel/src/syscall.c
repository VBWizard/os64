#include "syscall.h"
#include "syscall_numbers.h"

#include <stddef.h>
#include <stdint.h>

#include "BasicRenderer.h"
#include "panic.h"
#include "printd.h"
#include "scheduler.h"
#include "smp_core.h"
#include "task.h"
#include "memory/memcpy.h"
#include "memory/paging.h"
#include "memory/kmalloc.h"
#include "memory/vma.h"   // call_in_kernel_context
#include "log.h"
#include "console.h"

// spawn: cap on argv length. A command line's worth of args is plenty; the
// per-arg length cap is task.c's TASK_MAX_PATH_LEN (the blob it builds uses
// fixed-size slots of that width).
#define SPAWN_MAX_ARGS 32

#define SYSCALL_RESULT_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)
#define SYSCALL_RESULT_BAD_USER_DATA UINT64_C(0xFFFFFFFFFFFFFFFE)

static uint64_t g_saved_cr3[MAX_CPUS];
static bool g_saved_cr3_valid[MAX_CPUS];

static inline uint32_t get_current_cpu_index(void);
static bool prepare_syscall_args(const syscall_entry_t *entry, const uint64_t incoming[6], uint64_t prepared[6]);
static bool copy_user_string(const char *user_str, char *buffer, size_t buffer_len);
static bool copy_user_buffer(const void *user_src, void *kernel_dst, size_t length);
static bool copy_to_user_buffer(void *user_dst, const void *kernel_src, size_t length);

static uint64_t syscall_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_spawn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_debug_log(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_write(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);

// NOTE: syscall.S marshals the syscall registers straight into
// _syscall_dispatch()'s C arguments — there is deliberately no C-level entry
// shim here (the old register-pinned-locals version relied on behavior GCC
// only guarantees for inline-asm operands).
// Last column is user_ptr_arg_mask: bit i = "arg i is a user pointer, range-
// check it at the boundary".  Non-pointer args (handles, lengths, exit codes)
// and unused arg registers (ring-3 garbage!) must NOT be checked.
//
// needs_cr3_switch is FALSE for every current entry, on purpose.  Handlers run
// on the calling task's CR3, which maps BOTH the user's buffers (lower half)
// AND the whole kernel (upper half is shared into every task PML4) — there is
// nothing a syscall needs that the user CR3 can't see.  Switching to
// kKernelPML4 mid-syscall is actively fatal: the thread's syscall kernel stack
// is a task-local VA that kKernelPML4 does NOT map, so the first C statement
// after the switch touches an unmapped stack -> #PF -> the #PF handler pushes
// onto the same unmapped stack -> double -> TRIPLE FAULT (write() proved this
// the very first time the flag was ever exercised).  If a future syscall
// genuinely needs kernel context, it must switch STACK and CR3 together — see
// call_in_kernel_context in task_exit_asm.S for the proven pattern.
syscall_entry_t syscall_table[MAX_SYSCALLS] = {
	SYSCALL_DEFINE(SYSCALL_YIELD,     "yield",     syscall_yield,     false, 0x00),
	SYSCALL_DEFINE(SYSCALL_DEBUG_LOG, "debug_log", syscall_debug_log, false, 0x01),  // arg0 = message
	SYSCALL_DEFINE(SYSCALL_EXIT,      "exit",      syscall_exit,      false, 0x00),
	SYSCALL_DEFINE(SYSCALL_WRITE,     "write",     syscall_write,     false, 0x02),  // arg1 = buffer
	SYSCALL_DEFINE(SYSCALL_READ,      "read",      syscall_read,      false, 0x02),  // arg1 = buffer (written)
	SYSCALL_DEFINE(SYSCALL_SPAWN,     "spawn",     syscall_spawn,     false, 0x03),  // arg0 = path, arg1 = argv
	SYSCALL_DEFINE(SYSCALL_WAIT,      "wait",      syscall_wait,      false, 0x02),  // arg1 = exit-code out ptr
};

uint64_t _syscall_dispatch(
	uint64_t syscall_number,
	uint64_t arg0, uint64_t arg1, uint64_t arg2,
	uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	const uint64_t raw_args[6] = { arg0, arg1, arg2, arg3, arg4, arg5 };
	uint64_t prepared_args[6] = {0};
	bool switched_cr3 = false;
	uint64_t entry_cr3 = 0;

	// Remember the address space we arrived on for the exit tripwire below.
	asm volatile("mov %0, cr3" : "=r"(entry_cr3));

	if (syscall_number >= MAX_SYSCALLS)
	{
        printd(DEBUG_SYSCALL, "SYSCALL: invalid number %lu\n", syscall_number);
        return SYSCALL_RESULT_INVALID;
	}

	syscall_entry_t *entry = &syscall_table[syscall_number];
	if (!entry->func)
	{
        printd(DEBUG_SYSCALL, "SYSCALL: unimplemented number %lu\n", syscall_number);
        return SYSCALL_RESULT_INVALID;
	}

	if (!prepare_syscall_args(entry, raw_args, prepared_args))
	{
        printd(DEBUG_SYSCALL, "SYSCALL: user argument validation failed for %lu\n", syscall_number);
        return SYSCALL_RESULT_BAD_USER_DATA;
	}

	if (entry->needs_cr3_switch)
	{
		switch_to_kernel_cr3();
		switched_cr3 = true;
	}

	if (entry->trace_enabled)
	{
		log_syscall_invocation(entry, prepared_args);
	}

	uint64_t result = entry->func(
		prepared_args[0], prepared_args[1], prepared_args[2],
		prepared_args[3], prepared_args[4], prepared_args[5]);

	if (switched_cr3)
	{
		restore_user_cr3();
	}

	// Boundary tripwire (a keeper from the 32-bit OS): a syscall must leave on
	// the same address space it arrived on.  Escaping to ring 3 with the kernel
	// CR3 still loaded "works" (kernel maps are a superset) right up until it
	// corrupts something unrelated, so catch it here where the cause is obvious.
	// (Threads that legitimately ARRIVED on the kernel CR3 are exempt.)
	uint64_t exit_cr3 = 0;
	asm volatile("mov %0, cr3" : "=r"(exit_cr3));
	if (exit_cr3 != entry_cr3)
	{
		panic("_syscall_dispatch: syscall %lu (%s) entered on CR3 %#lx but is leaving on %#lx\n",
		      syscall_number, entry->name ? entry->name : "(unnamed)", entry_cr3, exit_cr3);
	}

	return result;
}

void switch_to_kernel_cr3(void)
{
	uint64_t current_cr3 = 0;
	asm volatile("mov %0, cr3" : "=r"(current_cr3));

	uint32_t cpu_index = get_current_cpu_index();
	g_saved_cr3[cpu_index] = current_cr3;
	g_saved_cr3_valid[cpu_index] = true;

	if (current_cr3 != (uint64_t)kKernelPML4)
	{
		asm volatile("mov cr3, %0" :: "r"((uint64_t)kKernelPML4) : "memory");
	}
}

void restore_user_cr3(void)
{
	uint32_t cpu_index = get_current_cpu_index();
	if (!g_saved_cr3_valid[cpu_index])
	{
		return;
	}

	uint64_t user_cr3 = g_saved_cr3[cpu_index];
	g_saved_cr3_valid[cpu_index] = false;

	if (user_cr3 && user_cr3 != (uint64_t)kKernelPML4)
	{
		asm volatile("mov cr3, %0" :: "r"(user_cr3) : "memory");
	}
}

bool validate_and_copy_user_data(const void* user_ptr, size_t length, void* kernel_buffer)
{
	if (!user_ptr || !kernel_buffer || length == 0)
	{
		return false;
	}

	uintptr_t user_address = (uintptr_t)user_ptr;

	// Reject ranges that start in — or run into — kernel-mapped memory.  The
	// subtraction form also catches user_address+length overflowing to wrap
	// back below kHHDMOffset.
	if (user_address >= kHHDMOffset || length > kHHDMOffset - user_address)
	{
		return false;
	}

	memcpy(kernel_buffer, user_ptr, length);
	return true;
}

void log_syscall_invocation(const syscall_entry_t* entry, const uint64_t args[6])
{
	if (!entry)
	{
		return;
	}

	size_t index = (size_t)(entry - syscall_table);
	const char *name = entry->name ? entry->name : "(unnamed)";
    printd(DEBUG_SYSCALL,
           "SYSCALL: #%zu %s args=%#lx,%#lx,%#lx,%#lx,%#lx,%#lx\n",
           index, name,
           args[0], args[1], args[2], args[3], args[4], args[5]);
}

static inline uint32_t get_current_cpu_index(void)
{
	core_local_storage_t *cls = get_core_local_storage();
	if (!cls)
	{
		return 0;
	}

	uint64_t apic_id = cls->apic_id;
	if (apic_id >= MAX_CPUS)
	{
		return 0;
	}

	return (uint32_t)apic_id;
}

// Boundary validation of user-pointer arguments, driven by the entry's
// user_ptr_arg_mask.  Only args the table declares as pointers are checked —
// see the mask's comment in syscall.h for why checking all six is a bug, not
// extra safety.  NULL is allowed through here so handlers can give it a
// per-call meaning (the copy helpers reject NULL where it matters).
static bool prepare_syscall_args(const syscall_entry_t *entry, const uint64_t incoming[6], uint64_t prepared[6])
{
	memcpy(prepared, incoming, sizeof(uint64_t) * 6);

	for (size_t i = 0; i < 6; ++i)
	{
		if (!(entry->user_ptr_arg_mask & (1u << i)))
		{
			continue;
		}

		if (prepared[i] == 0)
		{
			continue;
		}

		if (prepared[i] >= kHHDMOffset)
		{
			return false;
		}
	}

	return true;
}

// ── User-copy CR3 window ─────────────────────────────────────────────────────
// The dispatcher may already have moved this core to the kernel CR3 (entries
// with needs_cr3_switch), but user pointers only resolve under the USER CR3.
// The copy helpers below therefore run inside a "window": open() drops back to
// the user CR3 the dispatcher saved for this core (when there is one), and
// close() restores whatever was loaded before.  Interrupts are masked for the
// whole syscall (SFMASK clears IF), so the window can't be preempted while the
// "wrong" CR3 is live.
static uint64_t user_cr3_window_open(bool *switched)
{
        uint64_t original_cr3 = 0;
        asm volatile("mov %0, cr3" : "=r"(original_cr3));

        *switched = false;
        if (original_cr3 == (uint64_t)kKernelPML4)
        {
                uint32_t cpu_index = get_current_cpu_index();
                if (g_saved_cr3_valid[cpu_index])
                {
                        uint64_t user_cr3 = g_saved_cr3[cpu_index];
                        if (user_cr3 && user_cr3 != (uint64_t)kKernelPML4)
                        {
                                asm volatile("mov cr3, %0" :: "r"(user_cr3) : "memory");
                                *switched = true;
                        }
                }
        }

        return original_cr3;
}

static void user_cr3_window_close(uint64_t original_cr3, bool switched)
{
        if (switched)
        {
                asm volatile("mov cr3, %0" :: "r"(original_cr3) : "memory");
        }
}

// Copy a NUL-terminated string from user space into a kernel buffer, walking
// byte-by-byte so an unterminated user string can't run past buffer_len.
// Returns false if the user range is invalid or no NUL appears within the
// buffer (the buffer is still NUL-terminated for safe logging either way).
static bool copy_user_string(const char *user_str, char *buffer, size_t buffer_len)
{
        if (!user_str || !buffer || buffer_len == 0)
        {
                return false;
        }

        bool switched = false;
        uint64_t original_cr3 = user_cr3_window_open(&switched);

        bool success = false;
        size_t written = 0;
        while (written < buffer_len - 1)
        {
                char ch = 0;
                if (!validate_and_copy_user_data(user_str + written, sizeof(char), &ch))
                {
                        goto out;
                }

                buffer[written++] = ch;
                if (ch == '\0')
                {
                        success = true;
                        goto out;
                }
        }

        buffer[buffer_len - 1] = '\0';

out:
        user_cr3_window_close(original_cr3, switched);
        return success;
}

// Copy exactly `length` bytes from user space into a kernel buffer.  The
// length-based sibling of copy_user_string, for payloads that aren't strings
// (write() buffers may legally contain NUL bytes).
static bool copy_user_buffer(const void *user_src, void *kernel_dst, size_t length)
{
        if (!user_src || !kernel_dst || length == 0)
        {
                return false;
        }

        bool switched = false;
        uint64_t original_cr3 = user_cr3_window_open(&switched);

        bool success = validate_and_copy_user_data(user_src, length, kernel_dst);

        user_cr3_window_close(original_cr3, switched);
        return success;
}

// Copy `length` bytes from a kernel buffer OUT to user space — the reverse of
// copy_user_buffer, for read()'s result. Range-checks the destination the same
// way (reject any dst that reaches into kernel-mapped memory, overflow-safe).
static bool copy_to_user_buffer(void *user_dst, const void *kernel_src, size_t length)
{
        if (!user_dst || !kernel_src || length == 0)
        {
                return false;
        }

        uintptr_t user_address = (uintptr_t)user_dst;
        if (user_address >= kHHDMOffset || length > kHHDMOffset - user_address)
        {
                return false;
        }

        bool switched = false;
        uint64_t original_cr3 = user_cr3_window_open(&switched);

        memcpy(user_dst, kernel_src, length);

        user_cr3_window_close(original_cr3, switched);
        return true;
}

static uint64_t syscall_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg0;
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	// scheduler_trigger: genuine APIC self-IPI into the scheduler, same entry
	// semantics as the timer path (in-service bit, EOI discipline).  Returns
	// as soon as the scheduler decides — immediately if nothing else is
	// runnable, after a context-switch round trip if something was.
	// (Replaced scheduler_yield, whose direct software `int` bypassed APIC
	// nesting protection and whose empty-queue path slept a full tick.)
	scheduler_trigger(NULL);
	return 0;
}

static uint64_t syscall_debug_log(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	const char *user_message = (const char*)arg0;
    char kernel_buffer[MAX_LOG_MESSAGE_SIZE];

    if (!copy_user_string(user_message, kernel_buffer, sizeof(kernel_buffer)))
	{
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	printf("[user] %s\n", kernel_buffer);
	return 0;
}

// exit(code) — terminate the calling task.  arg0 becomes the task's retVal
// (harvested by task_wait / the test harness).  Everything after the retVal
// store is task_exit()'s problem: it switches to the per-core kernel interrupt
// stack and kKernelPML4 itself, marks task+thread exited, and yields away for
// good — which is why this entry runs with needs_cr3_switch=false (the
// dispatcher's save/restore bookkeeping would never get to run anyway).
static uint64_t syscall_exit(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1;
	(void)arg2;
	(void)arg3;
	(void)arg4;
	(void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task)
	{
		task->retVal = arg0;
	}

	task_exit();
	__builtin_unreachable();
}

// write(handle, buffer, length) — write bytes to an output handle.
// Until a per-task handle table exists, only the two console handles are
// valid, and both reach the screen.  Runs entirely on the calling task's CR3
// (user buffer in the lower half, console/renderer in the shared upper half —
// see the table comment).  The user buffer is ferried through a bounded
// kernel chunk so arbitrary user lengths never map to unbounded kernel stack
// use.  Returns the byte count written, or a SYSCALL_RESULT_* error sentinel.
#define WRITE_CHUNK_SIZE 512
static uint64_t syscall_write(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3;
	(void)arg4;
	(void)arg5;

	uint64_t handle = arg0;
	const char *user_buffer = (const char*)arg1;
	size_t length = (size_t)arg2;

	if (handle != SYSCALL_HANDLE_CONSOLE_OUT && handle != SYSCALL_HANDLE_CONSOLE_ERR)
	{
		return SYSCALL_RESULT_INVALID;
	}

	if (length == 0)
	{
		return 0;
	}

	char chunk[WRITE_CHUNK_SIZE];
	size_t copied = 0;
	while (copied < length)
	{
		size_t this_chunk = length - copied;
		if (this_chunk > sizeof(chunk))
		{
			this_chunk = sizeof(chunk);
		}

		if (!copy_user_buffer(user_buffer + copied, chunk, this_chunk))
		{
			// Report progress if some bytes already made it to the console;
			// only fail outright when nothing was written.
			return copied ? copied : SYSCALL_RESULT_BAD_USER_DATA;
		}

		print_n(chunk, this_chunk);
		copied += this_chunk;
	}

	return copied;
}

// read(handle, buffer, length) — read input bytes into a user buffer.
// Until a per-task handle table exists, only stdin (handle 0) is valid; it
// reads from the console keyboard. BLOCKS until at least one byte is available
// (Unix terminal semantics) — the block happens inside console_read via
// SIGSLEEP, so the calling thread sleeps (zero CPU) and other threads run
// while it waits; it is woken when a key arrives. Returns the byte count, or a
// SYSCALL_RESULT_* sentinel. Runs on the caller's CR3 (user buffer in the
// lower half is directly writable); result ferried through a bounded kernel
// chunk like write() does.
#define READ_CHUNK_SIZE 256
static uint64_t syscall_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3;
	(void)arg4;
	(void)arg5;

	uint64_t handle = arg0;
	void *user_buffer = (void*)arg1;
	size_t length = (size_t)arg2;

	if (handle != SYSCALL_HANDLE_CONSOLE_IN)
	{
		return SYSCALL_RESULT_INVALID;
	}

	if (length == 0)
	{
		return 0;
	}

	char chunk[READ_CHUNK_SIZE];
	size_t want = length < sizeof(chunk) ? length : sizeof(chunk);

	// Blocks until >=1 byte is available, then returns what's queued (<= want).
	long got = console_read(chunk, want);
	if (got <= 0)
	{
		return 0;
	}

	if (!copy_to_user_buffer(user_buffer, chunk, (size_t)got))
	{
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	return (uint64_t)got;
}

// Marshal a user argv (NULL-terminated array of user string pointers) into
// kernel space: fills kargv[] (<= SPAWN_MAX_ARGS, NULL-terminated) with
// pointers into strbuf, each string copied from user space. Returns argc, or
// -1 on a bad user pointer. NULL user_argv means "no args" (argc 0).
static int marshal_user_argv(char *const *user_argv, char *kargv[],
                             char *strbuf, size_t strbuf_len)
{
	if (user_argv == NULL)
	{
		kargv[0] = NULL;
		return 0;
	}

	size_t used = 0;
	int argc = 0;
	for (argc = 0; argc < SPAWN_MAX_ARGS; argc++)
	{
		// Read user_argv[argc] — a user-space char* — into the kernel.
		char *uptr = NULL;
		if (!copy_user_buffer(&user_argv[argc], &uptr, sizeof(char *)))
			return -1;
		if (uptr == NULL)
			break;   // NULL terminator: end of argv

		char *dst = strbuf + used;
		size_t avail = strbuf_len - used;
		if (avail < 2)
			return -1;   // out of scratch space
		size_t cap = avail < TASK_MAX_PATH_LEN ? avail : TASK_MAX_PATH_LEN;
		if (!copy_user_string((const char *)uptr, dst, cap))
			return -1;

		kargv[argc] = dst;
		// Advance past the copied (NUL-terminated) string.
		size_t slen = 0;
		while (dst[slen] != '\0')
			slen++;
		used += slen + 1;
	}
	kargv[argc] = NULL;
	return argc;
}

// All the state task_create() needs, in ONE kmalloc'd (HHDM) block so it is
// reachable from BOTH the syscall CR3 (to fill in) AND kKernelPML4 (where the
// helper below runs). Nothing here may live on the syscall's task-local kernel
// stack — kKernelPML4 doesn't map it. argv[] points into argvstrs[], both in
// this block.
typedef struct {
	char   path[TASK_MAX_PATH_LEN];
	int    argc;
	char  *argv[SPAWN_MAX_ARGS + 1];
	char   argvstrs[SPAWN_MAX_ARGS * TASK_MAX_PATH_LEN];
	task_t *parent;
	volatile long result;                 // child pid, or -1 on failure
} spawn_params_t;

// Runs under kKernelPML4 (via call_in_kernel_context), because task_create
// does disk I/O — the ELF load touches NVMe/AHCI DMA regions that are mapped
// in the kernel tables but NOT in a user task's CR3 (which is why calling
// task_create directly from the syscall faulted in nvme_submit_command). Same
// discipline the demand-pager uses for kernel_read_file.
static void spawn_do_create(void *arg)
{
	spawn_params_t *p = (spawn_params_t *)arg;
	task_t *child = task_create(p->path, p->argc, p->argv, p->parent,
	                            false, THREAD_NO_AFFINITY);
	if (child != NULL)
	{
		scheduler_submit_new_task(child);
		p->result = (long)child->taskID;
	}
	else
	{
		p->result = -1;
	}
}

// spawn(path, argv) — launch `path` as a child of the calling task and return
// its pid (task id), or a SYSCALL_RESULT_* sentinel. Non-blocking: the child
// is submitted to the scheduler and runs concurrently; the caller reaps it
// with wait(). The child inherits the caller's environment (task_create does
// this via the parent).
static uint64_t syscall_spawn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	const char *user_path = (const char *)arg0;
	char *const *user_argv = (char *const *)arg1;

	spawn_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	// Marshal path + argv from user space into the HHDM block (runs on the
	// caller's CR3, which maps both the user args and the HHDM).
	if (!copy_user_string(user_path, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	int argc = marshal_user_argv(user_argv, p->argv, p->argvstrs, sizeof(p->argvstrs));
	if (argc < 0)
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	p->argc = argc;

	core_local_storage_t *cls = get_core_local_storage();
	p->parent = cls ? cls->task : NULL;
	p->result = 0;

	// task_create runs under kKernelPML4 so its disk I/O sees the DMA mappings.
	call_in_kernel_context(spawn_do_create, p);

	long r = p->result;
	kfree(p);
	if (r < 0)
		return SYSCALL_RESULT_INVALID;   // bad path / load failure
	return (uint64_t)r;
}

// wait(pid, exit_code_out) — block until a child exits, reap it, return its
// pid. pid > 0 waits for that specific child; pid == 0 waits for the FIRST of
// any child to end (os64's own design, not POSIX). Writes the child's exit
// code to *exit_code_out (if non-NULL). Returns the ended pid, or a
// SYSCALL_RESULT_* sentinel (e.g. no matching child exists). If the child has
// ALREADY exited, returns immediately without sleeping.
static uint64_t syscall_wait(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uint64_t targetPid = arg0;
	int *user_code = (int *)arg1;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = cls ? cls->task : NULL;
	if (parent == NULL)
		return SYSCALL_RESULT_INVALID;

	uint64_t exitCode = 0;
	task_t *child = task_wait(parent, targetPid, &exitCode);
	if (child == NULL)
		return SYSCALL_RESULT_INVALID;   // no such child

	uint64_t endedPid = child->taskID;
	if (user_code != NULL)
	{
		int code = (int)exitCode;
		if (!copy_to_user_buffer(user_code, &code, sizeof(code)))
			return SYSCALL_RESULT_BAD_USER_DATA;
	}
	return endedPid;
}
