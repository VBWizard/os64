#ifndef COPY_FAULT_H
#define COPY_FAULT_H

// Which ring-0 page faults a user copy recovers from (USER_COPY.md § Which
// faults recover). Pure: no kernel headers, so tools/test_copy_fault_host.sh
// drives it on the host beside a model.

#include <stdint.h>

// Which register holds a move's USER pointer at a fault: RSI when the copy
// reads user memory, RDI when it writes it.
#define COPY_USER_IN_RSI 0u
#define COPY_USER_IN_RDI 1u

// One instruction that touches user memory by its user address, the address
// a fault there resumes at, which register holds its user pointer, and its
// step in bytes (user_copy.S's resume table; 24 bytes an entry).
typedef struct {
    uint64_t at;
    uint64_t resume;
    uint32_t user_reg;
    uint32_t step;
} copy_resume_t;

// The page-fault error code's bits this asks about.
#define COPY_FAULT_USER     (1u << 2)   // the access came from ring 3
#define COPY_FAULT_RESERVED (1u << 3)   // a reserved bit was set in an entry

// Where a fault resumes: the table's resume address for `rip`, or 0 to
// panic as before. Nonzero only for a ring-0 fault with no reserved bit, at
// a table instruction, on an address at or above page_size and below
// user_ceiling, and within one step of that instruction's user pointer
// (`rsi` or `rdi` at the fault). The architecture reports a string move's
// fault at the element that faulted, with RSI and RDI pointing at it, so
// the last test is what tells the user operand's fault (a page a sibling
// took away) from the kernel operand's: a kernel buffer is a local on a
// kernel stack, which lives in the user range too, and a copy that ran off
// it into its guard is a kernel bug that stays a panic. So does a fault on
// the kernel's half or page zero. The handler asks only on a fault it
// would otherwise panic on; one it resolves is resolved.
uint64_t copy_fault_resume(uint64_t rip, uint64_t cr2, uint64_t error_code,
                           uint64_t rsi, uint64_t rdi,
                           uint64_t page_size, uint64_t user_ceiling,
                           const copy_resume_t *table, uint64_t count);

#endif
