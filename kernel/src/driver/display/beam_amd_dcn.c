// beam_amd_dcn.c — the beam from a Radeon display core (DCN): the output
// timing generator's line counter, OTG_STATUS_POSITION, which says which line
// the display is reading. All it does is read registers: the control, frame
// and blank registers of each pipe in turn until one is running, then that
// pipe's line counter.
//
// READ ONLY, ENFORCED BY THE CPU. The pages holding those registers are
// mapped present, uncached and NOT writable (CR0.WP makes that bind ring 0),
// so a stray store faults instead of reaching the chip. Nothing here writes
// to the chip, its BARs or its configuration space; a BAR is never sized,
// because sizing writes to it.

#include "driver/display/beam.h"
#include "pci.h"
#include "paging.h"
#include "printd.h"
#include "CONFIG.h"
#include "sprintf.h"

extern uint64_t kHHDMOffset;
extern uintptr_t kKernelPML4v;

#define LINE_MASK 0x7FFFu     // V_TOTAL, V_BLANK_START and VERT_COUNT: bits 0-14
#define OTG_MASTER_EN 0x1u    // OTG_CONTROL bit 0: the pipe is running

// One row per chip: where its registers are, every address with the source
// it came from. Indices are dwords, as the Linux headers give them.
typedef struct {
	uint16_t vendor, device;
	const char *core;
	uint8_t bar;              // the register BAR
	uint32_t segment;         // the display core's register segment, its base
	uint32_t v_total, v_blank_start_end, control, status_position;   // OTG0's, within the segment
	uint32_t otg_stride;      // from one OTG's registers to the next's
	uint32_t otgs;
} dcn_row_t;

static const dcn_row_t s_rows[] = {
	// Rembrandt (Radeon 680M, the P5's), DCN 3.1.2. From Linux master
	// 141b164bbe18 (2026-11-16), drivers/gpu/drm/amd/:
	//   amdgpu/amdgpu_drv.c:2183
	//       { PCI_DEVICE(0x1002, 0x1681), .driver_data = CHIP_YELLOW_CARP|AMD_IS_APU }
	//   display/dc/resource/dcn31/dcn31_resource.c:82-83, 128-134
	//       this chip's display code takes yellow_carp_offset.h and
	//       dcn/dcn_3_1_2_offset.h, and a register's index is its segment's
	//       base plus its offset (SR, BASE_INNER)
	//   include/yellow_carp_offset.h:387
	//       DCN_BASE__INST0_SEG2 0x000034C0
	//   include/asic_reg/dcn/dcn_3_1_2_offset.h:8767-8814
	//       regOTG0_OTG_V_TOTAL 0x1b2f, ..._V_BLANK_START_END 0x1b36,
	//       ..._OTG_CONTROL 0x1b41, ..._OTG_STATUS_POSITION 0x1b4a, each
	//       _BASE_IDX 2; OTG1 and OTG3 0x80 and 0x180 on (8981, 9027, 9409, 9455)
	//   include/asic_reg/dcn/dcn_3_1_2_sh_mask.h:29356, 29400-29401, 29499, 29544
	//       V_TOTAL, V_BLANK_START and VERT_COUNT are bits 0-14,
	//       V_BLANK_END bits 16-30, OTG_MASTER_EN bit 0
	//   amdgpu/amdgpu_device.c:3926
	//       rmmio_base = pci_resource_start(adev->pdev, 5)
	//   amdgpu/amdgpu_reg_access.c:378
	//       readl(adev->rmmio + (reg * 4))
	// So OTG0's position is BAR 5 + 4 x (0x34C0 + 0x1b4a) = BAR 5 + 0x14028,
	// and the four OTGs' registers lie in BAR 5 + 0x13000..0x14FFF.
	{ 0x1002, 0x1681, "DCN 3.1.2", 5, 0x34C0, 0x1b2f, 0x1b36, 0x1b41, 0x1b4a, 0x80, 4 },
};

#define AMD_VENDOR 0x1002

static volatile const uint32_t *s_position;
static char s_why[96];

static uint32_t reg_byte(const dcn_row_t *row, uint32_t reg, uint32_t otg)
{
	return 4u * (row->segment + reg + otg * row->otg_stride);
}

static uint32_t dcn_read_line(void)
{
	uint32_t raw = *s_position;
	return raw == 0xFFFFFFFFu ? raw : raw & LINE_MASK;
}

// init_PCI files function 0 of each device in kPCIDeviceHeaders and the
// other functions in kPCIDeviceFunctions, so both are searched: the P5's
// display core is function 0, and the beam probe missed it by searching one.
static const pci_device_t *find_display(const dcn_row_t **row, const pci_device_t **other_amd)
{
	*other_amd = NULL;
	for (int list = 0; list < 2; list++) {
		const pci_device_t *devs = list == 0 ? kPCIDeviceHeaders : kPCIDeviceFunctions;
		uint32_t count = list == 0 ? kPCIDeviceCount : kPCIFunctionCount;
		for (uint32_t i = 0; i < count; i++) {
			const pci_device_t *d = &devs[i];
			if (d->class != PCI_CLASS_DISPLAY_CONTROLLER)
				continue;
			for (unsigned r = 0; r < sizeof(s_rows) / sizeof(s_rows[0]); r++)
				if (d->vendor == s_rows[r].vendor && d->device == s_rows[r].device) {
					*row = &s_rows[r];
					return d;
				}
			if (d->vendor == AMD_VENDOR && *other_amd == NULL)
				*other_amd = d;
		}
	}
	return NULL;
}

// A register page, read-only and uncached, at its HHDM address.
//
// VISIBLE FROM EVERY ADDRESS SPACE, and that has to be checked, not assumed.
// Every task's PML4 copies the kernel's top-level entries 256-511 when the
// task is made (task_init_page_tables), so the tables BELOW an entry are
// shared and a page mapped under an entry that already exists is seen by
// every task, made before or after. An entry created now would be seen only
// by tasks made after it. So a page whose top-level entry is absent is
// refused. (A 32-bit BAR lies in the HHDM's first 4 GB, whose entry the
// kernel's own memory already occupies.)
static const char *map_page(uint64_t page)
{
	uint64_t va = page | kHHDMOffset;
	if ((((pt_entry_t *)kKernelPML4v)[(va >> 39) & 0x1FF] & PAGE_PRESENT) == 0)
		return "the registers lie under a top-level page table entry no task shares";
	if (paging_walk_paging_table((pt_entry_t *)kKernelPML4v, va) != 0xbadbadba)
		return "the register pages are already mapped by something else";
	if (!paging_hhdm_prepare_range(page, PAGE_SIZE))
		return "no page tables for the register pages";
	paging_map_page((pt_entry_t *)kKernelPML4v, va, page,
	                PAGE_PRESENT | PAGE_PCD | PAGE_PWT | PAGE_NO_EXECUTE);
	return NULL;
}

static const char *dcn_probe(beam_found_t *out)
{
	const dcn_row_t *row = NULL;
	const pci_device_t *other;
	const pci_device_t *d = find_display(&row, &other);
	if (d == NULL) {
		if (other == NULL)
			return BEAM_NOT_MINE;
		snprintf(s_why, sizeof(s_why), "an AMD display core with no registers in the table (%02x:%02x.%x %04x:%04x)",
		         other->busNo, other->deviceNo, other->funcNo, other->vendor, other->device);
		return s_why;
	}

	// Read, never sized: sizing a BAR writes to it.
	uint32_t command = readPCIRegister(d->busNo, d->deviceNo, d->funcNo, 0x04);
	uint32_t bar = readPCIRegister(d->busNo, d->deviceNo, d->funcNo, 0x10 + 4 * row->bar);
	uint64_t base = bar & ~0xFull;
	if ((bar & 1) || ((bar >> 1) & 3) != 0 || base == 0 || !(command & 2)) {
		snprintf(s_why, sizeof(s_why), "BAR %u is not a 32-bit memory window with decoding on (0x%08x, command 0x%04x)",
		         row->bar, bar, command & 0xFFFF);
		return s_why;
	}

	uint64_t first = (base + reg_byte(row, row->v_total, 0)) & PAGE_ADDRESS_MASK;
	uint64_t last = (base + reg_byte(row, row->status_position, row->otgs - 1)) & PAGE_ADDRESS_MASK;
	uint64_t mapped = 0;
	const char *why = NULL;
	for (uint64_t page = first; page <= last && why == NULL; page += PAGE_SIZE) {
		why = map_page(page);
		if (why == NULL)
			mapped++;
	}

	int active = -1;
	volatile const uint32_t *regs = (volatile const uint32_t *)(uintptr_t)(base | kHHDMOffset);
	uint32_t vt = 0, vb = 0;
	for (uint32_t o = 0; o < row->otgs && why == NULL; o++) {
		uint32_t ctl = regs[reg_byte(row, row->control, o) / 4];
		vt = regs[reg_byte(row, row->v_total, o) / 4];
		vb = regs[reg_byte(row, row->v_blank_start_end, o) / 4];
		if (ctl == 0xFFFFFFFFu && vt == 0xFFFFFFFFu && vb == 0xFFFFFFFFu) {
			why = "the display core's registers read all ones";
			break;
		}
		// The first pipe running is the one read, and those after it are not
		// touched: an idle pipe's registers are the least certain thing on the
		// chip to read.
		if ((ctl & OTG_MASTER_EN) && (vt & LINE_MASK) != 0) {
			active = (int)o;
			break;
		}
	}
	if (why == NULL && active < 0)
		why = "no display pipe is running";
	if (why != NULL) {
		// Nothing will read these pages. A CPU that saw them can only hold a
		// read-only, uncached view of the same registers.
		for (uint64_t i = 0; i < mapped; i++)
			paging_unmap_page((pt_entry_t *)kKernelPML4v, (first + i * PAGE_SIZE) | kHHDMOffset);
		return why;
	}

	s_position = &regs[reg_byte(row, row->status_position, (uint32_t)active) / 4];
	// amdgpu programs V_TOTAL as the frame's lines less one.
	out->lines = (vt & LINE_MASK) + 1;
	out->blank_start = vb & LINE_MASK;
	out->blank_end = (vb >> 16) & LINE_MASK;
	out->read_line = dcn_read_line;
	snprintf(out->where, sizeof(out->where), "%02x:%02x.%x %04x:%04x %s OTG%d", d->busNo, d->deviceNo,
	         d->funcNo, d->vendor, d->device, row->core, active);
	printd(DEBUG_BOOT, "beam: amd_dcn: %s, BAR %u 0x%lx, registers mapped read-only\n", out->where, row->bar, base);
	return NULL;
}

const beam_driver_t kBeamAmdDcn = { "amd_dcn", dcn_probe };
