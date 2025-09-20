#include <stddef.h>

#include "panic.h"
#include "BasicRenderer.h"
#include "serial_logging.h"
#include "smp_core.h"
#include "task.h"
#include "CONFIG.h"
#include "log.h"
#include "memory/paging.h"

uint64_t gLastFaultRbp = 0;
uint64_t gLastFaultRsp = 0;
uint64_t gLastFaultErrorCode = 0;

static bool is_canonical_address(uint64_t address)
{
	uint64_t upper = address >> 47;
	return upper == 0 || upper == 0x1FFFF;
}

static bool address_is_mapped(uint64_t address)
{
	uintptr_t pte = paging_walk_paging_table_keep_flags((pt_entry_t*)kKernelPML4v, address, true);
	if (pte == 0xbadbadba) {
		return false;
	}
	return (pte & PAGE_PRESENT) != 0;
}

//NOTE: Won't work with userland RIPs. Will need to modify to accept the CR3 for non-kernel processes once we have a userland
void dump_stack_trace(uint64_t rip)
{
	printf("Stack trace (most recent call first):\n");
	printf("  [0] RIP=0x%016lx\n", rip);
	printf("  Captured RSP=0x%016lx RBP=0x%016lx\n", gLastFaultRsp, gLastFaultRbp);

	if (gLastFaultErrorCode & (1ull << 2)) {
		printf("  <fault originated from user mode>\n");
	}

	uint64_t rbp = gLastFaultRbp;
	if (rbp == 0) {
		printf("  <no frame pointer captured>\n");
		return;
	}

	if (!is_canonical_address(rbp)) {
		printf("  <frame pointer 0x%016lx non-canonical>\n", rbp);
		return;
	}

	if (!address_is_mapped(rbp) || !address_is_mapped(rbp + sizeof(uint64_t))) {
		printf("  <frame pointer 0x%016lx unmapped>\n", rbp);
		return;
	}

	const uint32_t max_frames = 16;
	for (uint32_t frame = 1; frame < max_frames; frame++) {
		uint64_t *frame_ptr = (uint64_t*)rbp;
		uint64_t next_rbp = frame_ptr[0];
		uint64_t return_address = frame_ptr[1];

		if (!is_canonical_address(return_address)) {
			printf("  [%u] <non-canonical return address 0x%016lx>\n", frame, return_address);
			break;
		}

		printf("  [%u] RIP=0x%016lx\n", frame, return_address);

		if (next_rbp == 0) {
			break;
		}
		if (next_rbp <= rbp) {
			printf("  <next frame pointer 0x%016lx not higher than current 0x%016lx>\n", next_rbp, rbp);
			break;
		}
		if (!is_canonical_address(next_rbp)) {
			printf("  <next frame pointer 0x%016lx non-canonical>\n", next_rbp);
			break;
		}
		if (!address_is_mapped(next_rbp) || !address_is_mapped(next_rbp + sizeof(uint64_t))) {
			printf("  <next frame pointer 0x%016lx unmapped>\n", next_rbp);
			break;
		}

		rbp = next_rbp;
	}
}

void exception_panic(const char* message, uint64_t rip, uint64_t error_code) {
    core_local_storage_t* core = get_core_local_storage();

    printf("\n>>> EXCEPTION PANIC: %s <<<                      \n", message);  // 🛠 FIXED: Actually print the message!
    printf(">>> AP %lu (Thread %lu) <<<                        \n", core->apic_id, core->threadID);
    printf(">>> Faulting instruction: 0x%016lx <<<             \n", rip);
    
    if (error_code != 0xFFFFFFFFFFFFFFFF) {
        printf(">>> Error Code: 0x%lx <<<                          \n", error_code);
    }
    if (core->currentThread) {
		task_t *task = (task_t*)core->currentThread->ownerTask;

        printf(">>> Excepting Task: %s <<<                         \n", task->path);
    } else {
        printf(">>> No current task (core likely idle) <<<         \n");
    }

	// **Log it only if logging is initialized**
	if (kLoggingInitialized) {
		printd(DEBUG_EXCEPTIONS, "EXCEPTION: %s (AP %lu, Thread %lu, RIP: 0x%016lx, Error Code: 0x%lx)\n",
				message, core->apic_id, core->threadID, rip, error_code);
	}

	while (1) { __asm__ volatile ("cli\nhlt\n"); }
}

void handle_divide_by_zero(uint64_t rip) {
    exception_panic("Divide by zero (#DE) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_invalid_opcode(uint64_t rip) {
    exception_panic("Invalid opcode (#UD) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_double_fault(uint64_t rip) {
    exception_panic("Double fault (#DF) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}

void handle_general_protection_fault(uint64_t error_code, uint64_t rip) {
    exception_panic("General Protection Fault (#GP) occurred!", rip, error_code);
}


static void log_page_fault_bits(uint64_t error_code)
{
	const struct {
		uint64_t mask;
		const char *label;
	} bit_info[] = {
		{1ull << 0, "Present (bit 0)"},
		{1ull << 1, "Write (bit 1)"},
		{1ull << 2, "User (bit 2)"},
		{1ull << 3, "Reserved bit violation (bit 3)"},
		{1ull << 4, "Instruction fetch (bit 4)"},
	};

	bool any = false;
	for (size_t i = 0; i < sizeof(bit_info) / sizeof(bit_info[0]); i++) {
		if (error_code & bit_info[i].mask) {
			printf("  %s\n", bit_info[i].label);
			any = true;
		}
	}

	if (!any) {
		printf("  No recognized fault bits set.\n");
	}
}

void handle_page_fault(uint64_t cr2, uint64_t error_code, uint64_t rip)
{
	gLastFaultErrorCode = error_code;

	printf("\nPAGE FAULT at RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%016lx\n", rip, cr2, error_code);
	if (kLoggingInitialized) {
		printd(DEBUG_EXCEPTIONS, "PAGE FAULT at RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%016lx\n", rip, cr2, error_code);
	}

	log_page_fault_bits(error_code);

	dump_stack_trace(rip);

	printf("Unrecoverable page fault. Panicking.\n");
	panic("PAGE FAULT at RIP=0x%016lx, CR2=0x%016lx, ERROR=0x%016lx\n", rip, cr2, error_code);
}

void handle_machine_check(uint64_t rip) {
    exception_panic("Machine Check (#MC) occurred!", rip, 0xFFFFFFFFFFFFFFFF);
}
