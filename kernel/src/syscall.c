#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

#include "BasicRenderer.h"
#include "printd.h"
#include "scheduler.h"
#include "smp_core.h"
#include "memory/memcpy.h"
#include "memory/paging.h"
#include "log.h"

#define SYSCALL_RESULT_INVALID UINT64_C(0xFFFFFFFFFFFFFFFF)
#define SYSCALL_RESULT_BAD_USER_DATA UINT64_C(0xFFFFFFFFFFFFFFFE)

static uint64_t g_saved_cr3[MAX_CPUS];
static bool g_saved_cr3_valid[MAX_CPUS];

static inline uint32_t get_current_cpu_index(void);
static bool prepare_syscall_args(const syscall_entry_t *entry, const uint64_t incoming[6], uint64_t prepared[6]);
static bool copy_user_string(const char *user_str, char *buffer, size_t buffer_len);

static uint64_t syscall_yield(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);
static uint64_t syscall_debug_log(uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5);

syscall_entry_t syscall_table[MAX_SYSCALLS] = {
	SYSCALL_DEFINE(0, "yield", syscall_yield, false, false),
	SYSCALL_DEFINE(1, "debug_log", syscall_debug_log, true, true),
};

uint64_t _syscall(void)
{
	register uint64_t syscall_number __asm__("rax");
	register uint64_t arg0 __asm__("rdi");
	register uint64_t arg1 __asm__("rsi");
	register uint64_t arg2 __asm__("rdx");
	register uint64_t arg3 __asm__("r10");
	register uint64_t arg4 __asm__("r8");
	register uint64_t arg5 __asm__("r9");

	return _syscall_dispatch(syscall_number,
		arg0, arg1, arg2,
		arg3, arg4, arg5);
}

uint64_t _syscall_dispatch(
	uint64_t syscall_number,
	uint64_t arg0, uint64_t arg1, uint64_t arg2,
	uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
	const uint64_t raw_args[6] = { arg0, arg1, arg2, arg3, arg4, arg5 };
	uint64_t prepared_args[6] = {0};
	bool switched_cr3 = false;

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

	// Reject addresses that clearly point into kernel-mapped memory
	if (user_address >= kHHDMOffset)
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

static bool prepare_syscall_args(const syscall_entry_t *entry, const uint64_t incoming[6], uint64_t prepared[6])
{
	memcpy(prepared, incoming, sizeof(uint64_t) * 6);

	if (!entry->needs_user_copy)
	{
		return true;
	}

	for (size_t i = 0; i < 6; ++i)
	{
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

static bool copy_user_string(const char *user_str, char *buffer, size_t buffer_len)
{
	if (!user_str || !buffer || buffer_len == 0)
	{
		return false;
	}

	size_t written = 0;
	while (written < buffer_len - 1)
	{
		char ch = 0;
		if (!validate_and_copy_user_data(user_str + written, sizeof(char), &ch))
		{
			return false;
		}

		buffer[written++] = ch;
		if (ch == '\0')
		{
			return true;
		}
	}

	buffer[buffer_len - 1] = '\0';
	return false;
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

	scheduler_yield(NULL);
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
