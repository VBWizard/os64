#include "panic.h"
#include "BasicRenderer.h"

void exception_panic(const char* message, uint64_t rip, uint64_t error_code) {
    printf("\n>>> EXCEPTION PANIC: %s <<<\n", message);
    printf(">>> Faulting instruction: 0x%016lx <<<\n", rip);
    if (error_code != 0xFFFFFFFFFFFFFFFF) {  // Use -1 (all 1s) for "no error code"
        printf(">>> Error Code: 0x%lx <<<\n", error_code);
    }
    while (1) { __asm__ volatile ("hlt"); }
}

#include "panic.h"
#include "log.h"

void handle_divide_by_zero(uint64_t rip) {
    exception_panic("Divide by zero (#DE) occurred!", 0xFFFFFFFFFFFFFFFF, rip);
}

void handle_invalid_opcode(uint64_t rip) {
    exception_panic("Invalid opcode (#UD) occurred!", 0xFFFFFFFFFFFFFFFF, rip);
}

void handle_double_fault(uint64_t rip) {
    exception_panic("Double fault (#DF) occurred!", 0xFFFFFFFFFFFFFFFF, rip);
}

void handle_general_protection_fault(uint64_t error_code, uint64_t rip) {
    exception_panic("General Protection Fault (#GP) occurred!", rip, error_code);
}


void handle_page_fault(uint64_t error_code, uint64_t rip) {
    exception_panic("Page Fault (#PF) occurred!", rip, error_code);
}

void handle_machine_check(uint64_t rip) {
    exception_panic("Machine Check (#MC) occurred!", 0xFFFFFFFFFFFFFFFF, rip);
}
