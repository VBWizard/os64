#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdbool.h>
#include <stdint.h>

extern bool kTestingPageFaults;
extern uint64_t kTestingPageFaultResumeRip;
extern uint64_t kPageFaultCount;
void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip);

#endif // EXCEPTIONS_H
