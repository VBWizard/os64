#include "kernel_commandline.h"
#include <stdbool.h>
#include "strings/strings.h"
#include <stdint.h>
#include "printd.h"

bool kEnableAHCI=true, kEnableNVME=true;

void process_kernel_commandline(char* cmdline)
{
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
	if (strnstr(cmdline,"detlog",512) != NULL)
	{
		kDebugLevel |= DEBUG_DETAILED;
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

}