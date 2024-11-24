#include "smp.h"
#include "limine.h"
#include "kmalloc.h"
#include "apic.h"
#include "paging.h"
#include "serial_logging.h"
#include "BasicRenderer.h"

cpu_t *kCPUInfo;

extern struct limine_smp_response *kLimineSMPInfo;
extern 	uint8_t apciGetAPICID(unsigned whichAPIC);

void init_SMP()
{
	kCPUInfo = kmalloc((kLimineSMPInfo->cpu_count - 1) * sizeof(cpu_t));
	for (uint64_t core = 0; core < kLimineSMPInfo->cpu_count;core++)
	{
		kCPUInfo[core].apicID =  kLimineSMPInfo->cpus[core]->lapic_id;
		kCPUInfo[core].goto_address = &kLimineSMPInfo->cpus[core]->goto_address;
		if (core == 0)
		{
			kCPUInfo[0].registerBase=apicGetAPICBase();
			paging_map_page((pt_entry_t*)kKernelPML4v, kCPUInfo[0].registerBase + kHHDMOffset, kCPUInfo[0].registerBase, PAGE_PRESENT | PAGE_WRITE);
			//Offset to virtual since this is a virtual address
			kCPUInfo[0].registerBase += kHHDMOffset;
			kCPUInfo[0].ticksPerSecond=apicGetHZ();
			printd(DEBUG_SMP, "BSP apic timer HZ = %u\n",kCPUInfo[0].ticksPerSecond);
		}
	}
	printf("SMP: BSP timer %u\n",kCPUInfo[0].ticksPerSecond);
}