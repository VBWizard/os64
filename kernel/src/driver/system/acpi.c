#include "acpi.h"
#include "CONFIG.h"
#include "io.h"        // inw/outw/outb — the PM1 control registers are ports
#include "BasicRenderer.h"   // printf — acpi_poweroff reports to the GLASS (see it)
#include "strcmp.h"
#include "serial_logging.h"
#include "paging.h"
#include "panic.h"
#include "smp.h"

extern uintptr_t kPCIBaseAddress;
extern uintptr_t kLimineRSDP;

// The FADT, kept after the boot walk instead of being discarded with the
// stack frame that found it: the S5 poweroff path below needs its PM1 control
// blocks and its DSDT pointer, and the honest place to read them is once, at
// boot, while the tables are known-mapped and the machine is healthy.
acpiFADT_t *kAcpiFADT = NULL;

// ISA IRQ -> GSI routing from the MADT's Interrupt Source Overrides (see
// acpi.h). -1 = no override = identity mapping per the ACPI spec.
int16_t kISAIrqToGSI[16] = { -1, -1, -1, -1, -1, -1, -1, -1,
                             -1, -1, -1, -1, -1, -1, -1, -1 };
uint16_t kISAIrqOverrideFlags[16] = { 0 };

void parseMCFG(uintptr_t mcfgAddress) {
    acpi_mcfg_table_t* mcfg = (acpi_mcfg_table_t*)mcfgAddress;

    // Calculate the number of entries in the MCFG table
	size_t headerSize = sizeof(acpi_mcfg_table_t);
    size_t entryCount = (mcfg->length - headerSize) / sizeof(acpi_mcfg_entry_t);
    printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI: MCFG Table has %u entries\n", entryCount);

 // Iterate through the entries
	acpi_mcfg_entry_t* entries = (acpi_mcfg_entry_t*)((uint8_t*)mcfg + headerSize);
    for (size_t i = 0; i < entryCount; i++) {
        acpi_mcfg_entry_t* entry = &entries[i];
        printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI:  Entry %u:\n", i);
        printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI:    Base Address: 0x%016x\n", (unsigned long long)entry->base_address);
        printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI:    Segment Group: %u\n", entry->segment_group);
        printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI:    Start Bus: %u\n", entry->start_bus_number);
        printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI:    End Bus: %u\n", entry->end_bus_number);
        printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI:    Reserved: 0x%08x\n", entry->reserved);
	}
    for (size_t i = 0; i < entryCount; i++) {
        // Access each MCFG entry
        acpi_mcfg_entry_t* entry = (acpi_mcfg_entry_t*)&entries[i];
        printd(DEBUG_ACPI, "ACPI: Entry %u: BaseAddress=0x%016lx, SegmentGroup=%u, StartBus=%u, EndBus=%u\n",
               i, (unsigned long long)entry->base_address, entry->segment_group,
               entry->start_bus_number, entry->end_bus_number);

        // Use the BaseAddress for Segment 0 to identify the PCI base address
        if (entry->segment_group == 0) {
			kPCIBaseAddress = entry->base_address;
            printd(DEBUG_BOOT, "ACPI: *Found PCI Segment 0 Base Address: 0x%08x, mapping 0x7800 pages so that we can scan 120 busses\n", (unsigned long long)entry->base_address);	
			paging_map_pages((pt_entry_t*)kKernelPML4v,kHHDMOffset | kPCIBaseAddress, kPCIBaseAddress, 0x7800, PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
        }
		else
		{
			panic("PCI base address not found in MCFG\n");
		}
		break;
    }
}

uintptr_t doRSDPSearch(uintptr_t from, int count) {
    uint8_t* fromPtr = (uint8_t*)from; // Cast base address to byte pointer

    printd(DEBUG_ACPI, "ACPI: doRSDPSearch: Scanning range 0x%08x - 0x%08x\n",
           (uint32_t)from, (uint32_t)(from + count));

    for (int cnt = 0; cnt < count; cnt += 16) { // 16-byte alignment
        if (fromPtr[cnt] == 'R' && fromPtr[cnt + 1] == 'S' && fromPtr[cnt + 2] == 'D' && fromPtr[cnt + 3] == ' ' &&
            fromPtr[cnt + 4] == 'P' && fromPtr[cnt + 5] == 'T' && fromPtr[cnt + 6] == 'R' && fromPtr[cnt + 7] == ' ') {
            uint8_t checksum = 0;
            for (int i = 0; i < 20; i++) { // Standard RSDP checksum (first 20 bytes)
                checksum += fromPtr[cnt + i];
            }
            if (checksum == 0) { // Valid checksum
                return (uintptr_t)&fromPtr[cnt];
            }
        }
    }
    return 0xFFFFFFFFFFFFFFFF; // Not found
}

acpiFADT_t* acpiFindTable(void* RootSDT, char* tableSignature) {
    acpi_table_header_t* header = (acpi_table_header_t*)RootSDT;
    uint32_t entries = 0;
    uintptr_t tablePointer = 0;

    printd(DEBUG_ACPI, "ACPI: Root SDT signature: %c%c%c%c\n",
           header->Signature[0], header->Signature[1], header->Signature[2], header->Signature[3]);

    // Determine whether we are using RSDT or XSDT
    if (header->Signature[0] == 'R' && header->Signature[1] == 'S' &&
        header->Signature[2] == 'D' && header->Signature[3] == 'T') {
        // RSDT: 32-bit pointers
        entries = (header->Length - sizeof(acpi_table_header_t)) / sizeof(uint32_t);

        printd(DEBUG_ACPI, "ACPI: Parsing RSDT with %u entries\n", entries);

        // Iterate through entries without taking the address of packed members
        for (uint32_t i = 0; i < entries; i++) {
            tablePointer = *(uint32_t*)((uint8_t*)RootSDT + sizeof(acpi_table_header_t) + (i * sizeof(uint32_t)));
            acpi_table_header_t* table = (acpi_table_header_t*)(uintptr_t)tablePointer;

            if (!strncmp(table->Signature, tableSignature, 4)) {
                printd(DEBUG_ACPI | DEBUG_DETAILED, "ACPI: Table '%s' found at 0x%08x\n", tableSignature, tablePointer);
                return (acpiFADT_t*)table;
            }
        }
    } else if (header->Signature[0] == 'X' && header->Signature[1] == 'S' &&
               header->Signature[2] == 'D' && header->Signature[3] == 'T') {
        // XSDT: 64-bit pointers
        entries = (header->Length - sizeof(acpi_table_header_t)) / sizeof(uint64_t);

        printd(DEBUG_ACPI, "ACPI: Parsing XSDT with %u entries\n", entries);

        // Iterate through entries without taking the address of packed members
        for (uint32_t i = 0; i < entries; i++) {
            tablePointer = *(uint64_t*)((uint8_t*)RootSDT + sizeof(acpi_table_header_t) + (i * sizeof(uint64_t)));
			printd(DEBUG_ACPI, "ACPI: Looking for %s at table pointed to by index %u at 0x%016lx\n", tableSignature, i, tablePointer);
            acpi_table_header_t* table = (acpi_table_header_t*)(uintptr_t)tablePointer;

            if (!strncmp(table->Signature, tableSignature, 4)) {
                printd(DEBUG_ACPI, "ACPI: Table '%s' found at 0x%016x\n", tableSignature, (unsigned long long)tablePointer);
                return (acpiFADT_t*)table;
            }
        }
    } else {
        printd(DEBUG_ACPI, "ACPI: Unknown root SDT signature: %c%c%c%c\n",
               header->Signature[0], header->Signature[1], header->Signature[2], header->Signature[3]);
        return NULL;
    }

    // Table not found
    printd(DEBUG_ACPI, "ACPI: Table '%s' not found in root SDT\n", tableSignature);
    return NULL;
}

void acpiFindTables() {
    acpiRSDPHeader_t* rsdpTable;
    void* rootSDT = NULL; // Supports both RSDT and XSDT
    acpiFADT_t* fadtSDP;
	uint16_t* ebdaPtr = (uint16_t*)0x40E;
	uint16_t* edbaSize = (uint16_t*)0x410;
	uintptr_t rsdpBaseAddress = 0xFFFFFFFFFFFFFFFF;

	if (kLimineRSDP==0)
	{
		printd(DEBUG_ACPI, "ACPI: Looking for ACPI tables\n");

		paging_map_pages((pt_entry_t*)kKernelPML4v, 0x0, 0x0, 1, PAGE_PRESENT);

		printd(DEBUG_ACPI, "ACPI: EBDA is at 0x%04x for 0x%04x bytes\n",*ebdaPtr, *edbaSize);

		// Search in the EBDA
		if (ebdaPtr && *ebdaPtr != 0) {
			uintptr_t ebdaAddress = (uintptr_t)(*ebdaPtr) * 16; // EBDA address in paragraphs
			paging_map_pages((pt_entry_t*)kKernelPML4v, ebdaAddress, ebdaAddress, (0x60400 / PAGE_SIZE) + 1, PAGE_PRESENT);
			rsdpBaseAddress = doRSDPSearch(ebdaAddress, 0x603ff);
			//paging_map_pages((pt_entry_t*)kKernelPML4v, ebdaAddress, ebdaAddress, (0x10000 / PAGE_SIZE) + 1, 0);
		}

		// Fallback search in high memory
		if (rsdpBaseAddress == 0xFFFFFFFFFFFFFFFF) {
			paging_map_pages((pt_entry_t*)kKernelPML4v, 0x90000, 0x90000, (0x70000 / PAGE_SIZE) + 1, PAGE_PRESENT);
			rsdpBaseAddress = doRSDPSearch(0x90000, 0x6FFFF);
			//paging_map_pages((pt_entry_t*)kKernelPML4v, 0xE0000, 0xE0000, (0x20000 / PAGE_SIZE) + 1, 0);
		}

		paging_map_pages((pt_entry_t*)kKernelPML4v, 0x0, 0x0, 1, 0);

		if ( (rsdpBaseAddress == 0xFFFFFFFFFFFFFFFF) | (rsdpBaseAddress == 0x00000000FFFFFFFF) ) {
			printd(DEBUG_ACPI, "ACPI: RSDP table not found\n");
			return;
		}

	}
	else
	{
		rsdpBaseAddress = kLimineRSDP;
		printd(DEBUG_ACPI, "ACPI: Limine passed PCI base address of 0x%016lx, we'll use that\n",kLimineRSDP);
	}

	paging_map_pages((pt_entry_t*)kKernelPML4v, rsdpBaseAddress, rsdpBaseAddress, (0x20000 / PAGE_SIZE) + 1, PAGE_PRESENT);
    rsdpTable = (acpiRSDPHeader_t*)rsdpBaseAddress;
    printd(DEBUG_ACPI, "ACPI: RSDP found at 0x%016x\n", (unsigned long long)rsdpBaseAddress);

	printd(DEBUG_ACPI, "ACPI: RSDP revision = 0x%02x, OEMID = %c%c%c%c%c%c\n", rsdpTable->firstPart.Revision, rsdpTable->firstPart.OEMID[0], rsdpTable->firstPart.OEMID[1], rsdpTable->firstPart.OEMID[2], rsdpTable->firstPart.OEMID[3], rsdpTable->firstPart.OEMID[4], rsdpTable->firstPart.OEMID[5]);

    // Determine root SDT (RSDT or XSDT)
    if (rsdpTable->firstPart.Revision >= 2 && rsdpTable->XsdtAddress) {
        // Use XSDT for ACPI v2.0+
        rootSDT = (void*)(uintptr_t)rsdpTable->XsdtAddress;
		paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)rootSDT & 0xFFFFFFFFFFF00000, (uintptr_t)rootSDT & 0xFFFFFFFFFFF00000, (0x100000 / PAGE_SIZE) + 1, PAGE_PRESENT);
        printd(DEBUG_ACPI, "ACPI: Using XSDT at 0x%016x\n", (unsigned long long)rsdpTable->XsdtAddress);
    } 
	else if (rsdpTable->firstPart.RsdtAddress) {
        // Use RSDT for ACPI v1.0
        rootSDT = (void*)(uintptr_t)rsdpTable->firstPart.RsdtAddress;
		paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)rootSDT & 0xFFFFFFFFFFFFFFFF, (uintptr_t)rootSDT & 0xFFFFFFFFFFFFFFFF, (0x20000 / PAGE_SIZE) + 1, PAGE_PRESENT);
        printd(DEBUG_ACPI, "ACPI: Using RSDT at 0x%08x\n", rsdpTable->firstPart.RsdtAddress);
    } else {
        printd(DEBUG_ACPI, "ACPI: No valid RSDT or XSDT found\n");
        return;
    }

    // Locate FADT
    fadtSDP = (acpiFADT_t*)acpiFindTable(rootSDT, "FACP");
    if (fadtSDP) {
        printd(DEBUG_ACPI, "ACPI: FACP table found at 0x%08x\n", (uintptr_t)fadtSDP);
        kAcpiFADT = fadtSDP;   // kept for S5 (see the global's comment)
    } else {
        printd(DEBUG_ACPI, "ACPI: FACP table not found\n");
        return;
    }

    // Locate DSDT from FADT
    if (fadtSDP->Dsdt) {
        acpi_table_header_t* dsdtTable = (acpi_table_header_t*)(uintptr_t)fadtSDP->Dsdt;
        printd(DEBUG_ACPI, "ACPI: DSDT table found at 0x%08x\n", (uintptr_t)dsdtTable);
    } else {
        printd(DEBUG_ACPI, "ACPI: DSDT table not found in FADT\n");
    }

    // Locate MCFG
    acpi_mcfg_table_t* mcfgTable = (acpi_mcfg_table_t*)acpiFindTable(rootSDT, "MCFG");
    if (mcfgTable) {
        printd(DEBUG_ACPI, "ACPI: MCFG table found at 0x%08x\n", (uintptr_t)mcfgTable);
		parseMCFG((uintptr_t)mcfgTable);
    } else {
        printd(DEBUG_ACPI, "MCFG table not found\n");
    }

	//Locate MADT (Multiple APIC Description Table)
	acpi_table_header_t *madtHeader = (void*)acpiFindTable(rootSDT, "APIC");

	if (madtHeader)
	{
        printd(DEBUG_ACPI, "ACPI: MADT (APIC) table found at %p\n", madtHeader);
		uintptr_t detail = sizeof(acpi_table_header_t);
		detail += (uintptr_t)madtHeader + 8;

		// Walk EVERY entry (no early break): we want the first IO APIC (type
		// 1) AND every Interrupt Source Override (type 2). The overrides are
		// what tell us an ISA IRQ is wired to a different IOAPIC input than
		// its own number — most importantly the PIT (IRQ 0 -> GSI 2 on
		// VirtualBox and real hardware). See acpi.h.
		while (detail < (uintptr_t)madtHeader + madtHeader->Length)
		{
			uint8_t entryType = *(uint8_t*)detail;
			uint8_t entryLength = *(uint8_t*)(detail + 1);
			if (entryLength == 0)
				break;  // malformed entry — bail rather than loop forever

			if (entryType == 0x01 && kIOAPICAddress == 0)
			{
				IO_APIC_Entry *entry = (IO_APIC_Entry *)detail;
				kIOAPICAddress = entry->ioapic_addr;
		        printd(DEBUG_SMP | DEBUG_DETAILED, "ACPI: IO APIC address found in MADT table, value = 0x%08x\n", kIOAPICAddress);
			}
			else if (entryType == 0x02)
			{
				Interrupt_Source_Override_Entry *iso = (Interrupt_Source_Override_Entry *)detail;
				if (iso->source < 16)
				{
					kISAIrqToGSI[iso->source] = (int16_t)iso->gsi;
					kISAIrqOverrideFlags[iso->source] = iso->flags;
				}
		        printd(DEBUG_SMP | DEBUG_DETAILED, "ACPI: MADT interrupt source override: ISA IRQ %u -> GSI %u (flags 0x%04x)\n",
		               iso->source, iso->gsi, iso->flags);
			}

			detail += entryLength;
		}
	}
}

// ── S5 soft-off: the machine actually going dark (2026-08-21) ───────────────
// Contract and rationale in acpi.h. Four numbers are all a poweroff needs:
// the PM1a/PM1b control ports, and the SLP_TYP value each wants. SLP_EN then
// says "do it".
//
// WHY THIS IS A BYTE SCAN AND NOT AN INTERPRETER. \_S5 is an AML object in
// the DSDT, and evaluating AML properly means a name space, a parser, and a
// method interpreter — a project, not a function. But \_S5 is always the same
// shape: a NAME holding a PACKAGE of small integers, with no method call and
// nothing to evaluate. So we find the name and read the package. Every
// hobbyist OS and more than one commercial bootloader has done exactly this,
// and it works on the firmware that exists. THE COST IS NAMED: if a machine
// ever hides \_S5 behind something that must be executed, this finds nothing
// and says so — which is why acpi_poweroff() reports failure instead of
// pretending, and why the caller keeps its fallback.

#define ACPI_SLP_EN       (1u << 13)   // PM1_CNT: "perform the transition"
#define ACPI_SCI_EN       (1u << 0)    // PM1_CNT: ACPI mode is on
#define ACPI_SLP_TYP_MASK (0x7u << 10) // PM1_CNT: which sleep state (S5 lives here)

static bool     s_s5Ready;            // did the boot-time parse succeed?
static uint16_t s_pm1aControlPort;
static uint16_t s_pm1bControlPort;    // 0 = this machine has only PM1a
static uint16_t s_slpTypA;            // already shifted into position
static uint16_t s_slpTypB;
static uint32_t s_smiCommandPort;     // 0 = no SMI handshake needed
static uint8_t  s_acpiEnableValue;

// A control block can be described twice: the ACPI 1.0 32-bit port, and the
// 2.0+ generic address. Prefer the generic one when it is present AND says
// SystemIO — a PM1 block in memory space is legal on paper and has never been
// seen in the wild, so if one ever turns up we take the legacy port instead
// of inventing an MMIO path nobody can test.
// BY VALUE, not by pointer: the FADT is __packed, so taking the address of a
// member hands out a possibly-unaligned pointer — which gcc refuses outright
// (-Werror=address-of-packed-member), and rightly, since dereferencing one is
// undefined. Copying the twelve bytes out is free and correct.
static uint16_t acpi_control_port(GenericAddressStructure_t x, uint32_t legacy)
{
	if (x.Address != 0 && x.AddressSpace == SystemIO)
		return (uint16_t)x.Address;
	return (uint16_t)legacy;
}

void acpi_prepare_s5(void)
{
	if (kAcpiFADT == NULL)
	{
		printd(DEBUG_ACPI, "ACPI: no FADT — S5 poweroff unavailable\n");
		return;
	}

	s_pm1aControlPort = acpi_control_port(kAcpiFADT->X_PM1aControlBlock,
	                                      kAcpiFADT->PM1aControlBlock);
	s_pm1bControlPort = acpi_control_port(kAcpiFADT->X_PM1bControlBlock,
	                                      kAcpiFADT->PM1bControlBlock);
	s_smiCommandPort  = kAcpiFADT->SMI_CommandPort;
	s_acpiEnableValue = kAcpiFADT->AcpiEnable;

	if (s_pm1aControlPort == 0)
	{
		printd(DEBUG_ACPI, "ACPI: FADT names no PM1a control block — S5 unavailable\n");
		return;
	}

	// The DSDT's address is in the FADT twice: the 32-bit legacy field, and
	// the 64-bit X_ field added in ACPI 2.0.
	//
	// LEGACY FIRST, X_ AS THE FALLBACK — and be honest about why, because
	// the first version of this comment was not. The first draft preferred
	// X_Dsdt, read 0x0000200100000000 (thirty-two terabytes) under QEMU,
	// mapped it, and hung; this ordering was then justified as "what the
	// hardware said". It was NOT what the hardware said. It was the unpacked
	// GenericAddressStructure_t four bytes upstream (acpi.h) shifting every
	// late FADT field — the same bug that hid the P5's X_PM1a block — and
	// with the struct packed, X_Dsdt under QEMU reads the same 0x7ffe0040
	// the legacy field does. (Corrected 2026-08-22; the SUCCESSION fingerprint
	// row is the one to believe.)
	//
	// The ordering stays, on its real merits: the 32-bit field has been
	// correct on every machine since 1996 and cannot address above 4GB,
	// which no firmware does for the DSDT anyway, so when it is non-zero it
	// is the safer of two answers that agree. X_Dsdt is for the modern UEFI
	// firmware that zeroes the legacy field — the same machines whose PM1a
	// block only exists in the X_ form (acpi_control_port above prefers X_
	// for that reason; here the risk runs the other way, a 64-bit garbage
	// address is a hang and a 32-bit one at worst a bad signature check).
	// Fall back only when the legacy field is genuinely empty AND the table
	// is long enough to hold the extended one.
	uintptr_t dsdtPhys = (uintptr_t)kAcpiFADT->Dsdt;
	if (dsdtPhys == 0 && kAcpiFADT->h.Length >= 148)
		dsdtPhys = (uintptr_t)kAcpiFADT->X_Dsdt;
	if (dsdtPhys == 0)
	{
		printd(DEBUG_ACPI, "ACPI: FADT names no DSDT — S5 unavailable\n");
		return;
	}

	// MAP IT BEFORE READING. acpiFindTables identity-maps the regions it
	// walks, but it only ever PRINTED the DSDT's address — the table's bytes
	// have never been touched, so nothing has mapped them. One page first to
	// reach the header's Length, then the whole table.
	// Map it through the HHDM, NOT identity. acpiFindTables identity-maps the
	// low regions it walks (EBDA, 0x90000, the RSDP window — all under 1MB),
	// and copying that habit up here would put a kernel mapping at a
	// LOWER-HALF virtual address wherever the firmware happened to put the
	// DSDT — which is user address space, in the page tables the kernel task
	// itself runs on. The upper-half alias costs the same one call and lands
	// where every other physical-memory read in this kernel lands.
	// paging_hhdm_map_range, not paging_map_pages: the HHDM is LAZY here (it
	// maps what the allocator owns and deliberately leaves the rest as a
	// use-after-free tripwire), and it has its own maintenance entry point
	// that knows the boundary rules. Reaching around it with a raw mapping
	// call is how you write a PTE the rest of the system does not expect —
	// and it is exactly what wedged the boot on the first attempt.
	// `length` starts at the header's size and grows to the table's own — it
	// is declared HERE, before the first mapping, because every exit below
	// jumps to s5_done to hand the mapping back, and that label needs to know
	// how much was mapped no matter which exit got there.
	uint32_t length = sizeof(acpi_table_header_t);

	paging_hhdm_map_range(dsdtPhys, length);
	RELOAD_CR3;   // CLAUDE.md rule 4 — a fresh PTE nobody flushed is a fresh PTE nobody sees
	acpi_table_header_t *dsdt = (acpi_table_header_t *)(dsdtPhys | kHHDMOffset);
	if (dsdt->Signature[0] != 'D' || dsdt->Signature[1] != 'S' ||
	    dsdt->Signature[2] != 'D' || dsdt->Signature[3] != 'T')
	{
		printd(DEBUG_ACPI, "ACPI: DSDT at 0x%016lx has signature '%c%c%c%c', not DSDT — S5 unavailable\n",
		       dsdtPhys, dsdt->Signature[0], dsdt->Signature[1],
		       dsdt->Signature[2], dsdt->Signature[3]);
		goto s5_done;
	}

	if (dsdt->Length < sizeof(acpi_table_header_t) || dsdt->Length > 0x400000)
	{
		printd(DEBUG_ACPI, "ACPI: DSDT length %u is not credible — S5 unavailable\n",
		       dsdt->Length);
		goto s5_done;
	}
	length = dsdt->Length;
	paging_hhdm_map_range(dsdtPhys, length);
	RELOAD_CR3;

	// ── The scan ────────────────────────────────────────────────────────────
	// What we are looking for, byte by byte:
	//
	//   0x08              NameOp — "a name follows"
	//   '_' 'S' '5' '_'   the name (optionally prefixed by '\' for root-scope)
	//   0x12              PackageOp — "the value is a package"
	//   <PkgLength>       1..4 bytes; the top two bits of the FIRST byte say
	//                     how many FOLLOW it (that is the whole encoding)
	//   <NumElements>     one byte
	//   <element>         0x0A <byte> for a small integer, or 0x00/0x01 which
	//                     ARE the values zero and one with no payload
	//
	// The first two elements are SLP_TYPa and SLP_TYPb. Everything after them
	// is somebody else's business.
	const uint8_t *base = (const uint8_t *)dsdt;
	const uint8_t *end  = base + length;
	const uint8_t *s5   = NULL;

	for (const uint8_t *p = base + sizeof(acpi_table_header_t); p + 4 < end; p++)
	{
		if (p[0] == '_' && p[1] == 'S' && p[2] == '5' && p[3] == '_')
		{
			s5 = p;
			break;
		}
	}
	if (s5 == NULL)
	{
		printd(DEBUG_ACPI, "ACPI: no \\_S5 in the DSDT — S5 poweroff unavailable\n");
		goto s5_done;
	}

	// Confirm the shape around the name before believing it: a NameOp before
	// (possibly with a root-scope backslash between), a PackageOp after. The
	// four letters could otherwise be any string that happens to sit in the
	// table, and acting on a coincidence is how you write garbage to a power
	// register.
	bool namedHere = (s5 > base && s5[-1] == 0x08) ||
	                 (s5 > base + 1 && s5[-2] == 0x08 && s5[-1] == '\\');
	if (!namedHere || s5 + 4 >= end || s5[4] != 0x12)
	{
		printd(DEBUG_ACPI, "ACPI: found '_S5_' but not as a named package — S5 unavailable\n");
		goto s5_done;
	}

	const uint8_t *p = s5 + 5;                 // past the name and the PackageOp
	if (p >= end)
		goto s5_done;
	p += ((*p & 0xC0) >> 6) + 2;               // PkgLength's trailing bytes, then NumElements
	if (p >= end)
	{
		printd(DEBUG_ACPI, "ACPI: \\_S5 package runs off the end of the DSDT — S5 unavailable\n");
		goto s5_done;
	}

	if (*p == 0x0A)                            // BytePrefix: a value follows
		p++;
	if (p >= end)
		goto s5_done;
	s_slpTypA = (uint16_t)(*p << 10);
	p++;

	if (p < end)
	{
		if (*p == 0x0A)
			p++;
		if (p < end)
			s_slpTypB = (uint16_t)(*p << 10);
	}

	s_s5Ready = true;
	printd(DEBUG_BOOT, "ACPI: S5 ready — PM1a 0x%04x SLP_TYPa %u, PM1b 0x%04x SLP_TYPb %u\n",
	       s_pm1aControlPort, (unsigned)(s_slpTypA >> 10),
	       s_pm1bControlPort, (unsigned)(s_slpTypB >> 10));

s5_done:
	// GIVE THE MEMORY BACK. Everything this function came for is two small
	// integers, now sitting in file-scope statics — the table itself is of no
	// further use, and leaving it mapped is not free:
	//
	// The HHDM here is LAZY on purpose. Physical memory is mapped exactly
	// while the allocator considers it allocated, and everything else is
	// deliberately absent so that touching it page-faults with a
	// "use-after-free or wild pointer?" panic (MEMORY.md's tripwire). A
	// permanent PRESENT|WRITE window over the DSDT is a hole punched in that
	// tripwire for the whole life of the machine: a wild pointer landing
	// there would scribble silently instead of announcing itself.
	//
	// The allocator does NOT own this memory — it claims LIMINE_MEMMAP_USABLE
	// only (allocator.c), and the DSDT lives in ACPI-reclaimable or reserved
	// — which is what makes this unmap SAFE (nothing else can hold a live
	// HHDM mapping of these pages) and is also why the unmapped window was
	// never a candidate for the P5's random shutdown deaths of 2026-08-21:
	// no allocation could ever have been handed out under it. That one was
	// the hangup sweep shooting the shutdown task (shutdown.h), and it is
	// fixed. Either way the mapping has no business outliving the read.
	paging_hhdm_unmap_range(dsdtPhys, length);
}

bool acpi_poweroff(void)
{
	// EVERY line in here goes to the GLASS, not to printd. This function runs
	// after the log daemon has retired, on a machine that may have no serial
	// port at all — the screen is the only witness left, and a diagnostic
	// nobody can read is not a diagnostic.
	if (!s_s5Ready)
	{
		printf("  ACPI S5: not available (no FADT, no \\_S5, or no PM1a block)\n");
		return false;
	}

	printf("  ACPI S5: PM1a 0x%04x TYPa %u", s_pm1aControlPort,
	       (unsigned)(s_slpTypA >> 10));
	if (s_pm1bControlPort != 0)
		printf(", PM1b 0x%04x TYPb %u", s_pm1bControlPort,
		       (unsigned)(s_slpTypB >> 10));
	printf(", SCI_EN %u\n", (unsigned)(inw(s_pm1aControlPort) & ACPI_SCI_EN));

	// ACPI mode has to be ON before the PM1 registers mean anything. Under
	// UEFI it always already is (the firmware never ran in legacy mode), and
	// on anything with a SMI command port we ask once and wait a bounded
	// while — an unbounded poll here would hang the descent on firmware that
	// simply never answers.
	if (!(inw(s_pm1aControlPort) & ACPI_SCI_EN) && s_smiCommandPort != 0)
	{
		outb((uint16_t)s_smiCommandPort, s_acpiEnableValue);
		for (int spins = 0; spins < 300000; spins++)
		{
			if (inw(s_pm1aControlPort) & ACPI_SCI_EN)
				break;
			__builtin_ia32_pause();
		}
	}

	// READ, MODIFY, WRITE — never a bare write, and this is the bug the first
	// version shipped with. PM1a_CNT is not a command register, it is a
	// CONTROL register full of live bits, and bit 0 of it is SCI_EN — "ACPI
	// mode is on". Writing a bare (SLP_TYP | SLP_EN) therefore asks the
	// machine to enter S5 *and* to leave ACPI mode in the same 16-bit store,
	// which is not a request any firmware is obliged to honor, and most
	// don't. Preserve what is there, replace only the SLP_TYP field.
	uint16_t pm1a = inw(s_pm1aControlPort);
	pm1a &= (uint16_t)~ACPI_SLP_TYP_MASK;
	pm1a |= (uint16_t)(s_slpTypA | ACPI_SLP_EN);

	printf("  ACPI S5: PM1a port 0x%04x <- 0x%04x\n", s_pm1aControlPort, pm1a);
	outw(s_pm1aControlPort, pm1a);

	if (s_pm1bControlPort != 0)
	{
		uint16_t pm1b = inw(s_pm1bControlPort);
		pm1b &= (uint16_t)~ACPI_SLP_TYP_MASK;
		pm1b |= (uint16_t)(s_slpTypB | ACPI_SLP_EN);
		printf("  ACPI S5: PM1b port 0x%04x <- 0x%04x\n", s_pm1bControlPort, pm1b);
		outw(s_pm1bControlPort, pm1b);
	}

	// Reached only if the transition did not take. Say so ON THE GLASS: by
	// this point logd is retired and a machine with no UART (the P5) has no
	// other channel left — a printd here would die in the rings with the
	// machine that needed to report it. Hard-won, 2026-08-21.
	printf("  ACPI S5: the machine did not take it\n");
	return true;
}
