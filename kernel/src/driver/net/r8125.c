// r8125.c — RealTek RTL8125 2.5GbE. First light: identify, map, read the
// burned-in MAC, and report the link. Rings and traffic are the next slice.
//
// See r8125.h for the register-provenance rules and RTL8125.md for the
// design. The short version of both: this chip has never met this code, so
// every step says what it did, and every offset says where it came from.
//
// WHY THIS SLICE STOPS WHERE IT DOES. On hardware no emulator rehearses,
// the useful unit of work is ONE UNKNOWN AT A TIME. "Did we find the chip,
// can we reach its registers, does it tell us a MAC address that looks like
// a MAC address, and does it think the cable is in?" is a complete question
// with an unambiguous answer, and every later question depends on it. A
// driver that arrived with rings, interrupts and a transmit path in one
// commit would, on its first boot, produce a single silence with a dozen
// possible causes.

#include "driver/net/r8125.h"
#include "driver/net/net_device.h"
#include "driver/system/pci.h"
#include "serial_logging.h"
#include "BasicRenderer.h"
#include "memory/kmalloc.h"
#include "paging.h"
#include "kernel.h"
#include "CONFIG.h"

// kPCIDeviceHeaders / kPCIDeviceFunctions and their counts come from pci.h —
// declared there, so NOT re-declared here. (Re-declaring them by hand is how
// this file first failed to compile: the counts are uint8_t, not int, and the
// function table is a plain pci_device_t* rather than a type of its own.)
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;
// NOR8125 / POLL8125 — the flashlight pair (kernel_commandline.c). UPPERCASE
// because the cmdline parser matches with strcmp and does not fold case; a
// lowercase spelling matches nothing and is silently ignored, which is how
// three older boot entries spent years doing nothing (commit 642eb9f).
extern bool kEnableR8125;

// ── Registers ───────────────────────────────────────────────────────────────
// Provenance tags per r8125.h. [8169-family] = shared across the lineage,
// high confidence. [8125-SPECIFIC] = moved or widened in this generation,
// from Linux r8169.c (RTL_GIGA_MAC_VER_60+), MUST be confirmed against the
// vendor GPL r8125 driver before first light.
//
// This slice only READS from these, and only from the two safest ones
// (MAC0 and PHYstatus). Nothing here programs a ring, so a wrong offset in
// the not-yet-used group cannot do damage before it is verified.

#define R8125_MAC0        0x00   // [8169-family] 6 bytes, burned-in address
#define R8125_CHIPCMD     0x37   // [8169-family] 8-bit: reset / Tx enable / Rx enable
#define R8125_TXCONFIG    0x40   // [8169-family] 32-bit
#define R8125_RXCONFIG    0x44   // [8169-family] 32-bit
#define R8125_CFG9346     0x50   // [8169-family] 8-bit: config-register lock
#define R8125_PHYSTATUS   0x6C   // [8169-family] 8-bit: link + speed + duplex
#define R8125_RXMAXSIZE   0xDA   // [8169-family] 16-bit (RMS)
#define R8125_RDSAR_LOW   0xE4   // [8169-family] 32-bit: RX descriptor base
#define R8125_RDSAR_HIGH  0xE8   // [8169-family]
#define R8125_TNPDS_LOW   0x20   // [8169-family] 32-bit: TX descriptor base
#define R8125_TNPDS_HIGH  0x24   // [8169-family]

// The three that MOVED, and the reason this driver has a tripwire at all.
// On the 8169/8168 the interrupt mask/status were 16-bit at 0x3C/0x3E and
// the transmit doorbell (TxPoll) was 8-bit at 0x38. The 8125 widened the
// interrupt pair to 32 bits and relocated it to 0x38/0x3C — which it could
// only do because the doorbell moved OUT to 0x90. The three changes are one
// change, and that internal consistency is the main reason to believe this
// reading of them; it is NOT a substitute for checking the vendor driver.
// UNCONFIRMED — unused by this slice, listed so the next one starts here.
#define R8125_IMR0_8125   0x38   // [8125-SPECIFIC] 32-bit interrupt mask   (UNCONFIRMED)
#define R8125_ISR0_8125   0x3C   // [8125-SPECIFIC] 32-bit interrupt status (UNCONFIRMED)
#define R8125_TPPOLL_8125 0x90   // [8125-SPECIFIC] 16-bit transmit doorbell (UNCONFIRMED)

// PHYstatus bits [8169-family]. The 8125 adds a 2500Mbps indication that
// this driver does not decode: the ratified topology is a gigabit switch,
// so 1000/full is the expected and desired answer, and a link this driver
// cannot name is reported by its raw value rather than guessed at.
#define R8125_PHY_FULLDUP   0x01
#define R8125_PHY_LINKSTS   0x02
#define R8125_PHY_10M       0x04
#define R8125_PHY_100M      0x08
#define R8125_PHY_1000M     0x10

typedef struct
{
	pci_device_t* pci;
	volatile uint8_t* regs;
	net_device_t netdev;
	bool present;
} r8125_t;

static r8125_t s_r8125;

static inline uint8_t r8125_read8(r8125_t* r, uint32_t reg)
{
	return *(volatile uint8_t*)(r->regs + reg);
}

static inline uint32_t r8125_read32(r8125_t* r, uint32_t reg)
{
	return *(volatile uint32_t*)(r->regs + reg);
}

// ── BAR mapping ─────────────────────────────────────────────────────────────
// Resolve a BAR to a physical base, honoring the 64-bit encoding (bits 2:1
// == 10b means the high half lives in the next BAR) and refusing an I/O BAR
// outright. On this family an I/O BAR here is not a misconfiguration to work
// around — it means we asked for the wrong BAR number, and saying so beats
// poking ports and wondering.
static uint64_t r8125_bar_phys(pci_device_t* dev, uint8_t bar)
{
	if (bar >= 6)
		return 0;
	uint32_t lo = dev->baseAdd[bar];
	if (lo & 0x1)          // I/O space
		return 0;
	uint64_t phys = lo & ~0xFULL;
	if ((lo & 0x6) == 0x4 && bar < 5)
		phys |= ((uint64_t)dev->baseAdd[bar + 1]) << 32;
	return phys;
}

// Map the register window at the kernel's HHDM alias, PAGE_PCD.
//
// The HHDM (upper half) rather than anywhere else because r8125_poll runs
// from processSignals, under WHATEVER task's CR3 happened to be interrupted
// — upper-half mappings are shared by every address space, lower-half ones
// are not. That is not a style preference: it is the exact distinction that
// panicked kworker earlier today when a lower-half DMA mapping turned out to
// exist in one address space only (see DEBTS.md's kmalloc_dma row).
//
// PAGE_PCD because device registers must never be cached — a cached read of
// a ring cursor returns stale truth, which in a producer/consumer protocol
// is indistinguishable from a lie.
static volatile uint8_t* r8125_map_regs(uint64_t bar_phys)
{
	uint64_t first_page = bar_phys & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t page_count = (R8125_REG_WINDOW + (bar_phys - first_page) + PAGE_SIZE - 1) / PAGE_SIZE;

	paging_map_pages((pt_entry_t*)kKernelPML4v, kHHDMOffset + first_page,
	                 first_page, page_count,
	                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	return (volatile uint8_t*)(kHHDMOffset + bar_phys);
}

// ── Link ────────────────────────────────────────────────────────────────────
// Report what the PHY says, in words, either way. "No link" is a perfectly
// good answer that means "check the cable" — and on a machine with no
// debugger attached, an honest negative printed at boot is worth more than
// most positives.
static void r8125_report_link(r8125_t* r)
{
	uint8_t phy = r8125_read8(r, R8125_PHYSTATUS);
	r->netdev.link_up = (phy & R8125_PHY_LINKSTS) != 0;

	if (!r->netdev.link_up)
	{
		printf("r8125: no link — is the cable in? (PHYstatus 0x%02x)\n", phy);
		return;
	}

	const char* speed = (phy & R8125_PHY_1000M) ? "1000" :
	                    (phy & R8125_PHY_100M)  ? "100"  :
	                    (phy & R8125_PHY_10M)   ? "10"   : "?";
	printf("r8125: link %s/%s (PHYstatus 0x%02x)\n",
	       speed, (phy & R8125_PHY_FULLDUP) ? "full" : "half", phy);
	// The raw byte rides along on purpose: this is a 2.5GbE part reporting
	// through a register whose speed encoding this driver only partly
	// decodes, so if it ever negotiates something we cannot name, the number
	// that would explain it is already in the log.
}

// ── Transmit (not yet) ──────────────────────────────────────────────────────
// The seam wants a transmit op at registration time, and an honest refusal
// is better than a NULL that faults or a silent success that loses frames.
// This slice registers no net_device at all (see init_r8125), so nothing can
// reach this — it exists so the next slice replaces a named stub rather than
// filling a hole.
static int32_t r8125_transmit(struct net_device* dev, const void* frame, uint16_t length)
{
	(void)dev; (void)frame; (void)length;
	return -1;
}

static net_operations_t s_r8125_ops = {
	.transmit = r8125_transmit,
};

void r8125_poll(void)
{
	// Nothing to drain until the rings exist. The guard is the point: this
	// call is already wired into processSignals so the next slice needs no
	// scheduler surgery, and it costs one predictable branch per pass.
	if (!s_r8125.present)
		return;
}

// ── Bring-up ────────────────────────────────────────────────────────────────
static bool r8125_init_device(pci_device_t* dev)
{
	r8125_t* r = &s_r8125;
	r->pci = dev;

	// BEACON 1: found. Bus/device/function, so a P5 boot can be compared
	// against its own lspci output without guessing.
	printf("r8125: found 10ec:8125 at %02x:%02x.%u\n",
	       dev->busNo, dev->deviceNo, dev->funcNo);

	// Memory space + bus mastering, by LIVE read-modify-write of the command
	// register — never the cached enum copy. That is the ruling from a848273:
	// the cached word can be stale, and writing a stale command word back is
	// how you strip a neighbour's DMA mid-flight. Bus mastering is what will
	// legalize the rings' DMA in the next slice; without it the cruellest
	// failure mode in this file becomes available, where every register reads
	// back exactly as written and not one frame ever moves.
	uint32_t live = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4) & 0xFFFF;
	writePCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4, live | 0x6);

	uint64_t bar_phys = r8125_bar_phys(dev, R8125_BAR);
	if (bar_phys == 0)
	{
		// Refuse loudly. On this family BAR0 is I/O and BAR2 is memory, so
		// this branch most likely means the BAR NUMBER is wrong rather than
		// the hardware — name both possibilities where they will be read.
		printf("r8125: BAR%u is not a memory BAR (raw 0x%08x) — refusing\n",
		       R8125_BAR, dev->baseAdd[R8125_BAR]);
		return false;
	}
	r->regs = r8125_map_regs(bar_phys);

	// BEACON 2: mapped.
	printf("r8125: BAR%u phys 0x%lx mapped at HHDM\n", R8125_BAR, bar_phys);

	// BEACON 3: the MAC. This is the first read that proves the register
	// window is genuinely the chip's and not a mis-mapped page: a plausible
	// MAC is very hard to get by accident. All-zero or all-ones is what a
	// dead window returns, and either means STOP — continuing from here
	// would program rings through a pointer we have no reason to trust.
	for (int i = 0; i < NET_MAC_LEN; i++)
		r->netdev.mac[i] = r8125_read8(r, R8125_MAC0 + i);

	bool all_zero = true, all_ones = true;
	for (int i = 0; i < NET_MAC_LEN; i++)
	{
		if (r->netdev.mac[i] != 0x00) all_zero = false;
		if (r->netdev.mac[i] != 0xFF) all_ones = false;
	}
	if (all_zero || all_ones)
	{
		printf("r8125: MAC reads %s — register window is not answering, refusing\n",
		       all_zero ? "all zeroes" : "all ones");
		return false;
	}

	printf("r8125: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
	       r->netdev.mac[0], r->netdev.mac[1], r->netdev.mac[2],
	       r->netdev.mac[3], r->netdev.mac[4], r->netdev.mac[5]);

	// BEACON 4: what the chip says about itself. ChipCmd's Tx/Rx enable bits
	// tell us whether firmware left the engines running, which is worth
	// knowing before the next slice resets them.
	printd(DEBUG_NET, "r8125: ChipCmd 0x%02x  TxConfig 0x%08x  RxConfig 0x%08x\n",
	       r8125_read8(r, R8125_CHIPCMD),
	       r8125_read32(r, R8125_TXCONFIG),
	       r8125_read32(r, R8125_RXCONFIG));

	// BEACON 5: the link.
	r8125_report_link(r);

	r->netdev.ops = &s_r8125_ops;
	r->netdev.mtu = 1500;
	r->netdev.driver_data = r;
	r->netdev.name[0] = 'r'; r->netdev.name[1] = '8'; r->netdev.name[2] = '1';
	r->netdev.name[3] = '2'; r->netdev.name[4] = '5'; r->netdev.name[5] = '0';
	r->netdev.name[6] = '\0';

	// NOT REGISTERED WITH THE SEAM YET, and that is deliberate. A
	// net_device in kNetDevices is a promise the stack will believe: ARP
	// will hand it frames and dhcp_start would dial out through it. This
	// slice cannot transmit, so registering would turn "no driver" into
	// "a driver that silently drops everything" — strictly worse, and
	// exactly the kind of quiet lie this house refuses. Registration lands
	// with the rings, in the same commit that makes transmit real.
	r->present = true;
	return true;
}

void init_r8125(void)
{
	if (!kEnableR8125)
	{
		printd(DEBUG_NET, "r8125: disabled by NOR8125\n");
		return;
	}

	// Scan devices AND functions — the xHCI/e1000 precedent. Real topologies
	// multi-function everything, and the P5 has already taught this kernel
	// once not to assume a plain slot.
	pci_device_t* found = NULL;
	for (int i = 0; i < kPCIDeviceCount && found == NULL; i++)
		if (kPCIDeviceHeaders[i].vendor == R8125_VENDOR_REALTEK &&
		    kPCIDeviceHeaders[i].device == R8125_DEVICE_8125)
			found = &kPCIDeviceHeaders[i];
	for (int i = 0; i < kPCIFunctionCount && found == NULL; i++)
		if (kPCIDeviceFunctions[i].vendor == R8125_VENDOR_REALTEK &&
		    kPCIDeviceFunctions[i].device == R8125_DEVICE_8125)
			found = &kPCIDeviceFunctions[i];

	if (found == NULL)
	{
		// The common case on every machine in this house except one. A
		// configuration, not a failure — and QEMU emulates no RTL8125 at
		// all, so this branch is what keeps every existing boot green.
		// One debug line, so "why no network?" starts with an answer.
		printd(DEBUG_NET, "r8125: no device on PCI\n");
		return;
	}

	if (!r8125_init_device(found))
		printf("r8125: device found but bring-up stopped (see the beacons above)\n");
}
