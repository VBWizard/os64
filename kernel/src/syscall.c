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
#include "handle.h"
#include "pipe.h"
#include "signals.h"
#include "vfs.h"     // kRootFilesystem + vfs_file_t (open/seek/file read/write)

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
static uint64_t syscall_pipe(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_close(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_open(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_seek(uint64_t arg0, uint64_t arg1, uint64_t arg2,
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
	SYSCALL_DEFINE(SYSCALL_SPAWN,     "spawn",     syscall_spawn,     false, 0x03),  // arg0 = path, arg1 = argv (args 2-4 = in/out/err handles, NOT pointers)
	SYSCALL_DEFINE(SYSCALL_WAIT,      "wait",      syscall_wait,      false, 0x02),  // arg1 = exit-code out ptr
	SYSCALL_DEFINE(SYSCALL_PIPE,      "pipe",      syscall_pipe,      false, 0x01),  // arg0 = int[2] out
	SYSCALL_DEFINE(SYSCALL_CLOSE,     "close",     syscall_close,     false, 0x00),  // arg0 = handle (an int, not a pointer)
	SYSCALL_DEFINE(SYSCALL_OPEN,      "open",      syscall_open,      false, 0x03),  // arg0 = path, arg1 = mode (both strings)
	SYSCALL_DEFINE(SYSCALL_SEEK,      "seek",      syscall_seek,      false, 0x00),  // args: handle, offset, whence — no pointers
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

	printd(DEBUG_APPLICATION,"[user] %s\n", kernel_buffer);
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

// Raise SIGPIPE on the calling task: it wrote to a pipe whose readers have all
// closed, i.e. it is producing into the void. DEFAULT ACTION IS TERMINATE, and
// that default is load-bearing — it is precisely what makes `yes | head -1`
// exit instead of spinning forever. We record the signal and then end the task
// through the normal exit path (retVal carries the signal so a waiting parent
// can see HOW the child died, not just that it did).
//
// A program that wants to SURVIVE a vanishing reader will, once userland signal
// delivery exists, install a handler and get the PIPE_ERR_CLOSED return value
// instead. Until then the kernel enforces the default, which is the behavior
// every pipeline actually wants.
#define TASK_EXIT_SIGPIPE 141   // 128 + signal, the classic "died by signal" encoding
static void raise_sigpipe_and_die(task_t *task)
{
	if (task != NULL)
	{
		if (task->threads != NULL)
			task->threads->signals.sigind |= SIGPIPE;
		task->retVal = TASK_EXIT_SIGPIPE;
		printd(DEBUG_TASK, "SIGPIPE: task %s wrote to a pipe with no readers — terminating\n",
			task->exename);
	}

	task_exit();
	__builtin_unreachable();
}

// ── File I/O plumbing (HANDLE_FILE) ──────────────────────────────────────────
// Every actual file operation (read/write/seek — and open/close elsewhere)
// runs under kKernelPML4 via call_in_kernel_context, because the VFS bottoms
// out in NVMe/AHCI DMA structures that live in kernel-only mappings — calling
// the driver on a user CR3 faults in nvme_submit_command, the exact lesson
// spawn learned. Same rules as spawn_params_t: the params block AND the bounce
// buffer are kmalloc'd (HHDM-reachable from both address spaces); nothing the
// helper touches may live on the syscall's task-local kernel stack.
// One page per hop for read() and file-write bounce buffers. (Defined here,
// above BOTH read and write, because the file-write path below uses it too.)
#define READ_CHUNK_SIZE 4096

typedef struct {
	vfs_file_t *file;
	void       *buf;       // kmalloc'd kernel bounce buffer (never a user ptr)
	size_t      len;
	long        offset;    // seek only
	int         whence;    // seek only
	volatile long result;  // bytes moved / new position, or negative
} file_io_params_t;

static void file_do_read(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;
	p->result = (f->fops != NULL && f->fops->read != NULL)
	                ? f->fops->read(f, p->buf, p->len) : -1;
}

static void file_do_write(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;
	p->result = (f->fops != NULL && f->fops->write != NULL)
	                ? f->fops->write(f, p->buf, p->len) : -1;
}

static void file_do_seek(void *arg)
{
	file_io_params_t *p = (file_io_params_t *)arg;
	vfs_file_t *f = p->file;

	if (f->fops == NULL || f->fops->seek == NULL ||
	    f->fops->seek(f, p->offset, p->whence) < 0)
	{
		p->result = -1;
		return;
	}

	// seek()'s contract is "return the NEW absolute position" — genuinely more
	// useful than Unix lseek's, and free: tell() already knows. (A filesystem
	// with no tell() still seeks fine; the caller just gets 0 back.)
	p->result = (f->fops->tell != NULL) ? f->fops->tell(f) : 0;
}

// write(handle, buffer, length) — write bytes to an output handle.
//
// The handle is resolved through the CALLING TASK'S HANDLE TABLE — which is the
// entire point of handles. This function does not know or care whether handle 1
// is the console or the write end of a pipe; it asks the table and dispatches on
// the tag. That is why the shell can redirect a program's stdout into a pipeline
// without the program (or this syscall) changing by one line.
//
// Runs on the calling task's CR3 (user buffer in the lower half, console and
// pipe rings in the shared upper half — see the table comment).
#define WRITE_CHUNK_SIZE 512
static uint64_t syscall_write(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3;
	(void)arg4;
	(void)arg5;

	const char *user_buffer = (const char*)arg1;
	size_t length = (size_t)arg2;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL)
		return SYSCALL_RESULT_INVALID;

	if (length == 0)
		return 0;

	switch (h->type)
	{
		case HANDLE_CONSOLE_OUT:
		case HANDLE_CONSOLE_ERR:
		{
			// Ferry through a bounded kernel chunk so an arbitrary user length
			// never maps to unbounded kernel stack use.
			char chunk[WRITE_CHUNK_SIZE];
			size_t copied = 0;
			while (copied < length)
			{
				size_t this_chunk = length - copied;
				if (this_chunk > sizeof(chunk))
					this_chunk = sizeof(chunk);

				if (!copy_user_buffer(user_buffer + copied, chunk, this_chunk))
				{
					// Report progress if some bytes already made it to the
					// console; only fail outright when nothing was written.
					return copied ? copied : SYSCALL_RESULT_BAD_USER_DATA;
				}

				print_n(chunk, this_chunk);
				copied += this_chunk;
			}
			return copied;
		}

		case HANDLE_PIPE_WRITE:
		{
			pipe_t *p = (pipe_t *)h->object;

			// Copy the user bytes into kernel memory FIRST, in one piece, and
			// only THEN hand them to pipe_write. Two reasons, both load-bearing:
			//
			// 1. ATOMICITY. Our rule is that a write of <= PIPE_CAPACITY lands
			//    whole. If we chunked this through a 512-byte bounce buffer, a
			//    blocked write would interleave with another writer's bytes and
			//    the guarantee would be a lie.
			// 2. FAULTS. pipe_write copies into the ring while holding the pipe
			//    spinlock with interrupts OFF. Copying from USER memory there
			//    could demand-page — and faulting inside a spinlock with IF=0 is
			//    a deadlock (the fault handler wants locks the holder still has).
			//    Faulting out here, before any lock is taken, is perfectly safe.
			//
			// A write LARGER than the capacity can never fit in the ring, so it
			// is the one case that must chunk (documented in pipe.h) — we do it
			// at capacity granularity, which keeps each chunk atomic.
			size_t written = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > PIPE_CAPACITY)
					this_chunk = PIPE_CAPACITY;

				char *kbuf = kmalloc(this_chunk);
				if (kbuf == NULL)
					return written ? written : SYSCALL_RESULT_INVALID;

				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
				{
					kfree(kbuf);
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;
				}

				long n = pipe_write(p, kbuf, this_chunk);   // BLOCKS if full
				kfree(kbuf);

				if (n == PIPE_ERR_CLOSED)
				{
					// Nobody is left to read this. Default action: terminate.
					raise_sigpipe_and_die(task);
					__builtin_unreachable();
				}
				if (n < 0)
					return written ? written : SYSCALL_RESULT_INVALID;

				written += (size_t)n;
			}
			return written;
		}

		case HANDLE_FILE:
		{
			// Ferry through a bounded kernel bounce buffer, chunk by chunk.
			// Unlike the pipe path there is no cross-writer atomicity promise
			// to keep — a file write that lands in pieces is still one write —
			// so chunking costs nothing but loop iterations. Both the params
			// block and the buffer are kmalloc'd: file_do_write runs under
			// kKernelPML4, which cannot see this syscall's stack.
			file_io_params_t *fp = kmalloc(sizeof(*fp));
			char *kbuf = kmalloc(READ_CHUNK_SIZE);
			if (fp == NULL || kbuf == NULL)
			{
				if (fp)   kfree(fp);
				if (kbuf) kfree(kbuf);
				return SYSCALL_RESULT_INVALID;
			}

			size_t written = 0;
			uint64_t rc = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > READ_CHUNK_SIZE)
					this_chunk = READ_CHUNK_SIZE;

				// Copy from user space HERE, on the caller's CR3, where the
				// user buffer actually resolves (and where a demand-page fault
				// is safe — no locks held, kernel context not yet entered).
				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
				{
					rc = written ? written : SYSCALL_RESULT_BAD_USER_DATA;
					goto file_write_out;
				}

				fp->file = (vfs_file_t *)h->object;
				fp->buf = kbuf;
				fp->len = this_chunk;
				fp->result = -1;
				call_in_kernel_context(file_do_write, fp);

				if (fp->result < 0)
				{
					rc = written ? written : SYSCALL_RESULT_INVALID;
					goto file_write_out;
				}

				written += (size_t)fp->result;
				if ((size_t)fp->result < this_chunk)
					break;   // short write: filesystem/device is full — report progress
			}
			rc = written;

file_write_out:
			kfree(kbuf);
			kfree(fp);
			return rc;
		}

		default:
			// Reading-only handle (or the console's input side): not writable.
			return SYSCALL_RESULT_INVALID;
	}
}

// read(handle, buffer, length) — read input bytes into a user buffer.
//
// Resolved through the calling task's handle table, exactly like write(): this
// syscall does not know whether handle 0 is the keyboard or the read end of a
// pipe. Both BLOCK (via SIGSLEEP, so the thread genuinely sleeps at zero CPU
// while other threads run) and both return SHORT — whatever is available right
// now, not a filled buffer. A pipe read returns 0 at EOF, which is what a
// filter uses to know its input is finished.
//
// Runs on the caller's CR3 (the user buffer, lower half, is directly writable);
// results are ferried through a kernel bounce buffer (READ_CHUNK_SIZE per hop).
static uint64_t syscall_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3;
	(void)arg4;
	(void)arg5;

	void *user_buffer = (void*)arg1;
	size_t length = (size_t)arg2;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL)
		return SYSCALL_RESULT_INVALID;

	if (length == 0)
		return 0;

	size_t want = length < READ_CHUNK_SIZE ? length : READ_CHUNK_SIZE;
	char *kbuf = kmalloc(want);
	if (kbuf == NULL)
		return SYSCALL_RESULT_INVALID;

	long got = 0;
	switch (h->type)
	{
		case HANDLE_CONSOLE_IN:
			// Blocks until >=1 byte is available (terminal semantics).
			got = console_read(kbuf, want);
			break;

		case HANDLE_PIPE_READ:
			// Blocks until >=1 byte is available, OR the last writer closes —
			// which returns 0, and 0 is EOF. (EOF is the absence of writers.)
			// Copying into a KERNEL buffer, not straight to user space, for the
			// same reason write() does the reverse: pipe_read copies under the
			// pipe spinlock with interrupts off, and touching user memory there
			// could demand-page into a deadlock.
			got = pipe_read((pipe_t *)h->object, kbuf, want);
			break;

		case HANDLE_FILE:
		{
			// Never blocks (a file always knows its bytes) and returns SHORT at
			// the end: fewer bytes than asked near EOF, then 0 AT EOF — so the
			// canonical filter loop works on a file with zero special-casing.
			// The actual read runs under kKernelPML4 (see file_do_read); kbuf
			// and the params block are kmalloc'd, reachable from both worlds.
			file_io_params_t *fp = kmalloc(sizeof(*fp));
			if (fp == NULL)
			{
				kfree(kbuf);
				return SYSCALL_RESULT_INVALID;
			}
			fp->file = (vfs_file_t *)h->object;
			fp->buf = kbuf;
			fp->len = want;
			fp->result = -1;
			call_in_kernel_context(file_do_read, fp);
			got = fp->result;
			kfree(fp);

			if (got < 0)
			{
				// A real device/filesystem error — distinct from EOF's clean 0.
				kfree(kbuf);
				return SYSCALL_RESULT_INVALID;
			}
			break;
		}

		default:
			kfree(kbuf);
			return SYSCALL_RESULT_INVALID;   // a write-only handle
	}

	if (got <= 0)
	{
		kfree(kbuf);
		return 0;   // EOF (pipe) or nothing (console)
	}

	bool ok = copy_to_user_buffer(user_buffer, kbuf, (size_t)got);
	kfree(kbuf);

	if (!ok)
		return SYSCALL_RESULT_BAD_USER_DATA;

	return (uint64_t)got;
}

// pipe(int out[2]) — create a pipe; out[0] = read end, out[1] = write end.
//
// The creator ends up holding BOTH ends (readers == writers == 1). A shell
// hands one end to each child (spawn refs them) and then MUST close its own two
// copies — if it doesn't, the writer count never reaches zero, the reader never
// sees EOF, and `a | b` hangs forever. That is the single most common way a
// hand-written shell breaks, so it is worth saying twice.
static uint64_t syscall_pipe(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	pipe_t *p = pipe_create();
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	int rh = handle_alloc(task, HANDLE_PIPE_READ, p);
	int wh = handle_alloc(task, HANDLE_PIPE_WRITE, p);
	if (rh < 0 || wh < 0)
	{
		// Out of handles — unwind cleanly. Close whichever end we DID install
		// via the table (so its refcount drops), and the raw end we didn't.
		if (rh >= 0) handle_close(task, rh); else pipe_close_read_end(p);
		if (wh >= 0) handle_close(task, wh); else pipe_close_write_end(p);
		return SYSCALL_RESULT_INVALID;
	}

	int handles[2] = { rh, wh };
	if (!copy_to_user_buffer((void *)arg0, handles, sizeof(handles)))
	{
		handle_close(task, rh);
		handle_close(task, wh);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	printd(DEBUG_PIPE, "pipe: task %s got handles r=%d w=%d\n", task->exename, rh, wh);
	return 0;
}

// close(handle) — give up one handle.
//
// For a pipe end this is not bookkeeping, it is SIGNALLING: dropping the last
// write end is what delivers EOF to the reader, and dropping the last read end
// is what delivers SIGPIPE to the writer. close() is how a program says "I am
// done with this end" — and in a pipeline, saying so is mandatory.
static uint64_t syscall_close(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	if (!handle_close(task, (int)(int64_t)arg0))
		return SYSCALL_RESULT_INVALID;

	return 0;
}

// ── open / seek ──────────────────────────────────────────────────────────────

// Everything open's kernel-context helper needs, in one kmalloc'd (HHDM)
// block — spawn_params_t's little sibling. path_copy is special: the VFS
// stores the path POINTER it is given (f_path), so open must hand it a string
// with HANDLE lifetime, not syscall lifetime. handle_file_object_close frees
// it when the handle dies; this block itself dies with the syscall.
typedef struct {
	char        mode[4];      // "r"/"w"/"a"/"c" — validated before we get here
	char       *path_copy;    // kmalloc'd, becomes f_path, outlives the syscall
	vfs_file_t *file;         // out: the opened file
	volatile long result;     // 0 on success, negative on failure
} open_params_t;

// Runs under kKernelPML4: the open walks directories, which is disk I/O.
static void open_do(void *arg)
{
	open_params_t *p = (open_params_t *)arg;

	if (kRootFilesystem == NULL || kRootFilesystem->fops == NULL ||
	    kRootFilesystem->fops->open == NULL)
	{
		p->result = -1;
		return;
	}

	p->result = kRootFilesystem->fops->open(&p->file, p->path_copy, p->mode,
	                                        kRootFilesystem);
}

// open(path, mode) — open a file on the root filesystem, return a handle
// (>= 3, the standard streams are never displaced), or a SYSCALL_RESULT_*
// sentinel. mode is a 1-letter string, validated HERE at the boundary:
//   "r" read existing   "w"/"c" create-or-truncate for writing   "a" append
// NULL mode means "r". The handle then plugs into read/write/seek/close —
// and into spawn's redirection slots, where `upper < file` falls out for free.
static uint64_t syscall_open(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	const char *user_path = (const char *)arg0;
	const char *user_mode = (const char *)arg1;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	open_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	// Marshal the path into a scratch buffer first (syscall lifetime), then
	// clone it into path_copy (handle lifetime) once we know its real length.
	char path[TASK_MAX_PATH_LEN];
	if (!copy_user_string(user_path, path, sizeof(path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	if (user_mode == NULL)
	{
		p->mode[0] = 'r';
		p->mode[1] = '\0';
	}
	else if (!copy_user_string(user_mode, p->mode, sizeof(p->mode)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// Boundary validation: exactly one known mode letter. An unrecognized mode
	// would fall through the FAT glue as access-flags 0 — an open that succeeds
	// and then can't read, which is a miserable thing to debug from ring 3.
	if (p->mode[1] != '\0' ||
	    (p->mode[0] != 'r' && p->mode[0] != 'w' &&
	     p->mode[0] != 'a' && p->mode[0] != 'c'))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	size_t plen = 0;
	while (path[plen] != '\0')
		plen++;
	p->path_copy = kmalloc(plen + 1);
	if (p->path_copy == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}
	memcpy(p->path_copy, path, plen + 1);

	p->file = NULL;
	p->result = -1;

	// The directory walk does disk I/O — kernel context required (see open_do).
	call_in_kernel_context(open_do, p);

	if (p->result != 0 || p->file == NULL)
	{
		printd(DEBUG_SYSCALL, "open: task %s: '%s' mode '%s' failed\n",
		       task->exename, p->path_copy, p->mode);
		kfree(p->path_copy);
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // no such file / bad path
	}

	// One handle references this file so far (see handleRefCount in vfs.h) —
	// set BEFORE handle_alloc so no close path can ever see it uninitialized.
	p->file->handleRefCount = 1;

	int h = handle_alloc(task, HANDLE_FILE, p->file);
	if (h < 0)
	{
		// Table full — unwind the open. This also frees path_copy (it is the
		// file's f_path now; the closer owns it from here).
		handle_file_object_close(p->file);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	printd(DEBUG_SYSCALL, "open: task %s: '%s' mode '%s' -> handle %d\n",
	       task->exename, p->path_copy, p->mode, h);
	kfree(p);
	return (uint64_t)h;
}

// seek(handle, offset, whence) — move a file handle's position; returns the
// NEW absolute position (more useful than lseek's "whatever you passed in"),
// or a SYSCALL_RESULT_* sentinel. whence is OS64_SEEK_SET/CUR/END (the ABI
// header) which match the VFS's SEEK_* by design. Seeking a pipe or the
// console is an error — position is a property only files have.
static uint64_t syscall_seek(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL || h->type != HANDLE_FILE)
		return SYSCALL_RESULT_INVALID;

	file_io_params_t *fp = kmalloc(sizeof(*fp));
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;

	fp->file = (vfs_file_t *)h->object;
	fp->offset = (long)(int64_t)arg1;
	fp->whence = (int)(int64_t)arg2;
	fp->result = -1;

	// A FAT seek can walk the cluster chain — disk I/O, kernel context.
	call_in_kernel_context(file_do_seek, fp);

	long pos = fp->result;
	kfree(fp);

	if (pos < 0)
		return SYSCALL_RESULT_INVALID;   // bad whence, or the seek itself failed
	return (uint64_t)pos;
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
	// Redirection for the child's slots 0/1/2, RESOLVED from the parent's
	// handle numbers before we leave the caller's context (handle tables are
	// per-task, and in there we still know who the caller is). HANDLE_NONE
	// means "leave the child's default" — i.e. the console.
	handle_type_t redirType[3];
	void  *redirObject[3];
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
	if (child == NULL)
	{
		p->result = -1;
		return;
	}

	// Apply redirection BEFORE the child is submitted to the scheduler — the
	// child must never get a single instruction of CPU with the wrong handles
	// in slots 0/1/2. It is born already reading from and writing to the right
	// places, and it will never know it was redirected. That is the whole trick
	// behind `a | b`: neither program contains one line of pipe-awareness.
	for (int slot = 0; slot < 3; slot++)
	{
		if (p->redirType[slot] == HANDLE_NONE)
			continue;   // keep the child's default (console)

		// The child gets its OWN reference on the pipe end. Two tasks now hold
		// this end; both must close it before the refcount reaches zero and the
		// EOF/EPIPE fires. (This ref is why the shell closing its copy does not
		// yank the pipe out from under the child.)
		if (p->redirType[slot] == HANDLE_PIPE_READ)
			pipe_ref_read_end((pipe_t *)p->redirObject[slot]);
		else if (p->redirType[slot] == HANDLE_PIPE_WRITE)
			pipe_ref_write_end((pipe_t *)p->redirObject[slot]);
		// Files share by the same rule (handleRefCount, vfs.h): the child's
		// slot is a second reference on ONE open file, and only the last
		// close runs the VFS close. This is what makes `upper < file` safe:
		// the shell opens, spawns, and immediately closes its copy — the
		// child's handle survives because the refcount says so. (Note the
		// child inherits the file POSITION too — shared FIL, dup semantics —
		// which for a just-opened redirect file is position 0, as intended.)
		else if (p->redirType[slot] == HANDLE_FILE)
			__sync_add_and_fetch(&((vfs_file_t *)p->redirObject[slot])->handleRefCount, 1);

		handle_install(child, slot, p->redirType[slot], p->redirObject[slot]);
	}

	scheduler_submit_new_task(child);
	p->result = (long)child->taskID;
}

// spawn(path, argv, in, out, err) — launch `path` as a child of the calling
// task and return its pid, or a SYSCALL_RESULT_* sentinel. Non-blocking: the
// child is submitted to the scheduler and runs concurrently; the caller reaps
// it with wait(). The child inherits the caller's environment (via task_create's
// parent).
//
// in/out/err are the CALLER'S handle numbers to install as the child's 0/1/2,
// or -1 to leave that stream on the console. This is how a shell builds a
// pipeline: spawn(a, ..., -1, pipeW, -1) and spawn(b, ..., pipeR, -1, -1). We
// deliberately do NOT blanket-inherit the parent's whole handle table the way
// Unix does — a child gets the console plus exactly what was asked for, and can
// never accidentally hold some unrelated pipe end open (a classic pipeline hang).
static uint64_t syscall_spawn(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg5;

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

	// Resolve the redirection handles HERE, in the caller's context, where the
	// caller's handle table is the one in scope. spawn_do_create runs under
	// kKernelPML4 and only ever sees the resolved (type, object) pairs.
	int64_t redir[3] = { (int64_t)arg2, (int64_t)arg3, (int64_t)arg4 };
	for (int slot = 0; slot < 3; slot++)
	{
		p->redirType[slot] = HANDLE_NONE;
		p->redirObject[slot] = NULL;

		if (redir[slot] < 0)
			continue;   // -1 = leave this stream on the console

		handle_t *h = (p->parent != NULL) ? handle_get(p->parent, (int)redir[slot]) : NULL;
		if (h == NULL)
		{
			kfree(p);
			return SYSCALL_RESULT_INVALID;   // caller passed a bogus handle
		}

		p->redirType[slot] = h->type;
		p->redirObject[slot] = h->object;
	}

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
