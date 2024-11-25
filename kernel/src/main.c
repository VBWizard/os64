#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "kernel.h"
#include "limine_os64.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_hhdm_request hhmd_request;
extern volatile struct limine_module_request module_request;
extern volatile struct limine_smp_request smp_request;
extern uint64_t kHHDMOffset;

struct limine_memmap_response *memmap_response;
struct limine_hhdm_response *hhmd_response;
struct limine_framebuffer_response *framebuffer_response;
struct limine_module_response *limine_module_response;
extern struct limine_smp_response *kLimineSMPInfo;
struct limine_framebuffer *framebuffer;

char kernel_stack[0x1000*64] __attribute__((aligned(16)));


__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(3);

uint8_t* fb_ptr = NULL;

// Halt and catch fire function.
static void hcf(int error_number) {
    int error = error_number;
	for (;;) {
		asm ("sti\nhlt\n");
    }
}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void limine_boot_entry_point(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    
		__asm__ __volatile__ (
		"lea rsp, [%0 + %1 - 0x100]"
		:
		: "r"(kernel_stack), "i"(0x1000*64)
	);

	if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf(-5);
    }

	memmap_response = memmap_request.response;
	hhmd_response = hhmd_request.response;
	framebuffer_response = framebuffer_request.response;
	limine_module_response = module_request.response;
	kLimineSMPInfo = smp_request.response;

	int limine_response_status = verify_limine_responses(memmap_response, hhmd_response, framebuffer_response, limine_module_response, kLimineSMPInfo);

	if (limine_response_status != 0)
		hcf(limine_response_status);
	
	kHHDMOffset = hhmd_response->offset;
	kernel_main();
}
