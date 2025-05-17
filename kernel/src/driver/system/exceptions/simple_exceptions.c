#include "panic.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "smp_core.h"
#include "task.h"
#include "CONFIG.h"
#include "log.h"
#include "sprintf.h"

void exception_panic(const char* message, uint64_t rip, uint64_t error_code) {
    core_local_storage_t* core = get_core_local_storage();

    printf("\n>>> EXCEPTION PANIC: %s <<<\n", message);  // 🛠 FIXED: Actually print the message!
    printf(">>> AP %lu (Thread %lu) <<<\n", core->apic_id, core->threadID);
    printf(">>> Faulting instruction: 0x%016lx <<<\n", rip);
    
    if (error_code != 0xFFFFFFFFFFFFFFFF) {
        printf(">>> Error Code: 0x%lx <<<\n", error_code);
    }
    if (core->currentThread) {
		task_t *task = (task_t*)core->currentThread->ownerTask;

        printf(">>> Excepting Task: %s <<<\n", task->path);
    } else {
        printf(">>> No current task (core likely idle) <<<\n");
    }

	// **Log it only if logging is initialized**
	if (kLoggingInitialized) {
		printd(DEBUG_EXCEPTIONS, "EXCEPTION: %s (AP %lu, Thread %lu, RIP: 0x%016lx, Error Code: 0x%lx)\n",
				message, core->apic_id, core->threadID, rip, error_code);
	}

	while (1) { __asm__ volatile ("cli\nhlt\n"); }
}

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
	uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));  // Read CR2
	
	char message[128];
    sprintf(message, "Page Fault (#PF) occurred for address 0x%016lx", cr2);

    exception_panic(message, rip, error_code);
}

void handle_machine_check(uint64_t rip) {
    exception_panic("Machine Check (#MC) occurred!", 0xFFFFFFFFFFFFFFFF, rip);
}
