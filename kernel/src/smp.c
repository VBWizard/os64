#include "smp.h"
#include "CONFIG.h"
#include "limine.h"
#include "kmalloc.h"
#include "apic.h"
#include "paging.h"
#include "serial_logging.h"
#include "BasicRenderer.h"
#include "memcpy.h"
#include "memset.h"
#include "strcmp.h"
#include "panic.h"
#include "smp_core.h"

cpu_t *kCPUInfo;
volatile uintptr_t kMPApicBase;
volatile uint8_t kMPCoreCount;
struct mpf_intel* kMPTable;
mpConfig_t* kMPConfigTable;
uintptr_t kIOAPICAddress;
struct mpf_intel* mp;
struct mpc_table* mc;
uintptr_t kMPICRLow=0;
uintptr_t kMPICRHigh=0;
uintptr_t kMPLVTTimer=0;
extern struct limine_smp_response *kLimineSMPInfo;
extern 	uint8_t apciGetAPICID(unsigned whichAPIC);
extern void enable_fsgsbase();

int kLocalAPICTimerSpeed[MAX_CPUS];
volatile core_local_storage_t* kCoreLocalStorage = 0;

bool mp_scan_for_config(uintptr_t start, uintptr_t length)
{
	uint64_t pagesToMap = length / PAGE_SIZE;
	if (length % PAGE_SIZE)
		pagesToMap++;
	printd(DEBUG_SMP | DEBUG_DETAILED,"mp_scan_for_config; Mapping 0x%08x-->0x%08x\n", start, start+length);
	paging_map_pages((uintptr_t*)kKernelPML4v, start, start, pagesToMap, PAGE_PRESENT);
    for (uintptr_t cnt=start;cnt<start+length;cnt++)
    {
        kMPTable=(struct mpf_intel*)cnt;
        if(!strncmp(kMPTable->signature,"_MP_",4))
        {
            mp=(struct mpf_intel*)cnt;
            return true;
        }
    }
    return false;
}

bool mp_find_tables()
{
    uint16_t* lEBDAPtr=(uint16_t*)0x40e;
    bool lResult;
    
    if (lEBDAPtr != 0)
        lResult=mp_scan_for_config(*lEBDAPtr<<4, 0x400);
    if (!lResult)
        if (!mp_scan_for_config(0x9fc00, 0x400))
            if (!mp_scan_for_config(0xF0000, 0xFFFF))
            {
#ifndef DEBUG_NONE
                 if ((kDebugLevel & DEBUG_SMP) == DEBUG_SMP)
                    printd(DEBUG_SMP,"MP tables not found, MP not allowed, proceeding as single processor.\n");
#endif
                return false;
            }
#ifndef DEBUG_NONE
    if ((kDebugLevel & DEBUG_SMP) == DEBUG_SMP)
        printd(DEBUG_SMP,"MP tables found at 0x%08x, spec %c\n", kMPTable, kMPTable->specification);
#endif
    return true;
        
}

unsigned int parse_mp_table()
{
    uint8_t* recPtr;
    char lTempString[15];
    memset(lTempString, 0, 15);
    if (mp_find_tables())
	{
		printd(DEBUG_SMP,"MP table found @ 0x%08x, sig=%c%c%c%c, features=%u/%u/%u/%u/%u, length=%u\n", kMPTable, kMPTable->signature[0], kMPTable->signature[1], kMPTable->signature[2], kMPTable->signature[3], kMPTable->feature1, kMPTable->feature2, kMPTable->feature3, kMPTable->feature4, kMPTable->feature5, kMPTable->length);
		if (kMPTable->feature1!=0)
		{
			printd(DEBUG_SMP,"NOTE: need to implement Intel MP default configurations (%u)",kMPTable->feature1);
			return false;
		}
		mc=(struct mpc_table*)(uintptr_t)kMPTable->physptr;
		printd(DEBUG_SMP,"MC table found @ 0x%08x, sig=%c%c%c%c, length=%u, lapic=0x%08x, OEM tbl=0x%08x\n", mc, mc->signature[0], mc->signature[1], mc->signature[2], mc->signature[3], mc->length, mc->lapic, mc->oemptr);
		memcpy(lTempString, mc->oem,8);
		printd(DEBUG_SMP,"\tMPC OEM '%s'", lTempString);
		memset(lTempString, 0, 15);
		memcpy(lTempString, mc->productid,12);
		printd(DEBUG_SMP,", product': %s\n", lTempString);
		recPtr=(uint8_t*)(uintptr_t)kMPTable->physptr+sizeof(struct mpc_table);
		printd(DEBUG_SMP,"Parsing MC %u table entries at 0x%08x\n", mc->count, recPtr);
		for (int cnt=0;cnt< mc->count;cnt++)
		{
			switch((int)*(uint8_t*)recPtr)
			{
				case 0:
					//TODO: Add all this info to a kernel structure
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"\tCPU:  ");
					struct mpc_cpu* cpu=(struct mpc_cpu*)recPtr;
					memcpy(kMPConfigTable[cnt].rec,recPtr,20);
					if (cnt==0)
						kMPConfigTable[cnt].prevRecAddress=0xFFFFFFFF;
					else if (cnt==mc->count-1)
						kMPConfigTable[cnt].nextRecAddress=0xFFFFFFFF;
					else
					{
						kMPConfigTable[cnt].prevRecAddress=(uintptr_t)&kMPConfigTable[cnt-1];
						kMPConfigTable[cnt-1].nextRecAddress=(uintptr_t)&kMPConfigTable[cnt];
					}
					kMPConfigTable[cnt].recType=CPU;
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"APICId=%u, Enabled=%s, %s\n", cpu->apicid, (cpu->cpuflag&0x1)?"yes":"no", (cpu->cpuflag&0x2)?"BSP":"");
					recPtr+=20;
					break;
				case 1:
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"\tBus:  ");
					struct mpc_bus* bus=(struct mpc_bus*)recPtr;
					memcpy(kMPConfigTable[cnt].rec,recPtr,8);
					if (cnt==0)
						kMPConfigTable[cnt].prevRecAddress=0xFFFFFFFF;
					else if (cnt==mc->count-1)
						kMPConfigTable[cnt].nextRecAddress=0xFFFFFFFF;
					else
					{
						kMPConfigTable[cnt].prevRecAddress=(uintptr_t)&kMPConfigTable[cnt-1];
						kMPConfigTable[cnt-1].nextRecAddress=(uintptr_t)&kMPConfigTable[cnt];
					}
					recPtr+=8;
					kMPConfigTable[cnt].recType=BUS;
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"ID=%u, BusType=%c%c%c%c%c%c, Type=%c\n", bus->busid, bus->bustype[0], bus->bustype[1], bus->bustype[2], bus->bustype[3], bus->bustype[4], bus->bustype[5], bus->type );
					break;
				case 2:
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"\tIO APIC: ");
					struct mpc_ioapic* apic=(struct mpc_ioapic*)recPtr;
					memcpy(kMPConfigTable[cnt].rec,recPtr,8);
					if (cnt==0)
						kMPConfigTable[cnt].prevRecAddress=0xFFFFFFFF;
					else if (cnt==mc->count-1)
						kMPConfigTable[cnt].nextRecAddress=0xFFFFFFFF;
					else
					{
						kMPConfigTable[cnt].prevRecAddress=(uintptr_t)&kMPConfigTable[cnt-1];
						kMPConfigTable[cnt-1].nextRecAddress=(uintptr_t)&kMPConfigTable[cnt];
					}
					kMPConfigTable[cnt].recType=IOAPIC;
					recPtr+=8;
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"ID=%02X, Address=0x%08x, Enabled=%s\n",apic->apicid, apic->ioapicaddr, apic->flags&0x1?"yes":"no");
					if (!kIOAPICAddress)
					{
						if (kIOAPICAddress!=(uintptr_t)apic->ioapicaddr)
							printd(DEBUG_EXCEPTIONS, "SMP: WARNING - ACPI IO APIC address (0x%16lx) != MP table IO APIC address (0x%016lx)", kIOAPICAddress, apic->ioapicaddr);
					}
					else
						kIOAPICAddress=(uintptr_t)apic->ioapicaddr;
					break;
				case 3:
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"\tIRQ:  ");
					struct mpc_intsrc*intr=(struct mpc_intsrc*)recPtr;
					memcpy(kMPConfigTable[cnt].rec,recPtr,8);
					if (cnt==0)
						kMPConfigTable[cnt].prevRecAddress=0xFFFFFFFF;
					else if (cnt==mc->count-1)
						kMPConfigTable[cnt].nextRecAddress=0xFFFFFFFF;
					else
					{
						kMPConfigTable[cnt].prevRecAddress=(uintptr_t)&kMPConfigTable[cnt-1];
						kMPConfigTable[cnt-1].nextRecAddress=(uintptr_t)&kMPConfigTable[cnt];
					}
					kMPConfigTable[cnt].recType = IOINTASS;
					recPtr+=8;
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"Type=%s, Pol=%s, Trig=%s, sB=%02X, sI=%02X, dA=%02X, dI=%02X\n", 
							intr->irqtype==0?"INT"                      
								:intr->irqtype==1?"NMI"
								:intr->irqtype==2?"SMI"
								:intr->irqtype==3?"Ext"
								:"unk",                                 //iType
							(intr->irqflag&0x3)==0x0?"AL"
								:(intr->irqflag&0x3)==0x1?"AH"
								:(intr->irqflag&0x3)==0x2?"Res":"AL(2)", //Polarity
							(intr->irqflag&0xc)==0x0?"E"
								:(intr->irqflag&0xc)==0x4?"E(2)"
								:(intr->irqflag&0xc)==0x8?"R":"L", //Trigger
							intr->srcbus, intr->srcbusirq, intr->dstapic, intr->dstirq);
					break;
				case 4:
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"\tLINT: ");
					struct mpc_lintsrc* lint=(struct mpc_lintsrc*)recPtr;
					memcpy(kMPConfigTable[cnt].rec,recPtr,8);
					if (cnt==0)
						kMPConfigTable[cnt].prevRecAddress=0xFFFFFFFF;
					else if (cnt==mc->count-1)
						kMPConfigTable[cnt].nextRecAddress=0xFFFFFFFF;
					else
					{
						kMPConfigTable[cnt].prevRecAddress=(uintptr_t)&kMPConfigTable[cnt-1];
						kMPConfigTable[cnt-1].nextRecAddress=(uintptr_t)&kMPConfigTable[cnt];
					}
					kMPConfigTable[cnt].recType = LOCALINTASS;
					recPtr+=8;
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"Type=%s, Pol=%s, Trig=%s, sB=%02X, sI=%02X, dA=%02X, dL=%02X\n", 
								lint->irqtype==0?"INT"                      //type
								:lint->irqtype==1?"NMI"
								:lint->irqtype==2?"SMI"
								:lint->irqtype==3?"Ext"
								:"unk", //iType
							(lint->irqflag&0x3)==0x0?"AL"
								:(lint->irqflag&0x3)==0x1?"AH"
								:(lint->irqflag&0x3)==0x2?"Res":"AL(2)", //Polarity
							(lint->irqflag&0xc)==0x0?"E"
								:(lint->irqflag&0xc)==0x4?"E(2)"
								:(lint->irqflag&0xc)==0x8?"R":"L", //Trigger
							lint->srcbusid, lint->srcbusirq, lint->destapic, lint->destapiclint);
					break;
				default:
					printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"\tue %04X, ", (int)*(uint8_t*)recPtr);
					recPtr+=8;
					break;
			}
		}
		printd(DEBUG_SMP | DEBUG_DETAILED | DEBUG_EXTRA_DETAILED,"rec5 pp=%08X(%08X),np=%08X(%08X)\n", kMPConfigTable[5].prevRecAddress, &kMPConfigTable[4], kMPConfigTable[5].nextRecAddress, &kMPConfigTable[6]);
		return mc->count;
	}
	return 0;
}

//We will gather the details for each core, but only enable them if kEnableSMP = true
int init_SMP(bool enableSMP)
{
	int mp_records;
    kMPCoreCount = enableSMP?kLimineSMPInfo->cpu_count:1;

    kMPConfigTable = kmalloc(MAX_CPUS * sizeof(mpConfig_t));
    mp_records = parse_mp_table();
	if (!kIOAPICAddress)
		panic("Unable to determine IO APIC address after _MP_ and ACPI scans.");
	kCPUInfo = kmalloc((kMPCoreCount) * sizeof(cpu_t));
	for (uint64_t core = 0; core < kMPCoreCount;core++)
	{
		kCPUInfo[core].apicID =  kLimineSMPInfo->cpus[core]->lapic_id;
		kCPUInfo[core].goto_address = &kLimineSMPInfo->cpus[core]->goto_address;
		kCPUInfo[core].registerBase=apicGetAPICBase();
		kCPUInfo[core].ioAPICAddress = kIOAPICAddress;
		kCPUInfo[core].apic_lvt_timer  = kCPUInfo[core].registerBase + 0x320; // LVT Timer
		kCPUInfo[core].apic_lvt_lint0  = kCPUInfo[core].registerBase + 0x350; // LVT LINT0 (External IRQ)
		kCPUInfo[core].apic_lvt_lint1  = kCPUInfo[core].registerBase + 0x360; // LVT LINT1 (NMI)
		kCPUInfo[core].apic_lvt_error  = kCPUInfo[core].registerBase + 0x370; // LVT Error Register
		kCPUInfo[core].apic_tpr = kCPUInfo[core].registerBase + 0x80; // LVT Error Register
		//Offset to virtual since this is a virtual address
		kCPUInfo[core].apic_id_reg=kCPUInfo[core].registerBase+0x20;
		kCPUInfo[core].apic_svr=kCPUInfo[core].registerBase+0xF0;
		kCPUInfo[core].apic_eoi=kCPUInfo[core].registerBase+0xB0;
		kCPUInfo[core].apic_icr_low=kCPUInfo[core].registerBase+0x300;
		kCPUInfo[core].apic_icr_high=kCPUInfo[core].registerBase+0x310;
		if (core == 0)
		{
			printd(DEBUG_SMP, "BSP APIC register base found @ 0x%016lx, remapped to 0x%016lx\n",kCPUInfo[0].registerBase, kCPUInfo[0].registerBase + kHHDMOffset);
			paging_map_page((pt_entry_t*)kKernelPML4v, kCPUInfo[0].registerBase + kHHDMOffset, kCPUInfo[0].registerBase, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
			paging_map_page((pt_entry_t*)kKernelPML4v, kIOAPICAddress + kHHDMOffset, kIOAPICAddress, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
			printd(DEBUG_SMP, "SMP: IO APIC found @ 0x%016lx, remapped to 0x%016lx\n", kIOAPICAddress, kIOAPICAddress + kHHDMOffset);
			kIOAPICAddress += kHHDMOffset;
			printd(DEBUG_SMP, "BSP apic timer HZ = %u\n",kCPUInfo[core].ticksPerSecond);
		}
		else
			kCPUInfo[core].ticksPerSecond=kCPUInfo[0].ticksPerSecond;
		//Now that we've logged all the register values and mapped them to the higher half, make them higher half
		kCPUInfo[core].registerBase += kHHDMOffset;
		kCPUInfo[core].apic_id_reg += kHHDMOffset;
		kCPUInfo[core].apic_svr += kHHDMOffset;
		kCPUInfo[core].apic_eoi += kHHDMOffset;
		kCPUInfo[core].apic_icr_low += kHHDMOffset;
		kCPUInfo[core].apic_icr_high += kHHDMOffset;
		kCPUInfo[core].apic_lvt_timer += kHHDMOffset;
		kCPUInfo[core].apic_lvt_lint0 += kHHDMOffset;
		kCPUInfo[core].apic_lvt_lint1 += kHHDMOffset;
		kCPUInfo[core].apic_lvt_error += kHHDMOffset;
		kCPUInfo[core].apic_tpr += kHHDMOffset;
		kCPUInfo[core].ticksPerSecond=apicGetHZ();
	}
	if (check_for_apic())
	{
		kMPApicBase = kCPUInfo[0].registerBase;
		kMPICRLow=kMPApicBase+0x300;
		kMPICRHigh=kMPApicBase+0x310;
		kMPLVTTimer=kMPApicBase+0x320;
		kMPIdReg = kCPUInfo[0].apic_id_reg;
        printd(DEBUG_SMP, "SMP: %s APIC %u Found, address 0x%016lx, initializing ... ", 
			acpiGetAPICVersion()==0?"Discrete":"Integrated", 
			kCPUInfo[0].apicID, 
			kCPUInfo[0].registerBase);
        if (apicIsEnabled())
            printf("enabled ... ");
		else
			printf("WARNING - not enabled ... ");
        printf("done. ");
	}
	kCoreLocalStorage = kmalloc_aligned(kLimineSMPInfo->cpu_count * sizeof(core_local_storage_t));
	return mp_records;
}
