#ifndef USER_COPY_H
#define USER_COPY_H

// The copies that touch user memory by its user address, and the only ones
// that may (USER_COPY.md). Each answers 0 when every byte was copied, else
// the bytes not copied: a page a sibling took away after the check
// (user_range_accessible) faults inside the copy, and the fault resumes at
// the copy's failure exit instead of panicking (copy_fault.h). The check
// stays the gate: give these only a range it has just passed.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "copy_fault.h"
#include "exception_report.h"   // exception_context_t

struct task;

size_t user_copy_in(void *kernel_dst, const void *user_src, size_t n);
size_t user_copy_out(void *user_dst, const void *kernel_src, size_t n);

// user_copy.S's resume table, read by the page-fault handler.
extern const copy_resume_t kCopyResumeTable[];
extern const uint64_t kCopyResumeCount;

// Recoveries taken since boot, machine-wide (/sys/memory/usercopy), with the
// last one's task and faulting address; and since the file was last read
// (the read starts that count again). A task's own count is
// task->copyRecoveries (/proc status).
typedef struct {
    uint64_t total, since_read;
    uint64_t last_task, last_cr2;
} user_copy_counts_t;
void user_copy_counts_read(user_copy_counts_t *out);

// Called by the page-fault handler on a ring-0 fault it would otherwise
// panic on: true, with the fault's own frame `ctx` resuming at the copy's
// failure exit and the recovery counted, when copy_fault_resume says so.
// False with a NULL frame (the old exception path), and for any fault not
// a user copy's.
bool user_copy_recover(exception_context_t *ctx, struct task *task, uint64_t cr2,
                       uint64_t error_code, uint64_t rip);

#endif
