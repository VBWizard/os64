#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "paging.h"
#include "allocator.h"
#include "video.h"
#include "memmap.h"
#include "kmalloc.h"
#include "sprintf.h"
#include "io.h"
#include "serial_logging.h"
#include "limine_os64.h"
#include "init.h"

void kernel_main();

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_hhdm_request hhmd_request;
extern volatile struct limine_module_request module_request;

struct limine_memmap_response *memmap_response;
struct limine_hhdm_response *hhmd_response;
struct limine_framebuffer_response *framebuffer_response;
struct limine_module_response *module_response;
struct limine_framebuffer *framebuffer;

__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(3);


uint8_t* fb_ptr = NULL;

// Halt and catch fire function.
static void hcf(int error_number) {
    int error = error_number;
	for (;;) {
        asm ("hlt");
    }
}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
void limine_boot_entry_point(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf(-5);
    }

	memmap_response = memmap_request.response;
	hhmd_response = hhmd_request.response;
	framebuffer_response = framebuffer_request.response;
	module_response = module_request.response;

	int limine_response_status = verify_limine_responses(memmap_response, hhmd_response, framebuffer_response, module_response);

	if (limine_response_status != 0)
		hcf(limine_response_status);
	
	kernel_main();
}

void kernel_main()
{
		framebuffer = framebuffer_request.response->framebuffers[0];

	printd(DEBUG_BOOT, "***** OS64 - system initializing! *****\n");

	boot_init();
	init_serial(0x3f8);
	kHHDMOffset = hhmd_response->offset;
	kKernelPML4v = kHHDMOffset + kKernelPML4;
	init_video(framebuffer, module_response);
	printf("Parsing memory map ... %u entries\n",memmap_response->entry_count);
	memmap_init(memmap_response->entries, memmap_response->entry_count);
	printf("Initializing paging (HHMD) ... \n");
	paging_init(/*kernel_base_address_physical, kernel_base_address_virtual*/);
	printf("Initializing allocator, available memory is %Lu bytes\n",kAvailableMemory);
	allocator_init();


    // We're done, just hang...
    
	extern uint64_t kMemoryStatusCurrentPtr;
	extern memory_status_t *kMemoryStatus;
	printd(DEBUG_BOOT, "BOOT END: Status of memory status:\n");
	for (uint64_t cnt=0;cnt<kMemoryStatusCurrentPtr;cnt++)
	{
		printd(DEBUG_BOOT, "\tMemory at 0x%016Lx for 0x%016Lx (%Lu) bytes is %s\n",kMemoryStatus[cnt].startAddress, kMemoryStatus[cnt].length, kMemoryStatus[cnt].length, kMemoryStatus[cnt].in_use?"in use":"not in use");
	}
	printf("All done, hcf-time!\n");
	printd(DEBUG_BOOT,"All done, hcf-time!\n");
	hcf(-6);
}