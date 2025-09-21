#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Type definition for a generic syscall function (6 args max)
typedef uint64_t (*syscall_func_t)(
    uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5
);

// Metadata for each syscall entry
typedef struct {
    syscall_func_t func;
    const char* name;
    bool needs_cr3_switch;
    bool needs_user_copy;
    bool trace_enabled;
} syscall_entry_t;

#define MAX_SYSCALLS 256

// The actual syscall table
extern syscall_entry_t syscall_table[MAX_SYSCALLS];

// Dispatcher called from assembly
uint64_t _syscall_dispatch(
    uint64_t syscall_number,
    uint64_t arg0, uint64_t arg1, uint64_t arg2,
    uint64_t arg3, uint64_t arg4, uint64_t arg5
);

// A macro to help define syscalls in syscall_table[]
#define SYSCALL_DEFINE(NUM, NAME, FN, CR3, COPY) \
    [NUM] = { .func = FN, .name = NAME, .needs_cr3_switch = CR3, .needs_user_copy = COPY, .trace_enabled = false }

// Optional: variant with trace flag
#define SYSCALL_DEFINE_EX(NUM, NAME, FN, CR3, COPY, TRACE) \
    [NUM] = { .func = FN, .name = NAME, .needs_cr3_switch = CR3, .needs_user_copy = COPY, .trace_enabled = TRACE }

void switch_to_kernel_cr3(void);
void restore_user_cr3(void);
bool validate_and_copy_user_data(const void* user_ptr, size_t length, void* kernel_buffer);
void log_syscall_invocation(const syscall_entry_t* entry, const uint64_t args[6]);

#endif
