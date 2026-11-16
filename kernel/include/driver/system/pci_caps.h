// pci_caps.h — a PCI function's capability list, read once and kept.
//
// PURE: no kernel headers, so tools/test_pci_caps_host.sh drives it on the
// host against configuration spaces built by hand, damaged ones included.
// The list is hardware's (and on a passed-through or emulated device,
// somebody else's software's), so the walk trusts nothing in it.

#ifndef PCI_CAPS_H
#define PCI_CAPS_H

#include <stdbool.h>
#include <stdint.h>

// The list lives in configuration space from 0x40 to 0xFF and an entry is
// dword-aligned, so 48 is every entry a list can hold without visiting one
// twice: the table cannot overflow.
#define PCI_CAPS_MAX 48

typedef struct
{
	uint8_t id;      // PCI capability id (PCI 3.0 Appendix H)
	uint8_t offset;  // where the entry sits in configuration space
} pci_cap_entry_t;

typedef struct
{
	uint8_t         count;
	pci_cap_entry_t entry[PCI_CAPS_MAX];
	// The list pointed outside 0x40..0xFF or back at an entry already
	// visited. What was read before that is kept; the list is not trusted
	// past it.
	bool            damaged;

	// MSI (id 0x05), from its Message Control word.
	bool            has_msi;
	uint8_t         msi_vectors;   // 1, 2, 4, 8, 16 or 32: what it CAN ask for
	bool            msi_64bit;     // takes a 64-bit message address
	bool            msi_masking;   // has per-vector mask bits

	// MSI-X (id 0x11), from its Message Control word.
	bool            has_msix;
	uint16_t        msix_vectors;  // table size, 1..2048
} pci_caps_t;

// Reads one dword of the function's configuration space. `offset` is
// dword-aligned and below 0x100.
typedef uint32_t (*pci_caps_read_t)(void *ctx, uint8_t offset);

// Walk the list into `out`. A function whose status register says it has no
// list yields count 0 and nothing else set.
void pci_caps_walk(pci_caps_read_t read, void *ctx, pci_caps_t *out);

// The entry's offset for `id`, or 0 when the walk did not find it.
uint8_t pci_caps_find(const pci_caps_t *caps, uint8_t id);

// A short lowercase name for an id ("msi", "express"), or NULL for one this
// table does not name; the caller prints the number then.
const char *pci_cap_name(uint8_t id);

#endif
