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

uint8_t* fb_ptr = NULL;
void boot_init();

static void hcf(void) ;

// Set the base revision to 3, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;


 __attribute__((used, section(".limine_requests")))
 static volatile struct limine_framebuffer_request framebuffer_request = {
     .id = LIMINE_FRAMEBUFFER_REQUEST,
     .revision = 0
 };


 __attribute__((used, section(".limine_requests")))
 static volatile struct limine_memmap_request memmap_request = {
     .id = LIMINE_MEMMAP_REQUEST,
     .revision = 0
 };

 __attribute__((used, section(".limine_requests")))
 struct limine_kernel_address_request kernel_address_request = {
     .id = LIMINE_KERNEL_ADDRESS_REQUEST,
     .revision = 0,
 };

 __attribute__((used, section(".limine_requests")))
 struct limine_hhdm_request hhmd_request = {
     .id = LIMINE_HHDM_REQUEST,
     .revision = 0,
 };

__attribute__((used, section(".limine_requests")))
struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};


// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

// GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
// DO NOT remove or rename these functions, or stuff will eventually break!
// They CAN be moved to a different .c file.

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }

    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}

// Halt and catch fire function.
static void hcf(void) {
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

    // Ensure we got a framebuffer.
    // if (framebuffer_request.response == NULL
    //  || framebuffer_request.response->framebuffer_count < 1) {
    //     hcf();
    // }

    // // Note: we assume the framebuffer model is RGB with 32-bit pixels.
    // for (size_t i = 0; i < 100; i++) {
    //     volatile uint32_t *fb_ptr = framebuffer->address;
    //     fb_ptr[i * (framebuffer->pitch / 4) + i] = 0xffffff;
    // }
	// volatile uint32_t *fb_ptr = framebuffer->address;

	if (memmap_request.response == NULL) {
		hcf();
	}
	struct limine_memmap_response *memmap_response = memmap_request.response;
	//struct limine_kernel_address_response *kernal_address = kernel_address_request.response;
	struct limine_hhdm_response *hhmd_response = hhmd_request.response;
	struct limine_framebuffer_response* framebuffer_response = framebuffer_request.response;
	struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
	struct limine_module_response *module_response = module_request.response;
	//uint64_t kernel_base_address_physical = kernal_address->physical_base;
	//uint64_t kernel_base_address_virtual = kernal_address->virtual_base;
	// Initialize the kernel memory pool
	// kmalloc_initialize();

	// Initialize the kernel memory pool

	// Initialize the kernel memory pool
	//

	boot_init();
	kHHDMOffset = hhmd_response->offset;
	kKernelPML4v = kHHDMOffset + kKernelPML4;
	init_video(framebuffer, module_response);
	printf("Parsing memory map\n");
	memmap_init(memmap_response->entries, memmap_response->entry_count);
	printf("Initializing paging (HHMD) ... \n");
	paging_init(/*kernel_base_address_physical, kernel_base_address_virtual*/);
	printf("Initializing allocator, available memory is %u bytes\n",kAvailableMemory);
	allocator_init();

	char *x=kmalloc(512);	
	char *y=kmalloc(512);	
	sprintf(x, "Hello %u world!\n",1234);
	sprintf(y, "Hello %u world!\n",5678);
    // We're done, just hang...
    hcf();
}
