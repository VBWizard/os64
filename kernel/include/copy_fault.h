#ifndef COPY_FAULT_H
#define COPY_FAULT_H

// Which ring-0 page faults a user copy recovers from (USER_COPY.md § Which
// faults recover). Pure: no kernel headers, so tools/test_copy_fault_host.sh
// drives it on the host beside a model.

#include <stdint.h>

// One instruction that touches user memory by its user address, and the
// address a fault there resumes at (user_copy.S's resume table).
typedef struct {
    uint64_t at;
    uint64_t resume;
} copy_resume_t;

// The page-fault error code's bits this asks about.
#define COPY_FAULT_USER     (1u << 2)   // the access came from ring 3
#define COPY_FAULT_RESERVED (1u << 3)   // a reserved bit was set in an entry

// Where a fault resumes: the table's resume address for `rip`, or 0 to
// panic as before. Nonzero only for a ring-0 fault, at a table instruction,
// on an address at or above page_size and below user_ceiling (one the
// check could have passed), with no reserved bit: a fault at a table
// instruction on the kernel's half or on page zero is a copy that was
// never checked, a kernel bug, and stays a panic. The handler asks only on
// a fault it would otherwise panic on; one it resolves is resolved.
uint64_t copy_fault_resume(uint64_t rip, uint64_t cr2, uint64_t error_code,
                           uint64_t page_size, uint64_t user_ceiling,
                           const copy_resume_t *table, uint64_t count);

#endif
