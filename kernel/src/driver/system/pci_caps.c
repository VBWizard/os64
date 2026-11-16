// pci_caps.c — walk a PCI function's capability list (PCI 3.0 § 6.7).
//
// The status register's bit 4 says a list exists; the byte at 0x34 points
// at the first entry; an entry is a dword whose low byte is the capability
// id and whose next byte points at the following entry, 0 ending the list.
// See pci_caps.h for why nothing here is trusted.

#include "pci_caps.h"

#include <stddef.h>

#define PCI_STATUS_COMMAND   0x04
#define PCI_STATUS_HAS_CAPS  0x0010   // in the status half (the high word)
#define PCI_CAPS_POINTER     0x34
#define PCI_CAPS_FIRST       0x40     // below this is the header

#define PCI_CAP_ID_MSI       0x05
#define PCI_CAP_ID_MSIX      0x11

void pci_caps_walk(pci_caps_read_t read, void *ctx, pci_caps_t *out)
{
	*out = (pci_caps_t){0};

	// Without the status bit the byte at 0x34 means nothing, and following
	// it is how a walk wanders.
	if (((read(ctx, PCI_STATUS_COMMAND) >> 16) & PCI_STATUS_HAS_CAPS) == 0)
		return;

	// One bit a dword of configuration space: an entry seen twice is a loop.
	uint64_t visited = 0;
	// The low two bits of a pointer are reserved and read as zero on a
	// conforming device; they are masked, as the specification says to.
	uint8_t ptr = (uint8_t)(read(ctx, PCI_CAPS_POINTER) & 0xFC);

	while (ptr != 0)
	{
		uint64_t bit = (uint64_t)1 << (ptr >> 2);
		if (ptr < PCI_CAPS_FIRST || (visited & bit) != 0)
		{
			out->damaged = true;
			return;
		}
		visited |= bit;

		uint32_t entry = read(ctx, ptr);
		uint8_t  id = (uint8_t)(entry & 0xFF);
		uint16_t control = (uint16_t)(entry >> 16);   // Message Control, for the two below

		out->entry[out->count].id = id;
		out->entry[out->count].offset = ptr;
		out->count++;

		// A function has one of each; a second in a damaged list does not
		// overwrite what the first said.
		if (id == PCI_CAP_ID_MSI && !out->has_msi)
		{
			out->has_msi = true;
			// Bits 3:1 are log2 of the vectors it can ask for; 6 and 7
			// are reserved encodings, held to the largest real one.
			uint8_t log2 = (uint8_t)((control >> 1) & 0x7);
			if (log2 > 5)
				log2 = 5;
			out->msi_vectors = (uint8_t)(1u << log2);
			out->msi_64bit = (control & 0x0080) != 0;
			out->msi_masking = (control & 0x0100) != 0;
		}
		else if (id == PCI_CAP_ID_MSIX && !out->has_msix)
		{
			out->has_msix = true;
			// Bits 10:0 are the table size minus one.
			out->msix_vectors = (uint16_t)((control & 0x07FF) + 1);
		}

		ptr = (uint8_t)((entry >> 8) & 0xFC);
	}
}

uint8_t pci_caps_find(const pci_caps_t *caps, uint8_t id)
{
	for (uint8_t i = 0; i < caps->count; i++)
		if (caps->entry[i].id == id)
			return caps->entry[i].offset;
	return 0;
}

const char *pci_cap_name(uint8_t id)
{
	switch (id)
	{
		case 0x01: return "pm";          // power management
		case 0x02: return "agp";
		case 0x03: return "vpd";         // vital product data
		case 0x04: return "slot";        // slot identification
		case 0x05: return "msi";
		case 0x06: return "hotswap";     // CompactPCI hot swap
		case 0x07: return "pcix";
		case 0x08: return "ht";          // HyperTransport
		case 0x09: return "vendor";      // vendor-specific
		case 0x0A: return "debug";       // debug port
		case 0x0B: return "cpci";        // CompactPCI resource control
		case 0x0C: return "hotplug";
		case 0x0D: return "ssvid";       // bridge subsystem vendor id
		case 0x0E: return "agp8x";
		case 0x0F: return "secure";
		case 0x10: return "express";     // PCI Express
		case 0x11: return "msix";
		case 0x12: return "sata";
		case 0x13: return "af";          // advanced features
		case 0x14: return "ea";          // enhanced allocation
		case 0x15: return "fpb";         // flattening portal bridge
		default:   return NULL;
	}
}
