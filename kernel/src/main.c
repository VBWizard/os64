#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "kernel.h"
#include "limine_os64.h"
#include "io.h"
#include "CONFIG.h"
#include "paging.h"
#include "strcpy.h"
#include "printd.h"

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_hhdm_request hhmd_request;
extern volatile struct limine_module_request module_request;
extern volatile struct limine_smp_request smp_request;
extern struct limine_smp_response *kLimineSMPInfo;
extern volatile struct limine_kernel_file_request kernel_file_request;
extern uint64_t kHHDMOffset;
extern char kKernelCommandline[];

struct limine_memmap_response *memmap_response;
struct limine_hhdm_response *hhmd_response;
struct limine_framebuffer_response *framebuffer_response;
struct limine_module_response *limine_module_response;
struct limine_framebuffer *framebuffer;
struct limine_kernel_file_response *kernelFileResponse;

char kernel_stack[0x1000*64] __attribute__((aligned(16)));


__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(3);

uint8_t* fb_ptr = NULL;

// Halt and catch fire function.
static void hcf(int error_number) {
    int error = error_number;
/*	char* buffer = (char*)0xb8000;
	buffer[0] = 'H';
	buffer[2] = 'C';
	buffer[4] = 'F';
	buffer[6] = '!';
	buffer[8] = '!';*/
	for (;;) {
		asm ("sti\nhlt\n");
    }
}

#include <stdint.h>

void pc_speaker_beep() {
    uint16_t frequency = 440; // Frequency in Hz (A4)

    // Calculate divisor for the PIT (1193182 Hz clock)
    uint16_t divisor = 1193182 / frequency;

    // Set up PIT for square wave on Channel 2
    __asm__ __volatile__ (
        "mov al, 0xB6\n\t"           // Command: Binary mode, Mode 3, Channel 2
        "out 0x43, al\n\t"           // Send to PIT command port
        "mov al, %0\n\t"             // Low byte of divisor
        "out 0x42, al\n\t"           // Send to Channel 2 data port
        "mov al, %1\n\t"             // High byte of divisor
        "out 0x42, al\n\t"           // Send to Channel 2 data port
        :
        : "r" ((uint8_t)(divisor & 0xFF)), // Low byte of divisor
          "r" ((uint8_t)((divisor >> 8) & 0xFF)) // High byte of divisor
        : "al"
    );

    // Enable the speaker
    __asm__ __volatile__ (
        "in al, 0x61\n\t"            // Read current value of port 0x61
        "or al, 0x03\n\t"            // Set bits 0 and 1
        "out 0x61, al\n\t"           // Write back to port 0x61
        :
        :
        : "al"
    );

    // Wait for a short time (busy loop)
    for (volatile uint32_t i = 0; i < 0xFFFFF; i++);

    // Disable the speaker
    __asm__ __volatile__ (
        "in al, 0x61\n\t"            // Read current value of port 0x61
        "and al, 0xFC\n\t"           // Clear bits 0 and 1
        "out 0x61, al\n\t"           // Write back to port 0x61
        :
        :
        : "al"
    );
}

void toggle_keyboard_leds(uint8_t led_status) {
    while ((inb(0x64) & 0x02));      // Wait until the keyboard controller is ready
    outb(0x60, 0xED);                // Send 'Set LEDs' command
    while ((inb(0x64) & 0x02));      // Wait until the keyboard controller is ready
    outb(0x60, led_status);          // Send LED status byte (e.g., 0x02 for Num Lock)
}

void post_code(uint8_t code) {
	__asm__ __volatile__ ("out 0x80, %0\n" : : "a"(code));
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
	kernelFileResponse = kernel_file_request.response;
	strncpy(kKernelCommandline, kernelFileResponse->kernel_file->cmdline, 512);

	int limine_response_status = verify_limine_responses(memmap_response, hhmd_response, framebuffer_response, limine_module_response, kLimineSMPInfo);

	if (limine_response_status != 0)
		hcf(limine_response_status);
	
	kHHDMOffset = hhmd_response->offset;
	kernel_main();
}
