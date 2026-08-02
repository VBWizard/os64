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
#include "allocator.h"  // free_memory — unmap returns pages at the choke point
#include "dlist.h"      // dlist_remove (unmap drops the region's VMA node)
#include "memory/mmap.h"   // MAP_ANONYMOUS (map()'s regions are anonymous)
#include "CONFIG.h"        // TICKS_PER_SECOND — sleep()'s ms→ticks boundary
#include "os64/ticks.h"    // os64_ticks_t — the ticks() out-struct (abi)
#include "os64/memory.h"   // os64_memory_t — the memory() out-struct (abi)
#include "os64/time.h"     // os64_time_t — the time() out-struct (abi)
#include "env.h"           // env_set/env_unset — setenv() mutates the task's env block
#include "os64/net.h"      // os64_netdest_t — net_dial's in-struct (abi)
#include "driver/net/net_device.h"   // kNetDevices — dial needs a NIC to dial on
#include "driver/net/net_wire.h"     // NET_IPV4_OCTETS — address logging
#include "driver/net/udp_conn.h"     // the object behind HANDLE_NET_UDP
#include "driver/net/tcp.h"          // ...and HANDLE_NET_TCP
#include "driver/net/icmp_conn.h"    // ...and HANDLE_NET_ICMP

// The monotonic tick counter (kernel.h) — read by sleep()'s deadline math
// and handed to ring 3 by ticks().
extern volatile uint64_t kTicksSinceStart;
extern volatile uint64_t kSystemCurrentTime;   // UTC epoch seconds (timer IRQ advances it)
extern volatile uint64_t irq0_current_count;   // ticks into the current second (same IRQ)
extern uint64_t kTicksPerSecond;
extern int kTimeZone;                          // configured zone, HOURS east of UTC
extern uint64_t kTotalMemory;      // installed RAM (memmap.c, Limine map sum)
extern uint64_t kAvailableMemory;  // USABLE entries only — what the allocator governs

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
static void raise_terminating_signal_and_die(task_t *task, thread_t *thread);   // defined with its SIGPIPE twin below
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
static uint64_t syscall_readdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_map(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_getcwd(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_chdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_stat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_reap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_ticks(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_memory(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_printat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_time(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_setenv(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_net_dial(uint64_t arg0, uint64_t arg1, uint64_t arg2,
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
	SYSCALL_DEFINE(SYSCALL_READDIR,   "readdir",   syscall_readdir,   false, 0x02),  // arg1 = os64_dirent_t out ptr
	SYSCALL_DEFINE(SYSCALL_MAP,       "map",       syscall_map,       false, 0x00),  // arg0 = length (not a pointer)
	SYSCALL_DEFINE(SYSCALL_UNMAP,     "unmap",     syscall_unmap,     false, 0x01),  // arg0 = region base (user VA)
	SYSCALL_DEFINE(SYSCALL_GETCWD,    "getcwd",    syscall_getcwd,    false, 0x01),  // arg0 = out buffer
	SYSCALL_DEFINE(SYSCALL_CHDIR,     "chdir",     syscall_chdir,     false, 0x01),  // arg0 = path
	SYSCALL_DEFINE(SYSCALL_STAT,      "stat",      syscall_stat,      false, 0x03),  // arg0 = path, arg1 = dirent out ptr
	SYSCALL_DEFINE(SYSCALL_REAP,      "reap",      syscall_reap,      false, 0x01),  // arg0 = exit-code out ptr
	SYSCALL_DEFINE(SYSCALL_SLEEP,     "sleep",     syscall_sleep,     false, 0x00),  // arg0 = milliseconds (a value, no pointers)
	SYSCALL_DEFINE(SYSCALL_TICKS,     "ticks",     syscall_ticks,     false, 0x01),  // arg0 = os64_ticks_t out ptr
	SYSCALL_DEFINE(SYSCALL_MEMORY,    "memory",    syscall_memory,    false, 0x01),  // arg0 = os64_memory_t out ptr
	SYSCALL_DEFINE(SYSCALL_PRINTAT,   "printat",   syscall_printat,   false, 0x04),  // arg0 = x cell, arg1 = y cell, arg2 = string
	SYSCALL_DEFINE(SYSCALL_TIME,      "time",      syscall_time,      false, 0x01),  // arg0 = os64_time_t out ptr
	SYSCALL_DEFINE(SYSCALL_SETENV,    "setenv",    syscall_setenv,    false, 0x01),  // arg0 = key; arg1 = value OR NULL (mask excludes it: NULL means unset)
	SYSCALL_DEFINE(SYSCALL_NET_DIAL,  "net_dial",  syscall_net_dial,  false, 0x01),  // arg0 = os64_netdest_t in ptr
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

	// The terminate checkpoint. Every task that is DOING anything passes
	// through here constantly (cat's write loop = thousands of crossings a
	// second), so a pending terminate — set at a keystroke (Ctrl+C) or by a
	// write to /proc/<id>/ctl — is enforced within microseconds, and in the
	// victim's own context, where task_exit is safe. Tasks that are BLOCKED
	// instead get here via their woken blocking loops (see
	// raise_terminating_signal_and_die). Only a syscall-free ring-3 spin
	// evades this boundary entirely — and the forced-syscall push in
	// scheduler.c closes that gap.
	{
		core_local_storage_t *sig_cls = get_core_local_storage();
		thread_t *sig_thread = sig_cls ? sig_cls->currentThread : NULL;
		if (sig_thread && (sig_thread->signals.sigind & SIGNALS_TERMINATING))
			raise_terminating_signal_and_die(sig_cls->task, sig_thread);
	}

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

// ── User-range pre-validation ────────────────────────────────────────────────
// The copy helpers below memcpy through user VAs in ring 0.  A fault during
// that copy is FINE when the page is demand-pageable — the #PF handler maps it
// and the access retries — but a WILD pointer (no VMA, nothing mapped) faults
// in KERNEL mode, and a kernel-mode fault with no VMA is, correctly, a panic.
// An app must not be able to panic the OS by handing a syscall a garbage
// pointer (ls did exactly that, 2026-07-22), so every page of a user range is
// vetted before the memcpy touches it:
//   - VMA-covered → legal: a not-present page is just demand paging waiting
//     to happen.  For copy-OUT the VMA must also be writable — a store to a
//     read-only page would be a ring-0 protection violation, another panic
//     door.  (CoW pages pass correctly: their VMA says PROT_WRITE and the
//     write fault resolves through the CoW branch.)
//   - No VMA but present in the task's tables → legal: covers kernel-created
//     eager mappings; the PTE's own W bit answers the writability question.
//   - Neither → wild pointer; reject, so the syscall fails with
//     SYSCALL_RESULT_BAD_USER_DATA instead of the kernel faulting.
static bool user_range_accessible(const void *user_ptr, size_t length, bool for_write)
{
	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return false;

	uintptr_t addr = (uintptr_t)user_ptr;
	uintptr_t end = addr + length;   // no overflow: callers range-check vs kHHDMOffset first

	for (uintptr_t page = addr & ~(uintptr_t)(PAGE_SIZE - 1); page < end; page += PAGE_SIZE)
	{
		vma_t *vma = vma_lookup(task, page);
		if (vma != NULL)
		{
			if (for_write && !(vma->prot & PROT_WRITE))
				return false;
			continue;
		}

		uintptr_t pte = paging_walk_paging_table_keep_flags((pt_entry_t *)task->pml4v, page, true);
		if (pte == 0xbadbadba || !(pte & PAGE_PRESENT))
			return false;
		if (for_write && !(pte & PAGE_WRITE))
			return false;
	}

	return true;
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

	if (!user_range_accessible(user_ptr, length, false))
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

        if (!user_range_accessible(user_dst, length, true))
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
// ── The terminating signals: enforcing the default action (SIGINT.md, PROC.md)
// Same "kernel enforces the default because ring 3 can't catch it" pattern as
// SIGPIPE below, same 128+signo retVal encoding for a waiting parent. The bit
// is set somewhere the victim is NOT running — at the KEYSTROKE
// (console_intr_intercept, IRQ path — cat writing a huge file is not reading
// the console, so a buffered byte could never work), or by another task
// writing to /proc/<id>/ctl. The KILL happens here, at the victim's own
// syscall boundary, in its own context — free to sleep, safe to close handles,
// through the very same task_exit path a voluntary death takes. Three roads
// lead here:
//   1. the dispatcher check in _syscall_dispatch (a busy task's next syscall)
//   2. console_read returning CONSOLE_READ_INTERRUPTED (blocked on stdin)
//   3. pipe_read/pipe_write returning PIPE_ERR_INTERRUPTED (blocked on a pipe)
//
// SIGKILL and SIGINT both terminate today (nothing in ring 3 can catch either
// yet) but they are NOT the same event, and the exit status must not pretend
// they are: a task killed through ctl did not die "interrupted from the
// keyboard". SIGKILL wins when both are pending — the uncatchable one always
// outranks the catchable one.
// `thread` may be NULL: all four call sites run in the VICTIM'S OWN context
// (that is the whole design), so the core's current thread is the right one
// to ask which bit is pending.
static void raise_terminating_signal_and_die(task_t *task, thread_t *thread)
{
	if (thread == NULL)
	{
		core_local_storage_t *cls = get_core_local_storage();
		thread = cls ? cls->currentThread : NULL;
	}

	bool killed = (thread != NULL) && (thread->signals.sigind & SIGKILL);

	if (task != NULL)
	{
		task->retVal = killed ? SIGNALS_EXIT_SIGKILL : SIGNALS_EXIT_SIGINT;
		printd(DEBUG_TASK, "%s: task %s terminating (exit %lu)\n",
			killed ? "SIGKILL" : "SIGINT", task->exename, task->retVal);
	}

	task_exit();
	__builtin_unreachable();
}

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

// The per-thread bounce block behind thread_t.syscallIOScratch: one
// file_io_params_t header followed by READ_CHUNK_SIZE data bytes, allocated on
// the thread's first read()/write() and reused for every one after. This
// replaced a kmalloc/kfree PER CALL — which, with a no-freelist allocator,
// meant a fresh zeroed page on the way in and a TLB-shootdown IPI to every
// core on the way out, per syscall. A program reading a file byte-at-a-time
// (os64_readline's pipe-safe mode) paid that toll per BYTE: the first top
// spent ~3 seconds printing 30 tasks, nearly all of it right here.
// kmalloc'd = upper half = visible under kKernelPML4 AND every task CR3,
// which is exactly what call_in_kernel_context demands of the params block
// and buffer anyway. Per-thread (not per-core) because console/pipe reads
// BLOCK while holding the data area — see the field's comment in thread.h.
// Reuse across calls is safe: a thread runs one syscall at a time, and both
// consumers (read's bounce, write's bounce) are done with the block before
// the syscall returns. Returns NULL only if the one-time allocation fails.
static file_io_params_t *syscall_io_scratch(char **data_out)
{
	core_local_storage_t *cls = get_core_local_storage();
	thread_t *thread = cls ? cls->currentThread : NULL;
	if (thread == NULL)
		return NULL;

	if (thread->syscallIOScratch == NULL)
		thread->syscallIOScratch = kmalloc(sizeof(file_io_params_t) + READ_CHUNK_SIZE);
	if (thread->syscallIOScratch == NULL)
		return NULL;

	if (data_out != NULL)
		*data_out = (char *)thread->syscallIOScratch + sizeof(file_io_params_t);
	return (file_io_params_t *)thread->syscallIOScratch;
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

				// The common case — a line of text, a filter's buffer — fits
				// the thread's scratch block: no allocation at all. Only a
				// write bigger than the scratch kmallocs, because atomicity
				// demands ONE piece up to PIPE_CAPACITY (see above) and that
				// can genuinely exceed READ_CHUNK_SIZE. Safe to hold the
				// scratch across pipe_write's block: it's THIS thread's.
				char *scratch_data = NULL;
				char *kbuf;
				if (this_chunk <= READ_CHUNK_SIZE &&
				    syscall_io_scratch(&scratch_data) != NULL)
					kbuf = scratch_data;
				else
				{
					kbuf = kmalloc(this_chunk);
					if (kbuf == NULL)
						return written ? written : SYSCALL_RESULT_INVALID;
				}

				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
				{
					if (kbuf != scratch_data)
						kfree(kbuf);
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;
				}

				long n = pipe_write(p, kbuf, this_chunk);   // BLOCKS if full
				if (kbuf != scratch_data)
					kfree(kbuf);

				if (n == PIPE_ERR_CLOSED)
				{
					// Nobody is left to read this. Default action: terminate.
					raise_sigpipe_and_die(task);
					__builtin_unreachable();
				}
				if (n == PIPE_ERR_INTERRUPTED)
				{
					// Ctrl+C landed while blocked on (or headed into) this
					// pipe write. Same rail, different signal: terminate, 130.
					raise_terminating_signal_and_die(task, NULL);
					__builtin_unreachable();
				}
				if (n < 0)
					return written ? written : SYSCALL_RESULT_INVALID;

				written += (size_t)n;
			}
			return written;
		}

		case HANDLE_NET_TCP:
		{
			// A stream write: no atomicity promise and no size limit —
			// tcp_conn_write segments it and blocks until every byte is
			// acknowledged. Bounce through kernel memory in MSS-ish
			// chunks for the same fault discipline as every other write.
			size_t written = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > READ_CHUNK_SIZE)
					this_chunk = READ_CHUNK_SIZE;

				char *kbuf = kmalloc(this_chunk);
				if (kbuf == NULL)
					return written ? written : SYSCALL_RESULT_INVALID;
				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
				{
					kfree(kbuf);
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;
				}

				long n = tcp_conn_write((tcp_conn_t *)h->object, kbuf, this_chunk);
				kfree(kbuf);

				if (n == TCP_ERR_INTERRUPTED)
				{
					raise_terminating_signal_and_die(task, NULL);
					__builtin_unreachable();
				}
				if (n < 0)
					return written ? written : SYSCALL_RESULT_INVALID;
				written += (size_t)n;
				if ((size_t)n < this_chunk)
					break;   // connection died mid-write: report progress
			}
			return written;
		}

		case HANDLE_NET_ICMP:
		{
			// One write = ONE echo request carrying these bytes. Same
			// atomic-datagram contract as UDP; the payload comes home
			// echoed, which is the whole mechanism `ping` measures with.
			if (length > ICMP_CONN_MAX_PAYLOAD)
				return SYSCALL_RESULT_INVALID;

			char *kbuf = kmalloc(length);
			if (kbuf == NULL)
				return SYSCALL_RESULT_INVALID;
			if (!copy_user_buffer(user_buffer, kbuf, length))
			{
				kfree(kbuf);
				return SYSCALL_RESULT_BAD_USER_DATA;
			}
			long n = icmp_conn_write((icmp_conn_t *)h->object, kbuf, length);
			kfree(kbuf);
			return (n < 0) ? SYSCALL_RESULT_INVALID : (uint64_t)n;
		}

		case HANDLE_NET_UDP:
		{
			// One write = ONE datagram, atomic by protocol: an oversize
			// write is an ERROR, never a fragmenting loop (os64/net.h
			// states the contract; UDP_CONN_MAX_DGRAM is the physics).
			// Kernel-bounce first for the same fault-discipline as pipes.
			if (length > UDP_CONN_MAX_DGRAM)
				return SYSCALL_RESULT_INVALID;

			char *kbuf = kmalloc(length);
			if (kbuf == NULL)
				return SYSCALL_RESULT_INVALID;
			if (!copy_user_buffer(user_buffer, kbuf, length))
			{
				kfree(kbuf);
				return SYSCALL_RESULT_BAD_USER_DATA;
			}

			long n = udp_conn_write((udp_conn_t *)h->object, kbuf, length);
			kfree(kbuf);
			return (n < 0) ? SYSCALL_RESULT_INVALID : (uint64_t)n;
		}

		case HANDLE_FILE:
		{
			// Ferry through the thread's bounce block, chunk by chunk. Unlike
			// the pipe path there is no cross-writer atomicity promise to
			// keep — a file write that lands in pieces is still one write —
			// so chunking costs nothing but loop iterations. The scratch is
			// kmalloc'd (once, at first use): file_do_write runs under
			// kKernelPML4, which cannot see this syscall's stack.
			char *kbuf = NULL;
			file_io_params_t *fp = syscall_io_scratch(&kbuf);
			if (fp == NULL)
				return SYSCALL_RESULT_INVALID;

			size_t written = 0;
			while (written < length)
			{
				size_t this_chunk = length - written;
				if (this_chunk > READ_CHUNK_SIZE)
					this_chunk = READ_CHUNK_SIZE;

				// Copy from user space HERE, on the caller's CR3, where the
				// user buffer actually resolves (and where a demand-page fault
				// is safe — no locks held, kernel context not yet entered).
				if (!copy_user_buffer(user_buffer + written, kbuf, this_chunk))
					return written ? written : SYSCALL_RESULT_BAD_USER_DATA;

				fp->file = (vfs_file_t *)h->object;
				fp->buf = kbuf;
				fp->len = this_chunk;
				fp->result = -1;
				call_in_kernel_context(file_do_write, fp);

				if (fp->result < 0)
					return written ? written : SYSCALL_RESULT_INVALID;

				written += (size_t)fp->result;
				if ((size_t)fp->result < this_chunk)
					break;   // short write: filesystem/device is full — report progress
			}
			return written;
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
	// The thread-owned bounce block (see syscall_io_scratch): no allocation,
	// no free, no per-call TLB shootdown — and therefore no kfree on ANY exit
	// path below, which is why the error paths got shorter.
	char *kbuf = NULL;
	file_io_params_t *fp = syscall_io_scratch(&kbuf);
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;

	long got = 0;
	switch (h->type)
	{
		case HANDLE_CONSOLE_IN:
			// Blocks until >=1 byte is available (terminal semantics). The
			// scratch is safe to hold across the block — it's THIS thread's.
			got = console_read(kbuf, want);
			if (got == CONSOLE_READ_INTERRUPTED)
			{
				// Ctrl+C landed while (or before) we were blocked on stdin.
				// Default action: terminate. The sentinel never reaches ring 3.
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			break;

		case HANDLE_PIPE_READ:
			// Blocks until >=1 byte is available, OR the last writer closes —
			// which returns 0, and 0 is EOF. (EOF is the absence of writers.)
			// Copying into a KERNEL buffer, not straight to user space, for the
			// same reason write() does the reverse: pipe_read copies under the
			// pipe spinlock with interrupts off, and touching user memory there
			// could demand-page into a deadlock.
			got = pipe_read((pipe_t *)h->object, kbuf, want);
			if (got == PIPE_ERR_INTERRUPTED)
			{
				// Ctrl+C landed while blocked on (or headed into) a pipe read.
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			break;

		case HANDLE_NET_TCP:
			// A stream read: blocks for at least one byte, returns SHORT,
			// and returns 0 at EOF once the peer's FIN drains — the exact
			// contract a pipe read has, which is why `cat` over a socket
			// would need no new code at all.
			got = tcp_conn_read((tcp_conn_t *)h->object, kbuf, want);
			if (got == TCP_ERR_INTERRUPTED)
			{
				kfree(kbuf);
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			if (got < 0)
			{
				// RST or a dead connection — distinct from EOF's clean 0.
				kfree(kbuf);
				return SYSCALL_RESULT_INVALID;
			}
			break;

		case HANDLE_NET_ICMP:
			// Blocks until an echo reply carrying OUR identifier comes
			// back, then returns the payload we sent, as the peer echoed
			// it. (What `ping` does with those bytes — a timestamp,
			// a pattern check — is `ping`'s business.)
			got = icmp_conn_read((icmp_conn_t *)h->object, kbuf, want);
			if (got == ICMP_CONN_ERR_INTERRUPTED)
			{
				kfree(kbuf);
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			break;

		case HANDLE_NET_UDP:
			// Blocks until one datagram arrives from the dialed peer, then
			// returns exactly that datagram (short if the buffer is smaller;
			// the tail drops — the truncation contract in os64/net.h).
			// `want` is already min(length, READ_CHUNK_SIZE=4096), which
			// exceeds UDP_CONN_MAX_DGRAM=1472, so no datagram is ever
			// clipped by the bounce buffer — only by the CALLER's length.
			got = udp_conn_read((udp_conn_t *)h->object, kbuf, want);
			if (got == UDP_CONN_ERR_INTERRUPTED)
			{
				// Ctrl+C landed while blocked waiting for a packet — same
				// rail as console and pipe reads: terminate, 130.
				kfree(kbuf);
				raise_terminating_signal_and_die(task, NULL);
				__builtin_unreachable();
			}
			break;

		case HANDLE_FILE:
		{
			// Never blocks (a file always knows its bytes) and returns SHORT at
			// the end: fewer bytes than asked near EOF, then 0 AT EOF — so the
			// canonical filter loop works on a file with zero special-casing.
			// The actual read runs under kKernelPML4 (see file_do_read); the
			// scratch block is kmalloc'd (once), reachable from both worlds.
			fp->file = (vfs_file_t *)h->object;
			fp->buf = kbuf;
			fp->len = want;
			fp->result = -1;
			call_in_kernel_context(file_do_read, fp);
			got = fp->result;

			if (got < 0)
			{
				// A real device/filesystem error — distinct from EOF's clean 0.
				return SYSCALL_RESULT_INVALID;
			}
			break;
		}

		default:
			return SYSCALL_RESULT_INVALID;   // a write-only handle
	}

	if (got <= 0)
		return 0;   // EOF (pipe) or nothing (console)

	if (!copy_to_user_buffer(user_buffer, kbuf, (size_t)got))
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

// net_dial(dest) — open a network conversation and hand back a handle.
//
// The whole ratified API doctrine lands in this one handler: a TYPED STRUCT
// crosses the boundary (never a dial string — libos64 parses those, ruling
// #1), every field HOST-order (ruling #2), and what comes back is an
// ordinary handle whose read/write ARE the datagram verbs (ruling #4) —
// dispatched by the same switches that serve pipes and files, because a
// conversation is just one more thing a handle can name.
static uint64_t syscall_net_dial(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	os64_netdest_t dest;
	if (!copy_user_buffer((const void *)arg0, &dest, sizeof(dest)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	if (dest.ip == 0)
		return SYSCALL_RESULT_INVALID;
	if (dest.protocol != OS64_NET_UDP && dest.protocol != OS64_NET_TCP &&
	    dest.protocol != OS64_NET_ICMP)
		return SYSCALL_RESULT_INVALID;   // no wildcard: os64 says what it means
	// A port of 0 is meaningless for the port protocols; ICMP has no ports
	// at all (its identifier is kernel-assigned), so the field is ignored.
	if (dest.port == 0 && dest.protocol != OS64_NET_ICMP)
		return SYSCALL_RESULT_INVALID;

	// Dial tone requires a line: no NIC, no conversation. (A netless boot
	// is a configuration, not an error — but dialing on one is an error.)
	if (kNetDeviceCount == 0)
		return SYSCALL_RESULT_INVALID;

	// One syscall, two protocols, two object types behind two handle tags —
	// and from the caller's side the difference shows up only in what
	// read/write MEAN (datagrams vs bytes), exactly as ruling #1 intended
	// when it made the protocol segment the verb of the call.
	void *conn;
	handle_type_t tag;
	if (dest.protocol == OS64_NET_TCP)
	{
		// BLOCKS through the three-way handshake (or a refusal, or the
		// connect timeout) — a dial that returns has a live stream.
		conn = tcp_conn_dial(kNetDevices[0], dest.ip, dest.port);
		tag = HANDLE_NET_TCP;
	}
	else if (dest.protocol == OS64_NET_ICMP)
	{
		// Nothing to negotiate: echo has no handshake, so the dial only
		// allocates the identifier that makes replies findable.
		conn = icmp_conn_dial(kNetDevices[0], dest.ip);
		tag = HANDLE_NET_ICMP;
	}
	else
	{
		conn = udp_conn_dial(kNetDevices[0], dest.ip, dest.port);
		tag = HANDLE_NET_UDP;
	}
	if (conn == NULL)
		return SYSCALL_RESULT_INVALID;

	int h = handle_alloc(task, tag, conn);
	if (h < 0)
	{
		// Out of handle slots — hang up cleanly on whichever we opened.
		if      (tag == HANDLE_NET_TCP)  tcp_conn_close((tcp_conn_t *)conn);
		else if (tag == HANDLE_NET_ICMP) icmp_conn_close((icmp_conn_t *)conn);
		else                             udp_conn_close((udp_conn_t *)conn);
		return SYSCALL_RESULT_INVALID;
	}

	const char *proto_name = (dest.protocol == OS64_NET_TCP)  ? "tcp" :
	                         (dest.protocol == OS64_NET_ICMP) ? "icmp" : "udp";
	printd(DEBUG_NET, "net_dial: task %s -> %s!%u.%u.%u.%u!%u = handle %d\n",
	       task->exename, proto_name, NET_IPV4_OCTETS(dest.ip), dest.port, h);
	return (uint64_t)h;
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
	char        mode[4];      // "r"/"w"/"a"/"c"/"d" — validated before we get here
	char       *path_copy;    // kmalloc'd, becomes f_path, outlives the syscall
	vfs_filesystem_t *fs;     // mount-resolved BEFORE kernel context (pure
	                          // string matching); path_copy is the fs-local
	                          // TAIL, which is what the fs must see AND what
	                          // f_path must be (the handle closer kfree's
	                          // f_path — it has to be a base pointer)
	vfs_file_t *file;         // out: the opened file       (file modes)
	vfs_directory_t *dir;     // out: the opened directory  (mode "d")
	volatile long result;     // 0 on success, negative on failure
} open_params_t;

// Runs under kKernelPML4: the open walks directories, which is disk I/O.
static void open_do(void *arg)
{
	open_params_t *p = (open_params_t *)arg;

	// Mode "d" opens a DIRECTORY for readdir() — same syscall, same handle
	// table, different tag. "One handle type for everything" means opendir
	// is just open asking for a different thing back.
	if (p->mode[0] == 'd')
	{
		if (p->fs->dops == NULL || p->fs->dops->open == NULL)
		{
			p->result = -1;
			return;
		}
		p->result = p->fs->dops->open(&p->dir, p->path_copy, p->fs);
		return;
	}

	if (p->fs->fops == NULL || p->fs->fops->open == NULL)
	{
		p->result = -1;
		return;
	}

	p->result = p->fs->fops->open(&p->file, p->path_copy, p->mode, p->fs);
}

// Resolve a just-copied user path against the task's cwd into canonical
// absolute form. THE relative-path choke point: open (files AND dirs), spawn,
// and chdir all pass through here, which is what makes `cd /bin` + `hello`
// (or ls with a bare "dir1") work everywhere at once — and means no
// filesystem driver ever sees a "..". Returns false if the result overflows.
static bool resolve_user_path(task_t *task, const char *in, char *out, size_t outlen)
{
	const char *cwd = (task != NULL && task->cwd != NULL) ? task->cwd : "/";
	return vfs_canonicalize_path(cwd, in, out, outlen) == 0;
}

// readdir's kernel-context half: one dops->read per call, into the HHDM
// params block (see syscall_readdir below).
typedef struct {
	vfs_directory_t *dir;
	os64_dirent_t entry;    // out, when result == 1
	volatile long result;   // 1 = entry, 0 = end of directory, <0 = error
} readdir_params_t;

static void readdir_do(void *arg)
{
	readdir_params_t *p = (readdir_params_t *)arg;
	vfs_directory_t *d = p->dir;
	p->result = (d->dops != NULL && d->dops->read != NULL)
	                ? d->dops->read(d, &p->entry) : -1;
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
	// Between the two: resolve against the task's cwd — open("notes.txt")
	// means "here", and "here" is kernel state now.
	char raw[TASK_MAX_PATH_LEN];
	char path[TASK_MAX_PATH_LEN];
	if (!copy_user_string(user_path, raw, sizeof(raw)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	if (!resolve_user_path(task, raw, path, sizeof(path)))
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
	// ("d" = directory, for readdir — see open_do.)
	if (p->mode[1] != '\0' ||
	    (p->mode[0] != 'r' && p->mode[0] != 'w' &&
	     p->mode[0] != 'a' && p->mode[0] != 'c' && p->mode[0] != 'd'))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// Route the canonical path to its mounted filesystem HERE, before kernel
	// context — the resolver is pure string matching against the mount table
	// (kernel .data, visible from any CR3). What gets cloned below is the
	// fs-local TAIL: the fs stores that pointer as f_path and the handle
	// closer kfree's it, so it must be a base pointer, never path+offset.
	const char *tail = NULL;
	p->fs = vfs_resolve_mount(path, &tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	size_t plen = 0;
	while (tail[plen] != '\0')
		plen++;
	p->path_copy = kmalloc(plen + 1);
	if (p->path_copy == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}
	memcpy(p->path_copy, tail, plen + 1);

	p->file = NULL;
	p->dir = NULL;
	p->result = -1;
	bool is_dir = (p->mode[0] == 'd');

	// The directory walk does disk I/O — kernel context required (see open_do).
	call_in_kernel_context(open_do, p);

	if (p->result != 0 || (is_dir ? (void *)p->dir : (void *)p->file) == NULL)
	{
		printd(DEBUG_SYSCALL, "open: task %s: '%s' mode '%s' failed\n",
		       task->exename, path, p->mode);   // full path — tail loses the mount
		kfree(p->path_copy);
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // no such file/directory / bad path
	}

	int h;
	if (is_dir)
	{
		// Tag mount roots for mount-aware readdir: an fs-local path of "/"
		// means this handle opened the top of its mount, so readdir must
		// append the mount points living directly under it once the fs's
		// own entries end (see vfs_readdir_child_mounts). The prefix is
		// found by the fs pointer — one mount per fs by GUID dedupe.
		if (p->path_copy[0] == '/' && p->path_copy[1] == '\0')
			for (int i = 0; i < kMountCount; i++)
				if (kMountTable[i].fs == p->fs)
				{
					p->dir->mount_prefix = kMountTable[i].prefix;
					p->dir->mount_prefix_len = kMountTable[i].prefix_len;
					break;
				}

		// Directories carry no refcount — spawn redirection rejects them, so
		// this handle is the object's one and only owner (see handle.h).
		h = handle_alloc(task, HANDLE_DIR, p->dir);
		if (h < 0)
		{
			handle_dir_object_close(p->dir);   // also frees path_copy (f_path)
			kfree(p);
			return SYSCALL_RESULT_INVALID;
		}
	}
	else
	{
		// One handle references this file so far (see handleRefCount in
		// vfs.h) — set BEFORE handle_alloc so no close path can ever see it
		// uninitialized.
		p->file->handleRefCount = 1;

		h = handle_alloc(task, HANDLE_FILE, p->file);
		if (h < 0)
		{
			// Table full — unwind the open. This also frees path_copy (it is
			// the file's f_path now; the closer owns it from here).
			handle_file_object_close(p->file);
			kfree(p);
			return SYSCALL_RESULT_INVALID;
		}
	}

	printd(DEBUG_SYSCALL, "open: task %s: '%s' mode '%s' -> handle %d\n",
	       task->exename, path, p->mode, h);   // full path — tail loses the mount
	kfree(p);
	return (uint64_t)h;
}

// readdir(handle, entry_out) — produce the next entry of an open directory
// (a handle from open(path, "d")). Returns 1 = *entry_out filled, 0 = end of
// directory, or a SYSCALL_RESULT_* sentinel. The entry is the fs-neutral
// os64_dirent_t (abi/include/os64/dirent.h): name, size, and a DIR flag in
// one call — no per-entry stat() dance.
static uint64_t syscall_readdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	handle_t *h = handle_get(task, (int)(int64_t)arg0);
	if (h == NULL || h->type != HANDLE_DIR)
		return SYSCALL_RESULT_INVALID;   // not a directory handle

	// Params + entry in one HHDM block: the dops->read runs under kKernelPML4
	// (directory entries come off the disk), the copy-out runs back on the
	// caller's CR3 — the block must be visible to both.
	readdir_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;
	p->dir = (vfs_directory_t *)h->object;
	p->result = -1;

	call_in_kernel_context(readdir_do, p);

	long r = p->result;

	// The fs's real entries are done: if this dir is a mount root, the mount
	// points under it come next — namespace content no on-disk filesystem
	// can know about. Pure mount-table scan, fine on the caller's CR3.
	if (r == 0)
		r = vfs_readdir_child_mounts(p->dir, &p->entry);

	if (r == 1 && !copy_to_user_buffer((void *)arg1, &p->entry, sizeof(p->entry)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}
	kfree(p);

	if (r < 0)
		return SYSCALL_RESULT_INVALID;
	return (uint64_t)r;   // 1 = entry delivered, 0 = end of directory
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

	// The thread's bounce block (params header only; the data area is idle
	// here). Seek sits on readline's fast path now — probe + surplus
	// seek-back per line — so it sheds its per-call kmalloc/kfree too.
	file_io_params_t *fp = syscall_io_scratch(NULL);
	if (fp == NULL)
		return SYSCALL_RESULT_INVALID;

	fp->file = (vfs_file_t *)h->object;
	fp->offset = (long)(int64_t)arg1;
	fp->whence = (int)(int64_t)arg2;
	fp->result = -1;

	// A FAT seek can walk the cluster chain — disk I/O, kernel context.
	call_in_kernel_context(file_do_seek, fp);

	long pos = fp->result;

	if (pos < 0)
		return SYSCALL_RESULT_INVALID;   // bad whence, or the seek itself failed
	return (uint64_t)pos;
}

// ── map / unmap ──────────────────────────────────────────────────────────────
// THE heap primitive — the ratified no-brk design (DEBTS): the kernel hands
// out whole demand-paged anonymous regions; userland's malloc carves them up.
// There is deliberately no brk/sbrk and never will be: regions are
// independent, so memory can be given back from the MIDDLE of the heap —
// the structural fix for brk's only-shrink-from-the-end flaw.

// map(len) — allocate a fresh anonymous region of at least `len` bytes
// (rounded up to whole pages), demand-paged and GUARANTEED ZEROED (the
// allocator zeroes at its choke point; the demand pager hands those pages
// straight over). Returns the region's base address, or a sentinel.
//
// This is almost embarrassingly thin, and that's the design: creating a
// mapping is just RECORDING INTENT (one VMA node). No pages move until the
// task actually touches memory — first touch faults, the demand pager
// resolves it, page by page, only for pages actually used.
static uint64_t syscall_map(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	size_t length = (size_t)arg0;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || length == 0)
		return SYSCALL_RESULT_INVALID;

	size_t rounded = (length + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

	// The heap VA allocator is a bump pointer: heapEnd starts at
	// TASK_HEAP_START and only ever advances. Freed regions' VAs are never
	// reused (v1) — with a 47-bit heap range, address space is the cheapest
	// resource this kernel owns, and never-reuse means a stale pointer into
	// an unmapped region ALWAYS faults instead of aliasing a new region.
	// (A use-after-unmap tripwire for free — same philosophy as the HHDM.)
	uintptr_t base = task->heapEnd;
	if (base + rounded >= TASK_HEAP_END || base + rounded < base)
		return SYSCALL_RESULT_INVALID;   // out of heap VA (a 47-bit feat)

	vma_t *vma = vma_create(base, base + rounded, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
	if (vma == NULL)
		return SYSCALL_RESULT_INVALID;
	vma_add(task, vma);

	// Advance past the region PLUS one permanently-unmapped guard page:
	// running off the end of a region faults instead of silently scribbling
	// on the next one. malloc overruns announce themselves here.
	task->heapEnd = base + rounded + PAGE_SIZE;

	printd(DEBUG_SYSCALL, "map: task %s: %lu bytes at 0x%016lx\n",
	       task->exename, rounded, base);
	return (uint64_t)base;
}

// unmap(base) — release an ENTIRE region previously returned by map(). Whole
// regions only, by design: partial unmap invites split-VMA bookkeeping for a
// need malloc doesn't have (it gives back the regions it took). Frees every
// page the task actually touched (untouched pages were never allocated —
// that's demand paging's other half) and drops the VMA.
static uint64_t syscall_unmap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uintptr_t base = (uintptr_t)arg0;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->mmaps == NULL)
		return SYSCALL_RESULT_INVALID;

	// Find the region by EXACT base — and only accept regions map() made
	// (anonymous, heap-range). Without this gate, userland could unmap its
	// own code segment and the next instruction fetch becomes a kernel
	// panic; a bad handle should hurt the caller, not the kernel.
	dlist_node_t *node = task->mmaps->head;
	vma_t *vma = NULL;
	while (node != NULL)
	{
		vma_t *v = (vma_t *)node->data;
		if (v != NULL && v->start == base)
		{
			vma = v;
			break;
		}
		node = node->next;
	}
	if (vma == NULL || !(vma->flags & MAP_ANONYMOUS) || base < TASK_HEAP_START)
		return SYSCALL_RESULT_INVALID;

	// Give back every page that was actually faulted in. paging_unmap_page
	// invlpg's locally; pages never touched have no PTE and nothing to free.
	for (uintptr_t va = vma->start; va < vma->end; va += PAGE_SIZE)
	{
		uintptr_t phys = paging_walk_paging_table((pt_entry_t *)task->pml4v, va);
		if (phys != 0 && phys != 0xbadbadba)
		{
			paging_unmap_page((pt_entry_t *)task->pml4v, va);
			// The allocator choke point: also HHDM-unmaps + TLB-shoots the
			// kernel alias, per the lazy-HHDM rules.
			free_memory(phys);
		}
	}

	printd(DEBUG_SYSCALL, "unmap: task %s: region 0x%016lx-0x%016lx released\n",
	       task->exename, vma->start, vma->end);

	dlist_remove(task->mmaps, node);
	vma_destroy(vma);
	return 0;
}

// ── getcwd / chdir ───────────────────────────────────────────────────────────

// getcwd(buf, len) — copy the task's current working directory (a canonical
// absolute path — chdir guarantees it) into the user buffer. Returns the
// path's length, or a sentinel if the buffer is too small. The trivial half
// of the pair: the string is already sitting in the task struct.
static uint64_t syscall_getcwd(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->cwd == NULL)
		return SYSCALL_RESULT_INVALID;

	size_t len = 0;
	while (task->cwd[len] != '\0')
		len++;
	if ((size_t)arg1 < len + 1)
		return SYSCALL_RESULT_INVALID;   // buffer too small (NUL included)

	if (!copy_to_user_buffer((void *)arg0, task->cwd, len + 1))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return (uint64_t)len;
}

// chdir's kernel-context half: prove the target EXISTS and IS a directory by
// opening it through the real filesystem, then close it again. Existence
// checking at change time is the entire advantage of kernel-owned cwd over
// a writable environment string — a cwd can never hold garbage.
typedef struct {
	char path[TASK_MAX_PATH_LEN];  // FULL canonical path — this becomes cwd
	vfs_filesystem_t *fs;          // mount-resolved from path (task context)
	const char *fs_tail;           // fs-local remainder, points into path (or
	                               // at a static "/") — transient, never freed
	vfs_directory_t *dir;
	volatile long result;
} chdir_params_t;

static void chdir_do(void *arg)
{
	chdir_params_t *p = (chdir_params_t *)arg;

	if (p->fs->dops == NULL || p->fs->dops->open == NULL)
	{
		p->result = -1;
		return;
	}
	p->result = p->fs->dops->open(&p->dir, p->fs_tail, p->fs);
	if (p->result == 0 && p->dir != NULL)
		p->fs->dops->close(p->dir);   // frees what open allocated
}

// chdir(path) — resolve against the current cwd, canonicalize, validate,
// store. After this returns 0, every relative path in every syscall means
// the new place, and the string getcwd hands back is already clean ("cd
// ../../.." from /bin is stored as "/", not as a growing trail of dots).
static uint64_t syscall_chdir(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->cwd == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw, sizeof(raw)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	chdir_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	if (!resolve_user_path(task, raw, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	// Route to the mounted filesystem (cwd may cross into "/fat", "/ext2"…).
	// The FULL canonical path is what cwd stores; the fs only validates its
	// own tail. A bare mount prefix ("cd /fat") yields tail "/" — the fs root
	// — which always exists, so mount prefixes are always enterable.
	p->fs_tail = NULL;
	p->fs = vfs_resolve_mount(p->path, &p->fs_tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	p->dir = NULL;
	p->result = -1;
	// The existence check walks the directory tree — disk I/O, kernel context.
	call_in_kernel_context(chdir_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "chdir: task %s: '%s' is not a directory\n",
		       task->exename, p->path);
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // no such directory — cwd unchanged
	}

	// Committed: the canonical path becomes the task's "here".
	// (task->cwd is a PAGE_SIZE kmalloc from task_create; TASK_MAX_PATH_LEN
	// fits with room to spare.)
	memcpy(task->cwd, p->path, sizeof(p->path));
	printd(DEBUG_SYSCALL, "chdir: task %s: cwd = '%s'\n", task->exename, task->cwd);
	kfree(p);
	return 0;
}

// stat's kernel-context half: one dops->stat, into the HHDM params block.
typedef struct {
	char path[TASK_MAX_PATH_LEN];  // full canonical path (kept for logging)
	vfs_filesystem_t *fs;          // mount-resolved in task context
	const char *fs_tail;           // fs-local remainder; points into path or
	                               // at a static "/" — transient, never freed
	os64_dirent_t entry;           // out, when result == 0
	volatile long result;
} stat_params_t;

static void stat_do(void *arg)
{
	stat_params_t *p = (stat_params_t *)arg;
	p->result = (p->fs->dops != NULL && p->fs->dops->stat != NULL)
	                ? p->fs->dops->stat(p->fs_tail, &p->entry, p->fs) : -1;
}

// stat(path, entry_out) — fill one os64_dirent_t for the object at `path`,
// file or directory: the answer to "what is this one name?" without opening
// it. Same struct readdir delivers — stat is readdir for exactly one name —
// so callers speak ONE vocabulary for "an entry" (this is 1971's oldest
// surviving syscall idea, minus POSIX's stat-vs-dirent split). Returns 0
// with *entry_out filled, or a SYSCALL_RESULT_* sentinel / negative on
// absence. ls uses this to tell a file argument from a directory argument.
static uint64_t syscall_stat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL)
		return SYSCALL_RESULT_INVALID;

	char raw[TASK_MAX_PATH_LEN];
	if (!copy_user_string((const char *)arg0, raw, sizeof(raw)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	stat_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;

	if (!resolve_user_path(task, raw, p->path, sizeof(p->path)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	p->fs_tail = NULL;
	p->fs = vfs_resolve_mount(p->path, &p->fs_tail);
	if (p->fs == NULL)
	{
		kfree(p);
		return SYSCALL_RESULT_INVALID;   // nothing mounted yet
	}

	p->result = -1;
	// Path resolution walks the directory tree — disk I/O, kernel context.
	call_in_kernel_context(stat_do, p);

	if (p->result != 0)
	{
		printd(DEBUG_SYSCALL, "stat: task %s: '%s' — no such path\n",
		       task->exename, p->path);
		kfree(p);
		return SYSCALL_RESULT_INVALID;
	}

	if (!copy_to_user_buffer((void *)arg1, &p->entry, sizeof(p->entry)))
	{
		kfree(p);
		return SYSCALL_RESULT_BAD_USER_DATA;
	}

	printd(DEBUG_SYSCALL, "stat: task %s: '%s' -> %s, size %lu\n",
	       task->exename, p->path,
	       (p->entry.flags & OS64_DE_DIR) ? "directory" : "file",
	       p->entry.size);
	kfree(p);
	return 0;
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
	bool   background;                    // OS64_SPAWN_BACKGROUND (`&`)
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

	// Marked BEFORE the child is submitted to the scheduler, for the same
	// reason redirection is applied below: it must not get one instruction of
	// CPU in the wrong state. A background job that ran even briefly as a
	// foreground one could steal a keystroke, and that race would be
	// spectacularly hard to see.
	//
	// DEBT (DEBTS.md): the flag is NOT inherited — a background job that
	// spawns its own child mints a FOREGROUND grandchild that can read the
	// keyboard, reopening one generation down exactly the hole this flag
	// closes. Latent while nothing backgrounded can spawn; the day husk runs
	// scripts, this line is where the ruling lands.
	child->backgroundJob = p->background;

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
	const char *user_path = (const char *)arg0;
	char *const *user_argv = (char *const *)arg1;

	// arg5 = flags (OS64_SPAWN_*). Zero is the everyday spawn, so every caller
	// written before this argument existed keeps working untouched — arg5 was
	// previously ignored, and "ignored" and "zero means normal" agree.
	// Unknown bits are REFUSED at the boundary rather than ignored: a flag the
	// kernel silently drops is a request that appeared to succeed and didn't,
	// the same reasoning that rejects unknown open() modes.
	uint64_t flags = arg5;
	if ((flags & ~(uint64_t)OS64_SPAWN_BACKGROUND) != 0)
		return SYSCALL_RESULT_BAD_USER_DATA;

	spawn_params_t *p = kmalloc(sizeof(*p));
	if (p == NULL)
		return SYSCALL_RESULT_INVALID;
	p->background = (flags & OS64_SPAWN_BACKGROUND) != 0;

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

	// Resolve the program path against the caller's cwd, so `cd /bin` makes
	// a bare `hello` launchable. (The child then INHERITS that cwd — the
	// task_create plumbing for that predates these syscalls by a while.)
	{
		char canon[TASK_MAX_PATH_LEN];
		if (!resolve_user_path(p->parent, p->path, canon, sizeof(canon)))
		{
			kfree(p);
			return SYSCALL_RESULT_BAD_USER_DATA;
		}
		memcpy(p->path, canon, sizeof(canon));
	}

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

		// Directory handles don't cross the spawn boundary: a child's 0/1/2
		// are byte streams, and a directory isn't one — and unlike files and
		// pipe ends, vfs_directory_t has no refcount, so sharing one would
		// dangle the child's copy the moment the parent closes. Reject here,
		// where the caller gets a clean error instead of a haunted handle.
		// Net handles are refused for the same refcount reason (udp_conn_t
		// has a single owner by design) — a "hand the child my connection"
		// story arrives with a refcount when a real consumer wants it.
		if (h->type == HANDLE_DIR || h->type == HANDLE_NET_UDP ||
		    h->type == HANDLE_NET_TCP || h->type == HANDLE_NET_ICMP)
		{
			kfree(p);
			return SYSCALL_RESULT_INVALID;
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

// reap(exit_code_out) — collect ONE finished child without ever blocking.
// Returns that child's task ID, or 0 for "nobody has died", or a
// SYSCALL_RESULT_* sentinel.
//
// A separate syscall rather than a flag on wait(), on purpose: a "wait" that
// does not wait is a name that lies, and this kernel has opinions about those.
// The three-way answer is readdir's, not Unix's — 0 means the perfectly
// ordinary "nothing to collect right now", not an error, so a shell can poll
// it at every prompt without treating the common case as a failure.
//
// This is what makes `&` clean. A shell that reports `[1]+ 57 Done` has to
// COLLECT that status, and collecting IS reaping — so the report and the
// cleanup are one act, and background jobs never pile up as zombies the way
// os32's did (its kshell forked, skipped the waitpid, and never spoke of the
// child again). The shell buries its own dead; kworker never has to care.
static uint64_t syscall_reap(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	int *user_code = (int *)arg0;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *parent = cls ? cls->task : NULL;
	if (parent == NULL)
		return SYSCALL_RESULT_INVALID;

	uint64_t exitCode = 0;
	task_t *child = task_reap_any_dead(parent, &exitCode);
	if (child == NULL)
		return 0;   // no finished children — the ordinary answer, not an error

	uint64_t endedPid = child->taskID;
	if (user_code != NULL)
	{
		int code = (int)exitCode;
		if (!copy_to_user_buffer(user_code, &code, sizeof(code)))
			return SYSCALL_RESULT_BAD_USER_DATA;
	}
	return endedPid;
}

// sleep(ms) — park the calling thread for AT LEAST `ms` milliseconds.
//
// The kernel half of this syscall is OLDER THAN THE SYSCALL: the signal
// struct has carried wake-ticks in sigdata[SIGSLEEP] since os32 (2012), and
// processSignals has both halves of the wake — the deadline sweep, and the
// terminate-outranks-the-nap cancel built for Ctrl+C. This handler is just
// the door that lets ring 3 ask for what the kernel already knew how to do.
//
// Units: the ABI speaks TIME (ms); the kernel speaks ticks. The conversion
// lives here, at the boundary, rounding UP against the ACTIVE rate — so a
// kernel rebuilt at 1ms ticks silently improves every existing binary's
// sleep(1) from 10ms to 1ms, and no tick constant ever calcifies into the
// ABI. "At least ms" is the only honest promise: park latency, another
// task's slice, and the signal sweep all land on top of the deadline.
//
// sleep(0) is the documented free yield — not folklore, not an accident.
//
// Interruption: a pending SIGINT/SIGKILL wakes the sleeper (processSignals'
// nap-cancel) and the loop-top check below dies on the same rail as an
// interrupted console read. Returns 0 always — the only interruption that
// exists today is death, and the dead read no return values. Remaining-time
// semantics deliberately wait for the SIGNALS.md EINTR-vs-restart ruling.
static uint64_t syscall_sleep(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	uint64_t ms = arg0;

	core_local_storage_t *cls = get_core_local_storage();
	thread_t *self = cls ? cls->currentThread : NULL;
	if (self == NULL)
		return SYSCALL_RESULT_INVALID;

	if (ms == 0)
	{
		// No time requested: the CPU goes back, nothing is parked. Same
		// trigger as syscall_yield — two honest doors onto one scheduler.
		scheduler_trigger(NULL);
		return 0;
	}

	// ms → ticks at the active rate, rounding UP; ms >= 1 guarantees at
	// least one tick without a separate floor. The clamp keeps absurd
	// requests from overflowing the multiply — capped, that's roughly
	// 2.9 billion years at 100 ticks/second, which is close enough to
	// "forever" that nobody will file a bug about the early wake.
	uint64_t ticks;
	if (ms > (UINT64_MAX - 999) / TICKS_PER_SECOND)
		ticks = UINT64_MAX / 2;
	else
		ticks = (ms * TICKS_PER_SECOND + 999) / 1000;

	uint64_t wakeTick = kTicksSinceStart + ticks;

	// Level-triggered like every park in this kernel: check, sleep, re-check.
	// processSignals wakes us for exactly two reasons — the deadline arrived,
	// or a terminate is pending — and the loop top distinguishes them.
	for (;;)
	{
		if (self->signals.sigind & SIGNALS_TERMINATING)
		{
			// Ctrl+C (or a ctl write) landed while we napped. Same rail as
			// an interrupted console read: die here, in our own context.
			raise_terminating_signal_and_die(cls->task, self);
			__builtin_unreachable();
		}

		if (kTicksSinceStart >= wakeTick)
			return 0;

		// Park with the wake deadline in sigdata[SIGSLEEP] — sigaction
		// triggers the scheduler itself, so we genuinely leave the CPU here
		// and resume on the next line when woken.
		sigaction(SIGSLEEP, NULL, wakeTick, self);
	}
}

// ticks(out) — hand ring 3 the monotonic clock: tick count since boot plus
// the ACTIVE tick rate, in one call (os64_ticks_t, abi os64/ticks.h — the
// stopwatch/calendar doctrine lives there). One call because the two numbers
// are only useful together: a count without its rate is a number wearing no
// units. First consumers: top's CPU%, sleep's help printing the live
// scheduler interval, uptime for whoever asks.
static uint64_t syscall_ticks(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	os64_ticks_t *user_out = (os64_ticks_t *)arg0;
	if (user_out == NULL)
		return SYSCALL_RESULT_BAD_USER_DATA;   // the whole point is the struct

	os64_ticks_t t;
	t.ticks = kTicksSinceStart;
	t.per_second = TICKS_PER_SECOND;

	if (!copy_to_user_buffer(user_out, &t, sizeof(t)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return 0;
}

// memory(out) — the physical memory picture, one atomic snapshot. free/used/
// largest come from a single walk of the allocator ledger under its lock
// (allocator_memory_snapshot), so they agree with each other; total/usable
// are boot-time memmap constants. `available` is summed HERE, in the kernel,
// so userland never reinvents Linux's wrong-column arithmetic — and
// `reclaimable` is the future page cache's seat at the table, honestly zero
// until one exists. free + used == usable is a LIVE INVARIANT (see
// os64/memory.h for the audit doctrine); the ring-3 fixture asserts it.
static uint64_t syscall_memory(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	os64_memory_t *user_out = (os64_memory_t *)arg0;
	if (user_out == NULL)
		return SYSCALL_RESULT_BAD_USER_DATA;   // the whole point is the struct

	os64_memory_t m;
	m.total  = kTotalMemory;
	m.usable = kAvailableMemory;
	allocator_memory_snapshot(&m.free, &m.used, &m.largest_free_extent);
	m.reclaimable = 0;                    // no page cache yet — the truth
	m.available   = m.free + m.reclaimable;
	m.page_size   = PAGE_SIZE;

	if (!copy_to_user_buffer(user_out, &m, sizeof(m)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return 0;
}

// printat(x, y, str) — the widget-plane syscall: park a string at an absolute
// character cell on the physical console. See the abi header for the doctrine
// (widget != console write; lives outside the future VT stack). The handler
// is a bounds check, a string copy, and a handoff to print_at(), which brings
// its own guarantees: no cursor motion, no wrap/scroll, clips at the screen
// edge under the renderer lock, and politely declines while the GUI owns the
// framebuffer.
static uint64_t syscall_printat(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg3; (void)arg4; (void)arg5;

	// One row of a widget, tops. print_at clips at the screen edge anyway, so
	// a bigger buffer would buy nothing but copy time; a "widget" longer than
	// this is console content that took a wrong turn at the API.
	char kernel_buffer[256];

	// Cap the CELL coordinates before print_at multiplies them into pixels:
	// its edge-clip compares px against the framebuffer width AFTER the
	// multiply, so an absurd x could wrap the 32-bit pixel math back onto the
	// screen. No display has 4096 columns; nothing legitimate is lost.
	if (arg0 > 4095 || arg1 > 4095)
		return SYSCALL_RESULT_INVALID;

	if (!copy_user_string((const char *)arg2, kernel_buffer, sizeof(kernel_buffer)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	print_at(&kRenderer, (unsigned int)arg0, (unsigned int)arg1, kernel_buffer);
	return 0;
}

// time(out) — the wall clock's raw truth (see os64/time.h for the doctrine:
// the kernel keeps a counter, the library keeps the calendar). One consistent
// snapshot: epoch and the sub-second phase are advanced by the same timer IRQ,
// so we read epoch / phase / epoch-again and retry if the second rolled in
// between — otherwise a caller could see 12:00:00 paired with the last tick
// of 11:59:59. The loop runs at most twice a second per caller in practice;
// it exists for the once-a-day time a syscall straddles the boundary.
static uint64_t syscall_time(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

	os64_time_t *user_out = (os64_time_t *)arg0;
	if (user_out == NULL)
		return SYSCALL_RESULT_BAD_USER_DATA;   // the whole point is the struct

	uint64_t epoch, phase;
	do {
		epoch = kSystemCurrentTime;
		phase = irq0_current_count;
	} while (epoch != kSystemCurrentTime);

	os64_time_t t;
	t.epoch             = (int64_t)epoch;
	t.tz_offset_minutes = kTimeZone * 60;     // kernel config is whole hours
	t.ticks_into_second = (uint32_t)phase;
	t.ticks_per_second  = (uint32_t)kTicksPerSecond;
	t.reserved          = 0;

	if (!copy_to_user_buffer(user_out, &t, sizeof(t)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	return 0;
}

// setenv(key, value|NULL) — mutate the calling task's environment (the abi
// header has the doctrine: same physical page the task reads, children
// snapshot at spawn). arg1 deliberately isn't in the dispatcher's pointer
// mask — NULL is a legal value meaning "unset", so the range check happens
// here, inside copy_user_string, only when a value is actually present.
static uint64_t syscall_setenv(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	(void)arg2; (void)arg3; (void)arg4; (void)arg5;

	core_local_storage_t *cls = get_core_local_storage();
	task_t *task = cls ? cls->task : NULL;
	if (task == NULL || task->env == NULL)
		return SYSCALL_RESULT_INVALID;

	// A key is a NAME, not a novel; a value has to fit the block anyway
	// (one page minus header), so these bounds reject only the absurd.
	char key[128];
	char val[2048];

	if (!copy_user_string((const char *)arg0, key, sizeof(key)))
		return SYSCALL_RESULT_BAD_USER_DATA;
	if (key[0] == '\0')
		return SYSCALL_RESULT_INVALID;      // the empty key names nothing

	if (arg1 == 0)
		return env_unset(task->env, key) ? 0 : SYSCALL_RESULT_INVALID;

	if (!copy_user_string((const char *)arg1, val, sizeof(val)))
		return SYSCALL_RESULT_BAD_USER_DATA;

	// env_set fails only when the block is full — surface that honestly.
	return env_set(task->env, key, val) ? 0 : SYSCALL_RESULT_INVALID;
}
