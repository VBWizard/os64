#ifndef LIMINE_OS64_H
#define LIMINE_OS64_H
#include "limine.h"

int verify_limine_responses(struct limine_memmap_response* memmap_response, 
		struct limine_hhdm_response* hhmd_response,
		struct limine_framebuffer_response* framebuffer_response,
		struct limine_module_response* module_response,
		struct limine_smp_response* smp_response);
		
#endif