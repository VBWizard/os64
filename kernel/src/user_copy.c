// user_copy.c — a user copy's fault resumed at its failure exit, and the
// count of those (user_copy.h; USER_COPY.md). The copies themselves are
// user_copy.S; the rule for which faults recover is copy_fault.c.

#include "user_copy.h"

#include "task.h"
#include "CONFIG.h"
#include "memory/paging.h"   // USER_CANONICAL_MAX
#include "serial_logging.h"

// user_copy.S lays the table out by hand.
_Static_assert(sizeof(copy_resume_t) == 24, "user_copy.S writes 24-byte resume entries");

// Machine-wide, written only by the page-fault handler and read by
// /sys/memory/usercopy, each with relaxed atomics: a count, not a lock.
static user_copy_counts_t s_counts;

bool user_copy_recover(exception_context_t *ctx, struct task *task, uint64_t cr2,
                       uint64_t error_code, uint64_t rip)
{
    if (ctx == NULL || task == NULL)
        return false;
    uint64_t resume = copy_fault_resume(rip, cr2, error_code, ctx->rsi, ctx->rdi,
                                        PAGE_SIZE, USER_CANONICAL_MAX,
                                        kCopyResumeTable,
                                        (uint64_t)(kCopyResumeTableEnd - kCopyResumeTable));
    if (resume == 0)
        return false;
    ctx->rip = resume;
    __atomic_add_fetch(&task->copyRecoveries, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_counts.total, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_counts.since_read, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s_counts.last_task, task->taskID, __ATOMIC_RELAXED);
    __atomic_store_n(&s_counts.last_cr2, cr2, __ATOMIC_RELAXED);
    printd(DEBUG_DEMAND_PAGING, "user copy: %s (task %lu) faulted at 0x%016lx after its check; the call answers "
           "not copied\n", task->exename, task->taskID, cr2);
    return true;
}

void user_copy_counts_read(user_copy_counts_t *out)
{
    out->total = __atomic_load_n(&s_counts.total, __ATOMIC_RELAXED);
    out->since_read = __atomic_exchange_n(&s_counts.since_read, 0, __ATOMIC_RELAXED);
    out->last_task = __atomic_load_n(&s_counts.last_task, __ATOMIC_RELAXED);
    out->last_cr2 = __atomic_load_n(&s_counts.last_cr2, __ATOMIC_RELAXED);
}
