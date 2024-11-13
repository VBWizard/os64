#include "limine.h"
#include "allocator.h"
#include "paging.h"
#include "memmap.h"

void boot_init()
{
	// Tell the compiler we want intel syntax
	__asm__(".intel_syntax noprefix");
	// Disable interrupts
	__asm__("cli\n");
	// Put the CR3 value in KpageDirectoryBaseAddress
  	__asm__("mov rax, cr3\n"
    		"mov %0, rax\n" : "=r"(kKernelPML4));
}