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
    // DANGER: leave this false unless the handler switches STACKS too.  The
    // dispatcher runs on the thread's syscall kernel stack — a task-local VA
    // that kKernelPML4 does not map — so flipping CR3 under a C handler
    // triple-faults on the next stack access.  The user CR3 already maps the
    // full kernel upper half; see the syscall_table comment in syscall.c.
    bool needs_cr3_switch;
    // Bit i set = arg i is a USER-SPACE POINTER the dispatcher should
    // range-check before the handler runs.  A mask (not a blanket "check all
    // six") because unused argument registers carry ring-3 garbage — and
    // right after a previous syscall that garbage is kernel addresses our own
    // clobber convention left behind, which a blanket >= kHHDMOffset scan
    // "helpfully" rejects.  (write() was the first casualty: R10 held a
    // leftover kernel pointer from yield(), so a perfectly valid write got
    // SYSCALL_RESULT_BAD_USER_DATA without the handler ever running.)
    uint8_t user_ptr_arg_mask;
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
// PTRMASK: bitmask of which args are user pointers (see user_ptr_arg_mask).
#define SYSCALL_DEFINE(NUM, NAME, FN, CR3, PTRMASK) \
    [NUM] = { .func = FN, .name = NAME, .needs_cr3_switch = CR3, .user_ptr_arg_mask = PTRMASK, .trace_enabled = false }

// Optional: variant with trace flag
#define SYSCALL_DEFINE_EX(NUM, NAME, FN, CR3, PTRMASK, TRACE) \
    [NUM] = { .func = FN, .name = NAME, .needs_cr3_switch = CR3, .user_ptr_arg_mask = PTRMASK, .trace_enabled = TRACE }

void switch_to_kernel_cr3(void);
void restore_user_cr3(void);
bool validate_and_copy_user_data(const void* user_ptr, size_t length, void* kernel_buffer);
void log_syscall_invocation(const syscall_entry_t* entry, const uint64_t args[6]);

#endif
