#include "acpi.h"
#include "CONFIG.h"
#include "strcmp.h"
#include "serial_logging.h"
#include "paging.h"

uintptr_t kPCIBaseAddress=0;
void parseMCFG(uintptr_t mcfgAddress) {
    acpi_mcfg_table_t* mcfg = (acpi_mcfg_table_t*)mcfgAddress;

    // Calculate the number of entries in the MCFG table
	size_t headerSize = sizeof(acpi_mcfg_table_t);
    size_t entryCount = (mcfg->length - headerSize) / sizeof(acpi_mcfg_entry_t);
    printd(DEBUG_ACPI, "MCFG Table has %zu entries\n", entryCount);

 // Iterate through the entries
	acpi_mcfg_entry_t* entries = (acpi_mcfg_entry_t*)((uint8_t*)mcfg + headerSize);
    for (size_t i = 0; i < entryCount; i++) {
        acpi_mcfg_entry_t* entry = &entries[i];
        printd(DEBUG_ACPI, "  Entry %u:\n", i);
        printd(DEBUG_ACPI, "    Base Address: 0x%016x\n", (unsigned long long)entry->base_address);
        printd(DEBUG_ACPI, "    Segment Group: %u\n", entry->segment_group);
        printd(DEBUG_ACPI, "    Start Bus: %u\n", entry->start_bus_number);
        printd(DEBUG_ACPI, "    End Bus: %u\n", entry->end_bus_number);
        printd(DEBUG_ACPI, "    Reserved: 0x%08x\n", entry->reserved);
	}
    for (size_t i = 0; i < entryCount; i++) {
        // Access each MCFG entry
        acpi_mcfg_entry_t* entry = (acpi_mcfg_entry_t*)&entries[i];
        printd(DEBUG_ACPI, "Entry %zu: BaseAddress=0x%016llx, SegmentGroup=%u, StartBus=%u, EndBus=%u\n",
               i, (unsigned long long)entry->base_address, entry->segment_group,
               entry->start_bus_number, entry->end_bus_number);

        // Example: Use the BaseAddress for Segment 0
        if (entry->segment_group == 0) {
			kPCIBaseAddress = entry->base_address;
            printd(DEBUG_BOOT, "ACPI: *Found PCI Segment 0 Base Address: 0x%08x\n", (unsigned long long)entry->base_address);
        }
    }
}

uintptr_t doRSDPSearch(uintptr_t from, int count) {
    uint8_t* fromPtr = (uint8_t*)from; // Cast base address to byte pointer

    printd(DEBUG_ACPI, "doRSDPSearch: Scanning range 0x%08x - 0x%08x\n",
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

    printd(DEBUG_ACPI, "Root SDT signature: %c%c%c%c\n",
           header->Signature[0], header->Signature[1], header->Signature[2], header->Signature[3]);

    // Determine whether we are using RSDT or XSDT
    if (header->Signature[0] == 'R' && header->Signature[1] == 'S' &&
        header->Signature[2] == 'D' && header->Signature[3] == 'T') {
        // RSDT: 32-bit pointers
        entries = (header->Length - sizeof(acpi_table_header_t)) / sizeof(uint32_t);

        printd(DEBUG_ACPI, "Parsing RSDT with %u entries\n", entries);

        // Iterate through entries without taking the address of packed members
        for (uint32_t i = 0; i < entries; i++) {
            tablePointer = *(uint32_t*)((uint8_t*)RootSDT + sizeof(acpi_table_header_t) + (i * sizeof(uint32_t)));
            acpi_table_header_t* table = (acpi_table_header_t*)(uintptr_t)tablePointer;

            if (!strncmp(table->Signature, tableSignature, 4)) {
                printd(DEBUG_ACPI, "Table '%s' found at 0x%08x\n", tableSignature, tablePointer);
                return (acpiFADT_t*)table;
            }
        }
    } else if (header->Signature[0] == 'X' && header->Signature[1] == 'S' &&
               header->Signature[2] == 'D' && header->Signature[3] == 'T') {
        // XSDT: 64-bit pointers
        entries = (header->Length - sizeof(acpi_table_header_t)) / sizeof(uint64_t);

        printd(DEBUG_ACPI, "Parsing XSDT with %u entries\n", entries);

        // Iterate through entries without taking the address of packed members
        for (uint32_t i = 0; i < entries; i++) {
            tablePointer = *(uint64_t*)((uint8_t*)RootSDT + sizeof(acpi_table_header_t) + (i * sizeof(uint64_t)));
            acpi_table_header_t* table = (acpi_table_header_t*)(uintptr_t)tablePointer;

            if (!strncmp(table->Signature, tableSignature, 4)) {
                printd(DEBUG_ACPI, "Table '%s' found at 0x%016x\n", tableSignature, (unsigned long long)tablePointer);
                return (acpiFADT_t*)table;
            }
        }
    } else {
        printd(DEBUG_ACPI, "Unknown root SDT signature: %c%c%c%c\n",
               header->Signature[0], header->Signature[1], header->Signature[2], header->Signature[3]);
        return NULL;
    }

    // Table not found
    printd(DEBUG_ACPI, "Table '%s' not found in root SDT\n", tableSignature);
    return NULL;
}

void acpiFindTables() {
    acpiRSDPHeader_t* rsdpTable;
    void* rootSDT = NULL; // Supports both RSDT and XSDT
    acpiFADT_t* fadtSDP;

    printd(DEBUG_ACPI, "acpiFindTables: Looking for ACPI tables\n");

    uint16_t* ebdaPtr = (uint16_t*)0x40E;
    uintptr_t rsdpBaseAddress = 0xFFFFFFFF;

	paging_map_pages((pt_entry_t*)kKernelPML4v, 0x0, 0x0, 1, PAGE_PRESENT);

    // Search in the EBDA
    if (ebdaPtr && *ebdaPtr != 0) {
        uintptr_t ebdaAddress = (uintptr_t)(*ebdaPtr) * 16; // EBDA address in paragraphs
		paging_map_pages((pt_entry_t*)kKernelPML4v, ebdaAddress, ebdaAddress, (0x10000 / PAGE_SIZE) + 1, PAGE_PRESENT);
        rsdpBaseAddress = doRSDPSearch(ebdaAddress, 0xFFFF);
		//paging_map_pages((pt_entry_t*)kKernelPML4v, ebdaAddress, ebdaAddress, (0x10000 / PAGE_SIZE) + 1, 0);
    }

    // Fallback search in high memory
    if (rsdpBaseAddress == 0xFFFFFFFFFFFFFFFF) {
		paging_map_pages((pt_entry_t*)kKernelPML4v, 0xE0000, 0xE0000, (0x20000 / PAGE_SIZE) + 1, PAGE_PRESENT);
        rsdpBaseAddress = doRSDPSearch(0xE0000, 0x1FFFF);
		//paging_map_pages((pt_entry_t*)kKernelPML4v, 0xE0000, 0xE0000, (0x20000 / PAGE_SIZE) + 1, 0);
    }

	paging_map_pages((pt_entry_t*)kKernelPML4v, 0x0, 0x0, 1, 0);

    if (rsdpBaseAddress == 0xFFFFFFFFFFFFFFFF) {
        printd(DEBUG_ACPI, "ACPI RSDP table not found\n");
        return;
    }

	paging_map_pages((pt_entry_t*)kKernelPML4v, rsdpBaseAddress, rsdpBaseAddress, (0x20000 / PAGE_SIZE) + 1, PAGE_PRESENT);
    rsdpTable = (acpiRSDPHeader_t*)rsdpBaseAddress;
    printd(DEBUG_ACPI, "ACPI RSDP found at 0x%016x\n", (unsigned long long)rsdpBaseAddress);

    // Determine root SDT (RSDT or XSDT)
    if (rsdpTable->firstPart.Revision >= 2 && rsdpTable->XsdtAddress) {
        // Use XSDT for ACPI v2.0+
        rootSDT = (void*)(uintptr_t)rsdpTable->XsdtAddress;
		paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)rootSDT, (uintptr_t)rootSDT, (0x20000 / PAGE_SIZE) + 1, PAGE_PRESENT);
        acpiXSDT_t* xsdt = (acpiXSDT_t*)rootSDT;
        printd(DEBUG_ACPI, "Using XSDT at 0x%016x\n", (unsigned long long)rsdpTable->XsdtAddress);
    } else if (rsdpTable->firstPart.RsdtAddress) {
        // Use RSDT for ACPI v1.0
        rootSDT = (void*)(uintptr_t)rsdpTable->firstPart.RsdtAddress;
		paging_map_pages((pt_entry_t*)kKernelPML4v, (uintptr_t)rootSDT, (uintptr_t)rootSDT, (0x20000 / PAGE_SIZE) + 1, PAGE_PRESENT);
        acpiRSDT_t* rsdt = (acpiRSDT_t*)rootSDT;
        printd(DEBUG_ACPI, "Using RSDT at 0x%08x\n", rsdpTable->firstPart.RsdtAddress);
    } else {
        printd(DEBUG_ACPI, "No valid RSDT or XSDT found\n");
        return;
    }

    // Locate FADT
    fadtSDP = (acpiFADT_t*)acpiFindTable(rootSDT, "FACP");
    if (fadtSDP) {
        printd(DEBUG_ACPI, "FACP table found at 0x%08x\n", (uintptr_t)fadtSDP);
    } else {
        printd(DEBUG_ACPI, "FACP table not found\n");
        return;
    }

    // Locate DSDT from FADT
    if (fadtSDP->Dsdt) {
        acpi_table_header_t* dsdtTable = (acpi_table_header_t*)(uintptr_t)fadtSDP->Dsdt;
        printd(DEBUG_ACPI, "DSDT table found at 0x%08x\n", (uintptr_t)dsdtTable);
    } else {
        printd(DEBUG_ACPI, "DSDT table not found in FADT\n");
    }

    // Locate MCFG
    acpi_mcfg_table_t* mcfgTable = (acpi_mcfg_table_t*)acpiFindTable(rootSDT, "MCFG");
    if (mcfgTable) {
        printd(DEBUG_ACPI, "MCFG table found at 0x%08x\n", (uintptr_t)mcfgTable);
		parseMCFG((uintptr_t)mcfgTable);
    } else {
        printd(DEBUG_ACPI, "MCFG table not found\n");
    }
}
