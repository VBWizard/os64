#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdbool.h>
#include <stdint.h>

extern bool kTestingPageFaults;
extern uint64_t kTestingPageFaultResumeRip;
extern volatile uint64_t kPageFaultCount;
#include "exception_report.h"   // exception_context_t
// `ctx` is the fault's own frame, on the faulting thread's kernel stack, so
// it is still this fault's after a resolve that parks and resumes on
// another CPU, where a per-CPU slot would name whatever that CPU was last
// doing. A caught SIGSEGV and a recovered user copy edit it, and a fatal
// fault's report is read from it. NULL from the old exception path
// (EXCOLD), where neither edit happens and a fatal fault is reported from
// the old stub's globals, as it always was.
void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip, exception_context_t *ctx);
// A non-#PF exception raised at CPL 3: report it and end the task. Returns
// only when there is no user task on this core to end (the caller then
// treats the exception as fatal).
void user_exception_kill(exception_context_t *ctx);

#endif // EXCEPTIONS_H
