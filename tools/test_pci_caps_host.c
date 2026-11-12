// test_pci_caps_host.c — drive kernel/src/driver/system/pci_caps.c on the host.
//
// A configuration space here is 256 bytes on the heap, and `read` refuses
// (and counts) any offset that is unaligned or was not asked for through
// the walk's own rules, so a walk that follows a bad pointer is caught
// twice: by the count, and by ASan if it leaves the block.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pci_caps.h"

static int failures;
static int reads;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t cfg_read(void *ctx, uint8_t offset)
{
	uint8_t *cfg = ctx;
	uint32_t v;
	reads++;
	CHECK((offset & 3) == 0, "unaligned read at 0x%02x", offset);
	memcpy(&v, cfg + offset, 4);   // offset <= 0xFC: stays inside the block
	return v;
}

static uint8_t *cfg_new(void)
{
	uint8_t *cfg = calloc(1, 256);
	if (!cfg) { perror("calloc"); exit(2); }
	cfg[0x06] = 0x10;              // status: capability list present
	return cfg;
}

// Lay an entry down: id, next pointer, and the 16-bit word that follows
// (Message Control for MSI and MSI-X).
static void cfg_cap(uint8_t *cfg, uint8_t at, uint8_t id, uint8_t next, uint16_t control)
{
	cfg[at] = id;
	cfg[at + 1] = next;
	cfg[at + 2] = (uint8_t)control;
	cfg[at + 3] = (uint8_t)(control >> 8);
}

static void test_no_list(void)
{
	uint8_t *cfg = cfg_new();
	pci_caps_t caps;
	cfg[0x06] = 0;                 // status says no list...
	cfg[0x34] = 0x50;              // ...whatever 0x34 holds
	cfg_cap(cfg, 0x50, 0x05, 0, 0);
	reads = 0;
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.count == 0 && !caps.damaged && !caps.has_msi, "a function with no list reported one");
	CHECK(reads == 1, "no list: read %d dwords, wanted the status alone", reads);
	free(cfg);
}

static void test_empty_list(void)
{
	uint8_t *cfg = cfg_new();
	pci_caps_t caps;
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.count == 0 && !caps.damaged, "an empty list: count %u damaged %d", caps.count, caps.damaged);
	free(cfg);
}

// The shape an AMD xHCI controller has: power management, MSI, PCI Express,
// MSI-X.
static void test_ordinary(void)
{
	uint8_t *cfg = cfg_new();
	pci_caps_t caps;
	cfg[0x34] = 0x50;
	cfg_cap(cfg, 0x50, 0x01, 0x64, 0);
	cfg_cap(cfg, 0x64, 0x10, 0xA0, 0);
	cfg_cap(cfg, 0xA0, 0x05, 0xC0, 0x0086);   // 8 vectors, 64-bit, no masking
	cfg_cap(cfg, 0xC0, 0x11, 0x00, 0x0007);   // table of 8
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.count == 4 && !caps.damaged, "ordinary: count %u damaged %d", caps.count, caps.damaged);
	CHECK(caps.entry[0].id == 0x01 && caps.entry[0].offset == 0x50, "ordinary: first entry");
	CHECK(caps.entry[3].id == 0x11 && caps.entry[3].offset == 0xC0, "ordinary: last entry");
	CHECK(caps.has_msi && caps.msi_vectors == 8 && caps.msi_64bit && !caps.msi_masking,
	      "ordinary: MSI vectors %u 64 %d mask %d", caps.msi_vectors, caps.msi_64bit, caps.msi_masking);
	CHECK(caps.has_msix && caps.msix_vectors == 8, "ordinary: MSI-X vectors %u", caps.msix_vectors);
	CHECK(pci_caps_find(&caps, 0x05) == 0xA0, "find MSI");
	CHECK(pci_caps_find(&caps, 0x10) == 0x64, "find express");
	CHECK(pci_caps_find(&caps, 0x12) == 0, "find something absent");
	free(cfg);
}

static void test_msi_fields(void)
{
	// Every multiple-message encoding, with the two reserved ones held.
	static const uint8_t want[8] = {1, 2, 4, 8, 16, 32, 32, 32};
	for (int log2 = 0; log2 < 8; log2++)
	{
		uint8_t *cfg = cfg_new();
		pci_caps_t caps;
		cfg[0x34] = 0x40;
		cfg_cap(cfg, 0x40, 0x05, 0, (uint16_t)(0x0100 | (log2 << 1)));   // masking, 32-bit
		pci_caps_walk(cfg_read, cfg, &caps);
		CHECK(caps.has_msi && caps.msi_vectors == want[log2], "MSI log2 %d gave %u", log2, caps.msi_vectors);
		CHECK(!caps.msi_64bit && caps.msi_masking, "MSI log2 %d: 64 %d mask %d", log2, caps.msi_64bit, caps.msi_masking);
		CHECK(!caps.has_msix, "MSI alone reported MSI-X");
		free(cfg);
	}
	// MSI-X's ends: a table of 1 and of 2048, with the bits above the size set.
	uint8_t *cfg = cfg_new();
	pci_caps_t caps;
	cfg[0x34] = 0x40;
	cfg_cap(cfg, 0x40, 0x11, 0, 0xC000);
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.has_msix && caps.msix_vectors == 1 && !caps.has_msi, "MSI-X of 1 gave %u", caps.msix_vectors);
	cfg_cap(cfg, 0x40, 0x11, 0, 0xC7FF);
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.msix_vectors == 2048, "MSI-X of 2048 gave %u", caps.msix_vectors);
	free(cfg);
}

static void test_reserved_pointer_bits(void)
{
	uint8_t *cfg = cfg_new();
	pci_caps_t caps;
	cfg[0x34] = 0x53;                          // reserved low bits set
	cfg_cap(cfg, 0x50, 0x01, 0x62, 0);         // and here
	cfg_cap(cfg, 0x60, 0x09, 0x00, 0);
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.count == 2 && !caps.damaged, "reserved bits: count %u damaged %d", caps.count, caps.damaged);
	CHECK(caps.entry[1].offset == 0x60, "reserved bits: second entry at 0x%02x", caps.entry[1].offset);
	free(cfg);
}

static void test_damage(void)
{
	pci_caps_t caps;

	// A first pointer into the header.
	uint8_t *cfg = cfg_new();
	cfg[0x34] = 0x10;
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.damaged && caps.count == 0, "pointer into the header: damaged %d count %u", caps.damaged, caps.count);
	free(cfg);

	// A later pointer into the header: what came before is kept.
	cfg = cfg_new();
	cfg[0x34] = 0x40;
	cfg_cap(cfg, 0x40, 0x05, 0x3C, 0x0080);
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.damaged && caps.count == 1 && caps.has_msi, "next into the header: damaged %d count %u", caps.damaged, caps.count);
	free(cfg);

	// An entry that points at itself.
	cfg = cfg_new();
	cfg[0x34] = 0x40;
	cfg_cap(cfg, 0x40, 0x01, 0x40, 0);
	reads = 0;
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.damaged && caps.count == 1, "self loop: damaged %d count %u", caps.damaged, caps.count);
	CHECK(reads == 3, "self loop: %d reads", reads);
	free(cfg);

	// A loop of three.
	cfg = cfg_new();
	cfg[0x34] = 0x40;
	cfg_cap(cfg, 0x40, 0x01, 0x80, 0);
	cfg_cap(cfg, 0x80, 0x10, 0xFC, 0);
	cfg_cap(cfg, 0xFC, 0x09, 0x80, 0);
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.damaged && caps.count == 3, "loop of three: damaged %d count %u", caps.damaged, caps.count);
	free(cfg);

	// A second MSI in a list does not replace the first.
	cfg = cfg_new();
	cfg[0x34] = 0x40;
	cfg_cap(cfg, 0x40, 0x05, 0x50, 0x0082);    // 2 vectors, 64-bit
	cfg_cap(cfg, 0x50, 0x05, 0x00, 0x010A);    // 32 vectors, masking
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.count == 2 && caps.msi_vectors == 2 && caps.msi_64bit && !caps.msi_masking, "second MSI replaced the first");
	CHECK(pci_caps_find(&caps, 0x05) == 0x40, "find returns the first MSI");
	free(cfg);
}

// The longest list there can be: every dword from 0x40 to 0xFC, in order.
static void test_full(void)
{
	uint8_t *cfg = cfg_new();
	pci_caps_t caps;
	cfg[0x34] = 0x40;
	for (int at = 0x40; at <= 0xFC; at += 4)
		cfg_cap(cfg, (uint8_t)at, 0x09, at == 0xFC ? 0 : (uint8_t)(at + 4), 0);
	pci_caps_walk(cfg_read, cfg, &caps);
	CHECK(caps.count == PCI_CAPS_MAX && !caps.damaged, "full list: count %u damaged %d", caps.count, caps.damaged);
	CHECK(caps.entry[PCI_CAPS_MAX - 1].offset == 0xFC, "full list: last at 0x%02x", caps.entry[PCI_CAPS_MAX - 1].offset);
	free(cfg);
}

// Random configuration spaces: the walk ends, stays inside the table and
// reads no more than the status, the pointer and one dword an entry.
static void test_random(int rounds)
{
	uint32_t seed = 0x05C0FFEE;
	for (int r = 0; r < rounds; r++)
	{
		uint8_t *cfg = cfg_new();
		pci_caps_t caps;
		for (int i = 0x34; i < 256; i++)
		{
			seed = seed * 1664525u + 1013904223u;
			cfg[i] = (uint8_t)(seed >> 24);
		}
		reads = 0;
		pci_caps_walk(cfg_read, cfg, &caps);
		CHECK(caps.count <= PCI_CAPS_MAX, "random %d: count %u", r, caps.count);
		CHECK(reads <= 2 + PCI_CAPS_MAX, "random %d: %d reads", r, reads);
		for (int i = 0; i < caps.count; i++)
		{
			CHECK(caps.entry[i].offset >= 0x40 && (caps.entry[i].offset & 3) == 0, "random %d: entry at 0x%02x", r, caps.entry[i].offset);
			for (int j = 0; j < i; j++)
				CHECK(caps.entry[j].offset != caps.entry[i].offset, "random %d: 0x%02x twice", r, caps.entry[i].offset);
		}
		free(cfg);
	}
}

static void test_names(void)
{
	CHECK(pci_cap_name(0x05) && strcmp(pci_cap_name(0x05), "msi") == 0, "name of MSI");
	CHECK(pci_cap_name(0x11) && strcmp(pci_cap_name(0x11), "msix") == 0, "name of MSI-X");
	CHECK(pci_cap_name(0x10) && strcmp(pci_cap_name(0x10), "express") == 0, "name of express");
	CHECK(pci_cap_name(0x00) == NULL && pci_cap_name(0xEE) == NULL, "an unknown id has a name");
	// A name is one token of a whitespace-separated line.
	for (int id = 0; id < 256; id++)
	{
		const char *n = pci_cap_name((uint8_t)id);
		if (n)
			CHECK(n[0] != 0 && strpbrk(n, " \t\n@") == NULL, "name of 0x%02x is not one token", id);
	}
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 20000;
	test_no_list();
	test_empty_list();
	test_ordinary();
	test_msi_fields();
	test_reserved_pointer_bits();
	test_damage();
	test_full();
	test_random(rounds);
	test_names();
	if (failures)
	{
		printf("pci_caps: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("pci_caps: all checks passed (%d random spaces)\n", rounds);
	return 0;
}
