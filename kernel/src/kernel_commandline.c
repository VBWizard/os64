#include "kernel_commandline.h"
#include <stdbool.h>
#include "strings/strings.h"
#include <stdint.h>
#include "printd.h"
#include "kernel.h"

extern bool kOverrideFileLogging;
extern char kRootPartUUID[];
bool kEnableAHCI=true, kEnableNVME=true;

void process_kernel_commandline(char* cmdline)
{	
	kOverrideFileLogging=false;

	printd(DEBUG_BOOT, "CMDLINE: Commandline processing\n");
	if (strnstr(cmdline,"nolog",512) != NULL)
	{
		kDebugLevel = 0;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter nolog passed, no logging will be done\n");
	}
	if (strnstr(cmdline,"alllog",512) != NULL)
	{
		kDebugLevel = 0xffffffffffffffff;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter alllog passed, all categories of logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_DETAILED",512) != NULL)
	{
		kDebugLevel |= DEBUG_DETAILED;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter det passed,  Detailed logging enabled\n");
	}
	if (strnstr(cmdline,"DEBUG_EXTRA_DETAILED",512) != NULL)
	{
		kDebugLevel |= DEBUG_DETAILED;
		kDebugLevel |= DEBUG_EXTRA_DETAILED;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter det passed,  Detailed logging enabled\n");
	}
	if (strnstr(cmdline,"noahci",512) != NULL)
	{
		kEnableAHCI=false;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter noahci passed, ahci will not be initialized or used\n");
	}
	if (strnstr(cmdline,"nonvme",512) != NULL)
	{
		kEnableNVME=false;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter nonvme passed, nvme will not be initialized or used\n");
	}
	if (strnstr(cmdline,"DEBUG_BOOT",512) != NULL)
	{
		kDebugLevel |= DEBUG_BOOT;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_BOOT passed, boot logging will be done\n");
	}

	if (strnstr(cmdline,"DEBUG_SMP",512) != NULL)
	{
		kDebugLevel |= DEBUG_SMP;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_SMP passed, SMP logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_PCI_DISCOVERY",512) != NULL)
	{
		kDebugLevel |= DEBUG_PCI_DISCOVERY;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_PCI_DISCOVERY passed, PCI discovery logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_PCI",512) != NULL)
	{
		kDebugLevel |= DEBUG_PCI;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_PCI passed, PCI logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_AHCI",512) != NULL)
	{
		kDebugLevel |= DEBUG_AHCI;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_AHCI passed, AHCI logging will be done\n");
	}

	if (strnstr(cmdline,"DEBUG_MEMMAP",512) != NULL)
	{
		kDebugLevel |= DEBUG_MEMMAP;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_MEMMAP passed, memmap logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_ACPI",512) != NULL)
	{
		kDebugLevel |= DEBUG_ACPI;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_ACPI passed, ACPI logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_PAGING",512) != NULL)
	{
		kDebugLevel |= DEBUG_PAGING;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_PAGING passed, paging logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_ALLOCATOR",512) != NULL)
	{
		kDebugLevel |= DEBUG_ALLOCATOR;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_ALLOCATOR passed, memory allocator logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_KMALLOC",512) != NULL)
	{
		kDebugLevel |= DEBUG_KMALLOC;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_KMALLOC passed, kernel memory allocation logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_NVME",512) != NULL)
	{
		kDebugLevel |= DEBUG_NVME;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_NVME passed, NVME logging will be done\n");
	}
	if (strnstr(cmdline,"DEBUG_EVERYTHING",512) != NULL)
	{
		kDebugLevel |= DEBUG_EVERYTHING;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter DEBUG_NVME passed, NVME logging will be done\n");
	}
#ifdef ENABLE_COM1
	if (strnstr(cmdline,"noseriallog",512) != NULL)
	{
		kOverrideFileLogging = true;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter NOLOGFILE passed, logging will go to the screen\n");
	}
#endif
	if (strnstr(cmdline,"NOSMP",512) != NULL)
	{
		kEnableSMP = false;
		printd(DEBUG_BOOT, "CMDLINE:\t Parameter NOSMP passed, only the boot processor core will be initialized\n");
	}

	char* ptr=0;
	if ((ptr=strnstr(cmdline,"ROOTPARTUUID=",512)) != NULL)
	{
		ptr+=13;
		strncpy((char*)&kRootPartUUID, ptr, 36);
	}
}