#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdbool.h>
#include <stdint.h>

extern bool kTestingPageFaults;
extern uint64_t kTestingPageFaultResumeRip;
extern volatile uint64_t kPageFaultCount;
void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip);
void handle_fpu_exception(uint64_t vector, uint64_t rip, uint64_t cs);

#endif // EXCEPTIONS_H
