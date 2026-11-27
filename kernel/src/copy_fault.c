// copy_fault.c — which ring-0 page faults a user copy recovers from
// (copy_fault.h; USER_COPY.md). Pure: tools/test_copy_fault_host.sh.

#include "copy_fault.h"

uint64_t copy_fault_resume(uint64_t rip, uint64_t cr2, uint64_t error_code,
                           uint64_t page_size, uint64_t user_ceiling,
                           const copy_resume_t *table, uint64_t count)
{
    if (error_code & (COPY_FAULT_USER | COPY_FAULT_RESERVED))
        return 0;
    if (cr2 < page_size || cr2 >= user_ceiling)
        return 0;
    for (uint64_t i = 0; i < count; i++)
        if (table[i].at == rip)
            return table[i].resume;
    return 0;
}
