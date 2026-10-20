// acpi.c — see acpi.h. The walk itself is acpi_tables.c; this file gives it
// memory to read, and turns what it found into the globals the rest of the
// kernel runs on.

#include "acpi.h"
#include "CONFIG.h"
#include "io.h"              // inw/outw/outb — the PM1 control registers are ports
#include "BasicRenderer.h"   // printf — the boot summary and acpi_poweroff report to the GLASS
#include "serial_logging.h"
#include "paging.h"
#include "smp.h"             // kIOAPICAddress
#include "driver/system/pci.h"
#include "sprintf.h"

extern uintptr_t kLimineRSDP;

int16_t kISAIrqToGSI[16] = { -1, -1, -1, -1, -1, -1, -1, -1,
                             -1, -1, -1, -1, -1, -1, -1, -1 };
uint16_t kISAIrqOverrideFlags[16] = { 0 };

static acpi_info_t s_acpi;
static acpi_used_t s_used = { -1, -1 };

const acpi_info_t *acpi_info(void)
{
	return &s_acpi;
}

const acpi_used_t *acpi_used(void)
{
	return &s_used;
}

// ── The walk's window onto firmware memory ──────────────────────────────────
// Firmware puts its tables wherever it likes, so every range the parser asks
// for is given a READ-ONLY HHDM alias of its own, and every page mapped for
// that purpose is recorded and unmapped when the walk ends.
// The lazy HHDM keeps memory nobody owns unmapped on purpose (a stray pointer
// into it faults instead of reading quietly), and nothing the kernel keeps
// from the tables needs them mapped afterwards: what it uses is copied out.
//
// A page that is ALREADY mapped is somebody else's — allocator-owned RAM, or
// a window another driver made — so it is read through, and never recorded,
// and never taken down.

// Physical addresses an HHDM alias can name with `|`: everything below the
// HHDM's own 47-bit span. A root-table entry past it is firmware garbage.
#define ACPI_PHYS_LIMIT (1ULL << 47)

#define ACPI_MAP_RUNS 128
static struct { uint64_t first; uint64_t pages; } s_runs[ACPI_MAP_RUNS];
static uint32_t s_run_count;

static bool acpi_map_record(uint64_t page)
{
	if (s_run_count > 0)
	{
		uint64_t last = s_runs[s_run_count - 1].first + s_runs[s_run_count - 1].pages * PAGE_SIZE;
		if (page == last)
		{
			s_runs[s_run_count - 1].pages++;
			return true;
		}
	}
	if (s_run_count >= ACPI_MAP_RUNS)
		return false;
	s_runs[s_run_count].first = page;
	s_runs[s_run_count].pages = 1;
	s_run_count++;
	return true;
}

static const void *acpi_map(void *ctx, uint64_t phys, uint32_t length)
{
	(void)ctx;
	if (length == 0 || phys >= ACPI_PHYS_LIMIT || length > ACPI_PHYS_LIMIT - phys)
		return NULL;

	uint64_t first = phys & PAGE_ADDRESS_MASK;
	uint64_t end = (phys + length + PAGE_SIZE - 1) & PAGE_ADDRESS_MASK;
	if (!paging_hhdm_prepare_range(first, end - first))
		return NULL;   // the page-table pool is dry: refuse, and the walk says so

	for (uint64_t page = first; page < end; page += PAGE_SIZE)
	{
		uint64_t va = page | kHHDMOffset;
		if (paging_walk_paging_table((pt_entry_t *)kKernelPML4v, va) != 0xbadbadba)
			continue;
		if (!acpi_map_record(page))
			return NULL;
		paging_map_page((pt_entry_t *)kKernelPML4v, va, page, PAGE_PRESENT);
	}
	return (const void *)(uintptr_t)(phys | kHHDMOffset);
}

// The walk runs before any other core does, so the local invlpg each unmap
// performs is the whole shootdown.
static void acpi_unmap_all(void)
{
	for (uint32_t i = 0; i < s_run_count; i++)
		paging_unmap_pages((pt_entry_t *)kKernelPML4v, s_runs[i].first | kHHDMOffset,
		                   s_runs[i].pages * PAGE_SIZE);
	s_run_count = 0;
}

// ── What the kernel takes from it ───────────────────────────────────────────

// The ISA IRQs are GSIs 0-15, so the IOAPIC that matters is the one whose
// range starts at GSI 0 — NOT the first one listed. A machine with two (a
// Ryzen board typically has the chipset's at GSI 0 and the processor's at
// 24) lists them in whichever order its firmware likes, and programming the
// PIT into the wrong chip loses the tick.
static void acpi_take_ioapic(void)
{
	const acpi_madt_info_t *m = &s_acpi.madt;
	const acpi_ioapic_t *isa = NULL;
	for (uint32_t i = 0; i < m->ioapic_count; i++)
		if (isa == NULL || m->ioapics[i].gsi_base < isa->gsi_base)
			isa = &m->ioapics[i];
	if (isa == NULL)
		return;
	if (isa->gsi_base != 0)
		printd(DEBUG_ACPI, "ACPI: no IO APIC serves GSI 0; using id %u at 0x%08x (GSI %u up)\n",
		       isa->id, isa->address, isa->gsi_base);
	kIOAPICAddress = isa->address;
	s_used.ioapic = (int)(isa - m->ioapics);

	for (uint32_t i = 0; i < m->override_count; i++)
	{
		const acpi_override_t *o = &m->overrides[i];
		if (o->bus == 0 && o->source < 16)
		{
			kISAIrqToGSI[o->source] = (int16_t)o->gsi;
			kISAIrqOverrideFlags[o->source] = o->flags;
		}
	}
}

// PCI Express configuration space for segment 0. MCFG's base is the address
// of BUS ZERO even when the range starts later, so the window mapped is only
// the buses it names, while kPCIBaseAddress stays the bus-zero origin that
// PCI_MMIO_OFFSET is added to. os64 enumerates one segment.
static void acpi_take_pci(void)
{
	for (uint32_t i = 0; i < s_acpi.mcfg_count; i++)
	{
		const acpi_mcfg_t *c = &s_acpi.mcfg[i];
		if (c->segment != 0 || kPCIBaseAddress != 0)
		{
			printd(DEBUG_ACPI, "ACPI: MCFG segment %u (buses %u-%u at 0x%016lx) not used\n",
			       c->segment, c->start_bus, c->end_bus, c->base);
			continue;
		}
		if (c->start_bus > c->end_bus || c->base == 0)
		{
			printd(DEBUG_ACPI, "ACPI: MCFG segment 0 names buses %u-%u at 0x%016lx - ignored\n",
			       c->start_bus, c->end_bus, c->base);
			continue;
		}
		uint64_t pages = ((uint64_t)c->end_bus - c->start_bus + 1) << (20 - 12);   // 1 MiB per bus
		uint64_t first = c->base + ((uint64_t)c->start_bus << 20);
		paging_map_pages((pt_entry_t *)kKernelPML4v, first | kHHDMOffset, first, pages,
		                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
		kPCIBaseAddress = c->base | kHHDMOffset;
		kPCIBusFirst = c->start_bus;
		kPCIBusLast = c->end_bus;
		s_used.mcfg = (int)i;
		printd(DEBUG_BOOT, "ACPI: PCI configuration space at 0x%016lx, buses %u-%u\n",
		       c->base, c->start_bus, c->end_bus);
	}
}

static void acpi_log(void)
{
	printd(DEBUG_ACPI, "ACPI: RSDP at 0x%016lx, revision %u, OEM '%s'; root %s at 0x%016lx, %u entries\n",
	       s_acpi.rsdp_phys, s_acpi.rsdp_revision, s_acpi.rsdp_oem_id,
	       s_acpi.root_signature[0] ? s_acpi.root_signature : "(none)", s_acpi.root_phys, s_acpi.root_entries);
	for (uint32_t i = 0; i < s_acpi.table_count; i++)
	{
		const acpi_table_t *t = &s_acpi.tables[i];
		printd(DEBUG_ACPI, "ACPI:   %s at 0x%016lx, %u bytes, revision %u, '%s' '%s'%s%s\n",
		       t->signature, t->phys, t->length, t->revision, t->oem_id, t->oem_table_id,
		       t->checksum_ok ? "" : ", BAD CHECKSUM", t->via_fadt ? ", via the FADT" : "");
	}
	uint32_t kept = s_acpi.problem_count < ACPI_MAX_PROBLEMS ? s_acpi.problem_count : ACPI_MAX_PROBLEMS;
	for (uint32_t i = 0; i < kept; i++)
	{
		const acpi_problem_t *p = &s_acpi.problems[i];
		printd(DEBUG_BOOT, "ACPI: problem: %s%s%s at 0x%016lx (0x%lx)\n",
		       p->signature, p->signature[0] ? " " : "", acpi_problem_name(p->code), p->phys, p->value);
	}
}

void acpi_init(void)
{
	if (kLimineRSDP == 0)
	{
		printf("ACPI: the bootloader found no RSDP - no ACPI\n");
		printd(DEBUG_BOOT, "ACPI: the bootloader found no RSDP - no ACPI\n");
		return;
	}

	acpi_parse(kLimineRSDP, acpi_map, NULL, &s_acpi);
	acpi_unmap_all();
	acpi_log();
	acpi_take_ioapic();
	acpi_take_pci();

	// One line, on the glass and in the log: on a machine meeting os64 for
	// the first time this is the line that says what the firmware described.
	char pci[32] = "no PCI configuration space";
	if (kPCIBaseAddress != 0)
		sprintf(pci, "PCI buses %u-%u", kPCIBusFirst, kPCIBusLast);
	char problems[48] = "";
	if (s_acpi.problem_count != 0)
		sprintf(problems, "; %u problems (cat /sys/acpi)", s_acpi.problem_count);
	char line[192];
	sprintf(line, "ACPI: %s, %u tables; %u CPUs, %u IO APICs, %s%s, S5 %s%s",
	        s_acpi.root_signature[0] ? s_acpi.root_signature : "no root table",
	        s_acpi.table_count, s_acpi.madt.cpus_enabled, s_acpi.madt.ioapic_count,
	        pci, s_acpi.hpet.present ? ", HPET" : "",
	        s_acpi.s5.present ? "ready" : "unavailable", problems);
	printf("%s\n", line);
	printd(DEBUG_BOOT, "%s\n", line);
}

// ── S5 soft-off: the machine actually going dark ────────────────────────────
// The hypervisor poweroff ports (0x604, 0x4004) are decoded by QEMU and VBox
// and by nothing else, so real iron needs ACPI's soft-off: write SLP_TYP for
// S5 into the PM1 control registers with SLP_EN. The numbers were all read at
// boot (acpi_init) — the shutdown descent is the worst possible moment to be
// walking firmware tables for the first time.

#define ACPI_SLP_EN       (1u << 13)   // PM1_CNT: "perform the transition"
#define ACPI_SCI_EN       (1u << 0)    // PM1_CNT: ACPI mode is on
#define ACPI_SLP_TYP_MASK (0x7u << 10) // PM1_CNT: which sleep state (S5 lives here)

bool acpi_poweroff(void)
{
	// EVERY line in here goes to the GLASS, not to printd. This function runs
	// after the log daemon has retired, on a machine that may have no serial
	// port at all — the screen is the only witness left, and a diagnostic
	// nobody can read is not a diagnostic.
	const acpi_fadt_info_t *f = &s_acpi.fadt;
	if (!f->present || !s_acpi.s5.present || f->pm1a_control == 0)
	{
		printf("  ACPI S5: not available (no FADT, no \\_S5, or no PM1a block)\n");
		return false;
	}
	uint16_t slpTypA = (uint16_t)((s_acpi.s5.slp_typ_a & 0x7u) << 10);
	uint16_t slpTypB = (uint16_t)((s_acpi.s5.slp_typ_b & 0x7u) << 10);

	printf("  ACPI S5: PM1a 0x%04x TYPa %u", f->pm1a_control, (unsigned)(slpTypA >> 10));
	if (f->pm1b_control != 0)
		printf(", PM1b 0x%04x TYPb %u", f->pm1b_control, (unsigned)(slpTypB >> 10));
	printf(", SCI_EN %u\n", (unsigned)(inw(f->pm1a_control) & ACPI_SCI_EN));

	// ACPI mode has to be ON before the PM1 registers mean anything. Under
	// UEFI it always already is (the firmware never ran in legacy mode), and
	// on anything with a SMI command port we ask once and wait a bounded
	// while — an unbounded poll here would hang the descent on firmware that
	// simply never answers.
	if (!(inw(f->pm1a_control) & ACPI_SCI_EN) && f->smi_command != 0)
	{
		outb((uint16_t)f->smi_command, f->acpi_enable);
		for (int spins = 0; spins < 300000; spins++)
		{
			if (inw(f->pm1a_control) & ACPI_SCI_EN)
				break;
			__builtin_ia32_pause();
		}
	}

	// READ, MODIFY, WRITE — never a bare write. PM1a_CNT is not a command
	// register, it is a CONTROL register full of live bits, and bit 0 of it
	// is SCI_EN — "ACPI mode is on". A bare (SLP_TYP | SLP_EN) asks the
	// machine to enter S5 *and* to leave ACPI mode in the same 16-bit store,
	// which is not a request any firmware is obliged to honor, and most
	// don't. Preserve what is there, replace only the SLP_TYP field.
	uint16_t pm1a = inw(f->pm1a_control);
	pm1a &= (uint16_t)~ACPI_SLP_TYP_MASK;
	pm1a |= (uint16_t)(slpTypA | ACPI_SLP_EN);

	printf("  ACPI S5: PM1a port 0x%04x <- 0x%04x\n", f->pm1a_control, pm1a);
	outw(f->pm1a_control, pm1a);

	if (f->pm1b_control != 0)
	{
		uint16_t pm1b = inw(f->pm1b_control);
		pm1b &= (uint16_t)~ACPI_SLP_TYP_MASK;
		pm1b |= (uint16_t)(slpTypB | ACPI_SLP_EN);
		printf("  ACPI S5: PM1b port 0x%04x <- 0x%04x\n", f->pm1b_control, pm1b);
		outw(f->pm1b_control, pm1b);
	}

	// Reached only if the transition did not take. Say so ON THE GLASS: by
	// this point logd is retired and a machine with no UART (the P5) has no
	// other channel left — a printd here would die in the rings with the
	// machine that needed to report it.
	printf("  ACPI S5: the machine did not take it\n");
	return true;
}
