#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdbool.h>
#include <stdint.h>

extern bool kTestingPageFaults;
extern uint64_t kTestingPageFaultResumeRip;
extern volatile uint64_t kPageFaultCount;
void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip);
// A non-#PF exception raised at CPL 3: report it and end the task. Returns
// only when there is no user task on this core to end (the caller then
// treats the exception as fatal).
#include "exception_report.h"   // exception_context_t
void user_exception_kill(exception_context_t *ctx);

#endif // EXCEPTIONS_H
