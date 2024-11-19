#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stdbool.h>
#include "limine.h"
#include "BasicRenderer.h"

/// @brief Number of IRQ0 ticks per second
extern uint64_t kTicksPerSecond;
/// @brief Is kernel initialization complete?
extern volatile bool kInitDone;

extern volatile uint64_t kSystemStartTime, kUptime;
extern volatile uint64_t kSystemCurrentTime;
extern volatile uint64_t kTicksSinceStart;
extern int kTimeZone;

extern volatile struct limine_framebuffer_request framebuffer_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_kernel_address_request kernel_address_request;
extern volatile struct limine_hhdm_request hhmd_request;
extern volatile struct limine_module_request module_request;
extern volatile struct limine_smp_request smp_request;
extern struct limine_module_response *limine_module_response;
extern struct limine_memmap_response *memmap_response;
extern int kTimeZone;
extern BasicRenderer kRenderer;

void kernel_main();

#endif
