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

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_hhdm_request hhmd_request;
extern volatile struct limine_module_request module_request;

// Set the base revision to 3, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.
__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(3);
// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.
__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;


uint8_t* fb_ptr = NULL;
void boot_init();

// Halt and catch fire function.
static void hcf(void) {
	printf("Nothing to do, hcf!");
	printd(DEBUG_BOOT,"Nothing to do, hcf!");
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
        hcf();
    }

	struct limine_memmap_response *memmap_response = memmap_request.response;
	//struct limine_kernel_address_response *kernal_address = kernel_address_request.response;
	struct limine_hhdm_response *hhmd_response = hhmd_request.response;
	struct limine_framebuffer *framebuffer;
	struct limine_module_response *module_response = module_request.response;

	if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1)
		hcf();
	else
	{
		framebuffer = framebuffer_request.response->framebuffers[0];
	}

    // // Note: we assume the framebuffer model is RGB with 32-bit pixels.
    // for (size_t i = 0; i < 100; i++) {
    //     volatile uint32_t *fb_ptr = framebuffer->address;
    //     fb_ptr[i * (framebuffer->pitch / 4) + i] = 0xffffff;
    // }
	// volatile uint32_t *fb_ptr = framebuffer->address;

	if (memmap_request.response == NULL) {
		hcf();
	}
	//uint64_t kernel_base_address_physical = kernal_address->physical_base;
	//uint64_t kernel_base_address_virtual = kernal_address->virtual_base;
	// Initialize the kernel memory pool
	// kmalloc_initialize();

	// Initialize the kernel memory pool

	// Initialize the kernel memory pool
	//

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
	hcf();
}
