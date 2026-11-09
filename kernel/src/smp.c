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
#include "sprintf.h"

cpu_t kCPUs[MAX_CPUS];
uint32_t kCPUCount;
uint8_t kApicToCPU[CPU_ROSTER_APIC_IDS];
cpu_roster_report_t kCPURosterReport;
volatile uintptr_t kMPApicBase;
struct mpf_intel* kMPTable;
mpConfig_t* kMPConfigTable;
uint32_t kMPConfigTableCount = 0;
uintptr_t kIOAPICAddress;
struct mpf_intel* mp;
struct mpc_table* mc;
uintptr_t kMPICRLow=0;
uintptr_t kMPICRHigh=0;
uintptr_t kMPLVTTimer=0;
extern struct limine_smp_response *kLimineSMPInfo;
extern bool kEnableSMP;
extern void enable_fsgsbase();

int kLocalAPICTimerSpeed[MAX_CPUS];
volatile core_local_storage_t* kDebugHoneypot = 0;

volatile core_local_storage_t kCoreLocalStorage[MAX_CPUS] = {0};

// MAXCPUS=N: how many CPUs this boot numbers and brings up; 0 = all of them.
// The rest are never woken — they stay parked in Limine's AP spin-loop. It
// takes the first N of the roster's order, so the BSP is always one of them.
// -1 = not given, which is not the same as 0: MAXCPUS=0 must still win over a
// stale MAXCORES=N.
int kMaxCPUs = -1;
// MAXCORES=N, the token's name before CPU was the word. Still honoured,
// because boot tokens live in each machine's own limine.conf and do not
// travel with a refresh; it says what it is called now, so it fades.
int kMaxCPUsAlias = -1;

// Number the CPUs (cpu_roster.h). Runs in kernel_main, on the BSP, before
// the log rings — they are allocated one per numbered CPU — and before
// anything else reads a CPU number. Limine's list and the cmdline are all it
// needs, and both exist by then.
void cpu_roster_init(void)
{
	if (kMaxCPUsAlias >= 0)
	{
		printf("MAXCORES=%d is now spelled MAXCPUS=%d%s\n", kMaxCPUsAlias, kMaxCPUsAlias,
		       kMaxCPUs >= 0 ? " (MAXCPUS= given too, and it wins)" : "");
		printd(DEBUG_BOOT, "MAXCORES=%d is now spelled MAXCPUS=%d%s\n", kMaxCPUsAlias, kMaxCPUsAlias,
		       kMaxCPUs >= 0 ? " (MAXCPUS= given too, and it wins)" : "");
		if (kMaxCPUs < 0)
			kMaxCPUs = kMaxCPUsAlias;
	}
	uint32_t cap = kEnableSMP ? (kMaxCPUs > 0 ? (uint32_t)kMaxCPUs : 0) : 1;

	// Limine's WHOLE list as plain IDs, however long: the roster decides
	// which entries are usable, and an addressable ID can sit after any
	// number of x2APIC-only or repeated ones.
	uint32_t listed = (uint32_t)kLimineSMPInfo->cpu_count;
	if (listed == 0)
		panic("CPU roster: Limine listed no CPUs, not even the BSP");
	uint32_t *ids = kmalloc(listed * sizeof(uint32_t));
	if (ids == NULL)
		panic("CPU roster: no room for Limine's list of %u CPUs", listed);
	for (uint32_t i = 0; i < listed; i++)
		ids[i] = kLimineSMPInfo->cpus[i]->lapic_id;

	static uint32_t slot[MAX_CPUS];
	if (!cpu_roster_build(ids, listed, kLimineSMPInfo->bsp_lapic_id, cap, MAX_CPUS,
	                      slot, kApicToCPU, &kCPURosterReport))
		panic("CPU roster: the BSP (APIC %u) is not among the %u CPUs Limine listed, or is not addressable",
		      kLimineSMPInfo->bsp_lapic_id, listed);
	kfree(ids);

	kCPUCount = kCPURosterReport.count;
	for (uint32_t cpu = 0; cpu < kCPUCount; cpu++)
	{
		struct limine_smp_info *info = kLimineSMPInfo->cpus[slot[cpu]];
		kCPUs[cpu].apic = (apic_id_t){ info->lapic_id };
		kCPUs[cpu].goto_address = (volatile uint64_t *)&info->goto_address;
	}
}

void cpu_roster_print(void)
{
	const cpu_roster_report_t *r = &kCPURosterReport;
	char line[192];
	int n = snprintf(line, sizeof(line), "%u CPU%s", kCPUCount, kCPUCount == 1 ? "" : "s");
	if (!r->dense)
		n += snprintf(line + n, sizeof(line) - n, ", APIC IDs %u-%u, sparse", r->apic_lowest, r->apic_highest);
	if (r->parked)
		n += snprintf(line + n, sizeof(line) - n, ", %u parked by %s", r->parked,
		              r->parked_by_max ? "MAX_CPUS" : (kEnableSMP ? "MAXCPUS=" : "nosmp"));
	if (r->too_wide)
		n += snprintf(line + n, sizeof(line) - n, ", %u left off (APIC ID above %u: x2APIC only)",
		              r->too_wide, CPU_ROSTER_APIC_MAX);
	if (r->duplicates)
		n += snprintf(line + n, sizeof(line) - n, ", %u duplicate APIC ID%s ignored",
		              r->duplicates, r->duplicates == 1 ? "" : "s");
	printf("(%s)\n", line);
	printd(DEBUG_BOOT, "SMP: %s\n", line);
	// The numbering is topological only when the BSP has the lowest ID —
	// true on every PC this project has met, so say so when it is not.
	if (r->bsp_not_lowest)
	{
		printf("SMP: BSP is APIC %u; CPU numbering is not topological\n", kCPUs[0].apic.id);
		printd(DEBUG_BOOT, "SMP: BSP is APIC %u; CPU numbering is not topological\n", kCPUs[0].apic.id);
	}
}

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
    // The BIOS data area lives in physical page zero. Its HHDM alias is
    // mapped for exactly one read and taken down again: physical 0 is what
    // a failed page walk returns, and the lazy HHDM's whole point is that
    // `0 | kHHDMOffset` FAULTS for a caller that forgot to check — an alias
    // left behind would turn that tripwire into a silent read of the BDA.
    paging_map_page((pt_entry_t *)kKernelPML4v, kHHDMOffset, 0, PAGE_PRESENT);
    const uint16_t *ebda_segment = (const uint16_t *)(kHHDMOffset + 0x40e);
    uintptr_t ebda = (uintptr_t)*ebda_segment << 4;
    paging_unmap_page((pt_entry_t *)kKernelPML4v, kHHDMOffset);
    bool lResult = false;

    // An absent or malformed EBDA pointer cannot name the guarded page.
    if (ebda >= PAGE_SIZE)
        lResult=mp_scan_for_config(ebda, 0x400);
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
    kMPConfigTableCount = 0;
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
		// One row per record, and a record is a CPU, a bus, an IO APIC or an
		// interrupt assignment — far more than there are CPUs on any real
		// table, so the table is sized by the count it is about to hold.
		kMPConfigTable = kmalloc(mc->count * sizeof(mpConfig_t));
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
					// ACPI's choice stands (acpi.c picks the IO APIC that
					// serves the ISA IRQs); the MP table supplies one only when
					// ACPI had none.
					if (!kIOAPICAddress)
						kIOAPICAddress=(uintptr_t)apic->ioapicaddr;
					else if (kIOAPICAddress!=(uintptr_t)apic->ioapicaddr)
						printd(DEBUG_SMP, "SMP: MP table IO APIC 0x%08x is not ACPI's 0x%016lx - keeping ACPI's\n", apic->ioapicaddr, kIOAPICAddress);
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
		kMPConfigTableCount = mc->count;
		return mc->count;
	}
	return 0;
}

// The platform half of SMP: the MP table, the IO APIC, and the BSP's local
// APIC mapped where every CPU will reach its own. Which CPUs exist, and how
// many this boot runs, the roster already decided (cpu_roster_init).
int init_SMP(bool enableSMP)
{
	int mp_records;
    mp_records = parse_mp_table();
    if (!kIOAPICAddress)
    {
        if (enableSMP)
                    panic("Unable to determine IO APIC address after _MP_ and ACPI scans.");
#ifndef DEBUG_NONE
            if ((kDebugLevel & DEBUG_SMP) == DEBUG_SMP)
                    printd(DEBUG_SMP, "SMP: IO APIC address unavailable, continuing without SMP support.\n");
#endif
    }
    // Every CPU's local APIC answers at the same physical address, each one
    // seeing its own registers there, so one mapping serves them all.
    uintptr_t lapicPhys = apicGetAPICBase();
    printd(DEBUG_SMP, "BSP APIC register base found @ 0x%016lx, remapped to 0x%016lx\n", lapicPhys, lapicPhys + kHHDMOffset);
    paging_map_page((pt_entry_t*)kKernelPML4v, lapicPhys + kHHDMOffset, lapicPhys, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
    if (kIOAPICAddress)
    {
            paging_map_page((pt_entry_t*)kKernelPML4v, kIOAPICAddress + kHHDMOffset, kIOAPICAddress, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
            printd(DEBUG_SMP, "SMP: IO APIC found @ 0x%016lx, remapped to 0x%016lx\n", kIOAPICAddress, kIOAPICAddress + kHHDMOffset);
            kIOAPICAddress += kHHDMOffset;
    }
    else
    {
#ifndef DEBUG_NONE
        if ((kDebugLevel & DEBUG_SMP) == DEBUG_SMP)
                printd(DEBUG_SMP, "SMP: IO APIC not present, skipping remap.\n");
#endif
    }
    kMPApicBase = lapicPhys + kHHDMOffset;
    kMPICRLow=kMPApicBase+0x300;
    kMPICRHigh=kMPApicBase+0x310;
    kMPLVTTimer=kMPApicBase+0x320;
    kMPIdReg = kMPApicBase+0x20;
    uint32_t lapicHz = apicGetHZ();
    printd(DEBUG_SMP, "BSP apic timer HZ = %u\n", lapicHz);
	if (check_for_apic())
	{
        printd(DEBUG_SMP, "SMP: %s APIC %u Found, address 0x%016lx, initializing ... ",
			acpiGetAPICVersion()==0?"Discrete":"Integrated",
			kCPUs[0].apic.id,
			kMPApicBase);
        if (apicIsEnabled())
            printf("enabled ... ");
		else
			printf("WARNING - not enabled ... ");
        printf("done. ");
	}
    kDebugHoneypot = kmalloc_aligned(kLimineSMPInfo->cpu_count * sizeof(core_local_storage_t));
    return mp_records;
}
