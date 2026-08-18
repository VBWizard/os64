// r8125.c — RealTek RTL8125 2.5GbE: probe, bring-up, and the polled
// transmit/receive path.
//
// See r8125.h for the register-provenance rules, r8125_ring.h for the
// descriptor protocol (deliberately hardware-free, and already proven by
// tools/test_r8125_host.c), and RTL8125.md for the design and its
// fact-check. The short version of all three: this chip meets this code on
// exactly one machine in the world, so every step says what it did and
// every offset says where it came from.
//
// HOW THIS DRIVER WAS BUILT, since the sequence was the point. First light
// (commit 22d3920) shipped ALONE: find the chip, map BAR2, read the MAC,
// report the link — four beacons, four separable answers, all four correct
// on the P5's first boot. The ring protocol shipped next, on its own, with
// eleven host tests. Only then this: reset, configure, enable, move frames.
// One unknown at a time, because on hardware no emulator rehearses, a
// driver that arrives all at once produces a single silence with a dozen
// possible causes.
//
// WHAT TO SUSPECT WHEN IT FAILS, in order. The bring-up beacons below name
// the last thing that worked, and the failure modes sort cleanly:
//   - stops before "soft reset complete"  → the register window is wrong
//     (but the MAC read already argues strongly against that)
//   - "chip disagreed" on RxConfig/TxConfig → a field this generation
//     shapes differently; the read-back value is the clue
//   - frames queue but never leave → THE DOORBELL. Register 0x90 and bit 0
//     are [8125-SPECIFIC] and UNCONFIRMED, and they are the single most
//     likely wrong thing in this file
//   - nothing ever arrives → the broadcast accept bit, or the rings' base
//     registers. ARP dies without broadcast and takes everything with it

#include "driver/net/r8125.h"
#include "driver/net/r8125_ring.h"
#include "driver/net/net_device.h"
#include "driver/system/pci.h"
#include "serial_logging.h"
#include "BasicRenderer.h"
#include "memory/kmalloc.h"
#include "paging.h"
#include "kernel.h"
#include "CONFIG.h"
#include "allocator.h"   // allocate_memory_aligned — HHDM-reachable DMA memory
#include "spinlock.h"
#include "memcpy.h"
#include "memset.h"

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
// ALL OF THESE ARE NOW LIVE. The first-light slice only read MAC0 and
// PHYstatus, which is why it was safe to ship ahead of verification; this
// one programs the chip, so a wrong offset can now do real damage. That is
// the whole reason for the read-back checks (r8125_write32_verify) and for
// the failure-mode list at the top of this file.

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
// UNCONFIRMED, and now USED — the doorbell in particular is the first
// suspect if frames queue and never leave.
#define R8125_IMR0_8125   0x38   // [8125-SPECIFIC] 32-bit interrupt mask   (UNCONFIRMED)
#define R8125_ISR0_8125   0x3C   // [8125-SPECIFIC] 32-bit interrupt status (UNCONFIRMED)
#define R8125_TPPOLL_8125 0x90   // [8125-SPECIFIC] 16-bit transmit doorbell (UNCONFIRMED)

// THE RECEIVE GATE — the prime suspect for a driver that transmits happily
// and receives nothing, which is exactly what the P5 reported on its first
// bring-up (tx=5, rx=0, drop_nh=0: net_device_rx was never even called).
//
// The 8168/8125 generations carry a MISC register with an RXDV_GATED_EN bit
// that gates the PHY's receive-data-valid signal. Gated, the MAC transmits
// perfectly and hears nothing — the asymmetry is the whole tell. Linux's
// r8169 has a dedicated rtl_disable_rxdvgate() and calls it while starting
// this generation's hardware, because the part can come out of reset with
// the gate closed. Nothing in our bring-up ever opened it.
//
// MEASURED ON THE P5, 2026-08-16: MISC reads 0x0000003f both before and
// after, so on THIS board the gate was never shut and clearing it was a
// no-op. The hypothesis was wrong — recorded rather than quietly deleted,
// because a plausible theory that the hardware refuted is worth more to the
// next reader than a clean file that pretends it was never entertained.
//
// The code STAYS. Linux's reference driver opens this gate while starting
// the generation, which means some board or some reset path does come up
// gated; ours simply is not one of them today. It costs one read-modify-
// write at init and it reports what it saw, which is exactly how we learned
// it was innocent.
//
// [8125-SPECIFIC] (UNCONFIRMED as to bit and register — all we have measured
// is that this address reads 0x3f on an RTL8125B, which is consistent with
// bit 19 being clear but does not prove the field is where we think it is).
#define R8125_MISC        0xF0        // [8125-SPECIFIC] 32-bit (UNCONFIRMED)
#define R8125_RXDV_GATED  (1u << 19)  // 1 = receive gated OFF

// PHYstatus bits [8169-family]. The 8125 adds a 2500Mbps indication that
// this driver does not decode: the ratified topology is a gigabit switch,
// so 1000/full is the expected and desired answer, and a link this driver
// cannot name is reported by its raw value rather than guessed at.
#define R8125_PHY_FULLDUP   0x01
#define R8125_PHY_LINKSTS   0x02
#define R8125_PHY_10M       0x04
#define R8125_PHY_100M      0x08
#define R8125_PHY_1000M     0x10

// ChipCmd bits [8169-family].
#define R8125_CMD_RESET     0x10
#define R8125_CMD_RX_ENABLE 0x08
#define R8125_CMD_TX_ENABLE 0x04

// PCI command bits. Memory decoding is needed to reach the MMIO reset;
// bus mastering stays OFF until that reset has stopped any stale DMA engine.
#define R8125_PCI_COMMAND_MEMORY 0x02
#define R8125_PCI_COMMAND_MASTER 0x04

// Cfg9346 [8169-family]: the config registers are write-locked until you
// say the magic word. A 1990s EEPROM-interface register still standing
// guard over the config space of a 2020s 2.5GbE part — the lock exists
// because these registers used to be shadowed from a 93C46 serial EEPROM.
#define R8125_CFG9346_UNLOCK 0xC0
#define R8125_CFG9346_LOCK   0x00

// RxConfig accept bits [8169-family].
#define R8125_RX_ACCEPT_ALLPHYS   0x01   // promiscuous — deliberately NOT set
#define R8125_RX_ACCEPT_MYPHYS    0x02   // unicast to our MAC
#define R8125_RX_ACCEPT_MULTICAST 0x04
#define R8125_RX_ACCEPT_BROADCAST 0x08   // NOT optional — see the essay at setup

// The DMA-burst / fetch fields. [8125-SPECIFIC] and UNCONFIRMED: the 8169
// generation put an unlimited DMA burst at (7 << 8), and Linux appears to
// add an 8125-only fetch-count field at (8 << 27). Read back and reported
// at init precisely because this author cannot verify it — see
// r8125_write32_verify.
#define R8125_RX_DMA_BURST   (7u << 8)
#define R8125_RX_FETCH_8125  (8u << 27)   // (UNCONFIRMED)

// TxConfig [8169-family]: unlimited DMA burst, standard interframe gap.
// 0x03000700 is the value this family has taken for twenty years.
#define R8125_TX_CONFIG_DEFAULT 0x03000700u

// Bits the CHIP owns in each config register — excluded from the read-back
// check. MEASURED on the P5 (2026-08-16), not guessed:
//
//   TxConfig: the read-only hardware version ID (R8125_HWVER_MASK, the field
//   that told us this is an RTL8125B) plus bit 11, the top of a DMA-burst
//   field that is four bits wide on this generation rather than three.
//
//   RxConfig: bit 17, which the chip asserts by itself. Everything this
//   driver actually asked for — the accept bits, the DMA burst, the fetch
//   field — was kept verbatim, so whatever bit 17 means, it is not a
//   rejection of ours.
#define R8125_HWVER_MASK 0x7CF00000u
#define R8125_TXCONFIG_HW_OWNED (R8125_HWVER_MASK | 0x00000800u)
#define R8125_RXCONFIG_HW_OWNED 0x00020000u

// The transmit doorbell's poke bit. [8125-SPECIFIC] (UNCONFIRMED) — on the
// 8169 this was NPQ (0x40) in an 8-bit register at 0x38; on the 8125 the
// doorbell moved to a 16-bit register at 0x90 and the poke appears to be
// bit 0. If frames are queued but never leave, THIS CONSTANT AND ITS
// REGISTER ARE THE FIRST TWO THINGS TO CHECK.
#define R8125_TPPOLL_NPQ 0x0001

typedef struct
{
	pci_device_t* pci;
	volatile uint8_t* regs;
	net_device_t netdev;
	bool present;

	// The rings, and both addresses for each: the PHYSICAL one the device
	// is given, and the HHDM alias we dereference. They are the same memory
	// — allocate_memory_aligned hands back page-aligned physical memory that
	// is HHDM-reachable while allocated (the lazy-HHDM rule), so one
	// allocation yields both. Upper-half by construction, which is what lets
	// r8125_poll touch them from processSignals under any task's CR3.
	uint64_t rx_phys, rx_buf_phys;
	uint64_t tx_phys, tx_buf_phys;
	r8125_desc_t* rx;
	r8125_desc_t* tx;
	uint8_t* rx_buf;
	uint8_t* tx_buf;

	uint16_t rx_cursor;    // next descriptor we expect a frame in
	uint16_t tx_next;      // next descriptor we will hand the device
	uint16_t tx_clean;     // oldest descriptor the device has not returned

	spinlock_t lock;       // guards the rings against concurrent transmits
} r8125_t;

static r8125_t s_r8125;

static inline uint8_t r8125_read8(r8125_t* r, uint32_t reg)
{
	return *(volatile uint8_t*)(r->regs + reg);
}

static inline uint16_t r8125_read16(r8125_t* r, uint32_t reg)
{
	return *(volatile uint16_t*)(r->regs + reg);
}

static inline uint32_t r8125_read32(r8125_t* r, uint32_t reg)
{
	return *(volatile uint32_t*)(r->regs + reg);
}

static inline void r8125_write8(r8125_t* r, uint32_t reg, uint8_t value)
{
	*(volatile uint8_t*)(r->regs + reg) = value;
}

static inline void r8125_write16(r8125_t* r, uint32_t reg, uint16_t value)
{
	*(volatile uint16_t*)(r->regs + reg) = value;
}

static inline void r8125_write32(r8125_t* r, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t*)(r->regs + reg) = value;
}

// Write a register, read it back, and SAY SO IF IT DISAGREES.
//
// This exists because of where this driver runs. On QEMU a wrong register
// value is ten seconds and a debugger; on the P5 it is a silence that could
// mean anything, in a room with no gdb. A chip that refuses or reshapes a
// value we wrote is telling us something specific — a reserved bit we set,
// a field that does not exist on this generation, a register that is not
// where we think it is — and that is exactly the information a boot log
// should carry off a machine we cannot attach to.
//
// Not every register reads back what you wrote (status bits, self-clearing
// commands), so this is used ONLY on the configuration registers where a
// mismatch really is news, and it reports rather than fails: an 8125 that
// quietly ignores one bit of RxConfig should still be given the chance to
// move a frame.
//
// `hardware_owned` names the bits the CHIP owns — read-only identity fields,
// reserved bits it asserts itself, fields it normalizes. Those are excluded
// from the comparison because a checker that cries wolf is worse than no
// checker: the second time it reports a "disagreement" that turns out to be
// normal, everybody stops reading it, including on the boot where it is
// telling the truth. Both masks below were measured on the P5's first
// bring-up (2026-08-16) rather than guessed.
static void r8125_write32_verify(r8125_t* r, uint32_t reg, uint32_t value,
                                 uint32_t hardware_owned, const char* name)
{
	r8125_write32(r, reg, value);
	uint32_t got = r8125_read32(r, reg);
	uint32_t differs = (got ^ value) & ~hardware_owned;

	if (differs != 0)
		printf("r8125: %s (0x%02x) wrote 0x%08x read back 0x%08x — chip disagreed on 0x%08x\n",
		       name, reg, value, got, differs);
	else
		printd(DEBUG_NET, "r8125: %s (0x%02x) = 0x%08x (read 0x%08x, hardware-owned bits ignored)\n",
		       name, reg, value, got);
}

// ── Which RTL8125 is this? ──────────────────────────────────────────────────
// The upper bits of TxConfig are a READ-ONLY hardware version ID — the whole
// family has carried one there for twenty years, and Linux's
// rtl8169_get_mac_version() does nothing but read this register and mask it
// to decide which chip it is talking to.
//
// We learned this the good way: the P5's first bring-up reported TxConfig
// "disagreeing" (wrote 0x03000700, read 0x67100f00), which was not a
// disagreement at all — it was the chip stating its identity in bits nobody
// can write. Masking gives 0x641, which is Linux's RTL_GIGA_MAC_VER_63: an
// RTL8125B. That matters beyond curiosity, because every offset in this
// driver marked UNCONFIRMED can now be checked against a SPECIFIC part
// rather than against a family.
static void r8125_report_version(r8125_t* r)
{
	uint32_t txcfg = r8125_read32(r, R8125_TXCONFIG);
	uint32_t id    = (txcfg & R8125_HWVER_MASK) >> 20;

	// Only the two this driver has any business claiming to know. Anything
	// else prints its number and no story — an unrecognized ID is a fact,
	// and inventing a name for it would be the kind of confident wrongness
	// that costs an afternoon later.
	const char* model = (id == 0x641) ? "RTL8125B" :
	                    (id == 0x608) ? "RTL8125A" : "unrecognized";
	printf("r8125: chip id 0x%03x (%s), TxConfig reads 0x%08x\n", id, model, txcfg);
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

// ── Ring construction ───────────────────────────────────────────────────────
// Both rings and both buffer slabs come from allocate_memory_aligned, which
// returns PAGE-ALIGNED physical memory that is HHDM-reachable while it is
// allocated. One allocation therefore yields both addresses this driver
// needs: the physical one the bus master is given, and an upper-half kernel
// pointer we can write through from any address space.
//
// NOT kmalloc_dma, and the reason is fresh: it identity-maps at a LOWER-half
// VA in kKernelPML4 alone, so anything touching such a buffer from another
// task's page tables takes a #PF — which is precisely what killed kworker
// earlier today (DEBTS.md). r8125_poll runs from processSignals under
// whatever CR3 was interrupted, so a lower-half buffer here would be a
// scheduled crash rather than a latent one. Page alignment also satisfies
// the hardware's 256-byte descriptor-base requirement several times over.
static bool r8125_setup_rings(r8125_t* r)
{
	r->rx_phys = allocate_memory_aligned(R8125_RX_DESCS * sizeof(r8125_desc_t));
	r->tx_phys = allocate_memory_aligned(R8125_TX_DESCS * sizeof(r8125_desc_t));
	r->rx_buf_phys = allocate_memory_aligned((uint64_t)R8125_RX_DESCS * R8125_BUF_SIZE);
	r->tx_buf_phys = allocate_memory_aligned((uint64_t)R8125_TX_DESCS * R8125_BUF_SIZE);
	if (r->rx_phys == 0 || r->tx_phys == 0 || r->rx_buf_phys == 0 || r->tx_buf_phys == 0)
	{
		// Give back whatever DID land before refusing. A one-time init
		// path, so the leak would be small — but "small" and "counted"
		// are different claims, and the lazy-HHDM tripwire means a leaked
		// page here would sit mapped forever, muddying the very
		// use-after-free diagnostics it exists to sharpen.
		if (r->rx_phys)     free_memory(r->rx_phys);
		if (r->tx_phys)     free_memory(r->tx_phys);
		if (r->rx_buf_phys) free_memory(r->rx_buf_phys);
		if (r->tx_buf_phys) free_memory(r->tx_buf_phys);
		r->rx_phys = r->tx_phys = r->rx_buf_phys = r->tx_buf_phys = 0;
		printf("r8125: out of memory building the rings\n");
		return false;
	}

	r->rx     = (r8125_desc_t*)(r->rx_phys | kHHDMOffset);
	r->tx     = (r8125_desc_t*)(r->tx_phys | kHHDMOffset);
	r->rx_buf = (uint8_t*)(r->rx_buf_phys | kHHDMOffset);
	r->tx_buf = (uint8_t*)(r->tx_buf_phys | kHHDMOffset);

	// The protocol half — pure arithmetic, and the half that is already
	// proven (tools/test_r8125_host.c, eleven tests including the EOR
	// invariant after every operation).
	r8125_ring_init_rx(r->rx, R8125_RX_DESCS, r->rx_buf_phys, R8125_BUF_SIZE);
	r8125_ring_init_tx(r->tx, R8125_TX_DESCS, r->tx_buf_phys, R8125_BUF_SIZE);

	r->rx_cursor = 0;
	r->tx_next   = 0;
	r->tx_clean  = 0;

	// The device is told PHYSICAL addresses, because a bus master has never
	// heard of virtual memory. Low half first, then high — the order does
	// not matter to a stopped engine, but writing the pair as a pair keeps
	// the next reader from wondering whether it did.
	// HIGH BEFORE LOW, and it is not a stylistic preference. Linux's r8169
	// writes the pair in that order under a comment calling it a "magic
	// spell" — the LOW write is what LATCHES the pair, so a HIGH written
	// afterwards may never be taken. The first version of this function had
	// it backwards (2026-08-16), which on the P5 left transmit working and
	// receive stone dead: our physical addresses are below 4GB so the HIGH
	// half is zero either way, but "the value happens to be harmless" and
	// "the register was programmed" are different claims, and only one of
	// them survives a machine that lays its rings out differently.
	r8125_write32(r, R8125_TNPDS_HIGH, (uint32_t)(r->tx_phys >> 32));
	r8125_write32(r, R8125_TNPDS_LOW,  (uint32_t)(r->tx_phys & 0xFFFFFFFF));
	r8125_write32(r, R8125_RDSAR_HIGH, (uint32_t)(r->rx_phys >> 32));
	r8125_write32(r, R8125_RDSAR_LOW,  (uint32_t)(r->rx_phys & 0xFFFFFFFF));

	// And read them back. These are plain read/write registers with no
	// hardware-owned bits, so a mismatch here is unambiguous — it means the
	// descriptor base the chip is walking is NOT the ring we built, which
	// would explain a receive path that never sees a thing.
	uint32_t rx_lo = r8125_read32(r, R8125_RDSAR_LOW);
	uint32_t tx_lo = r8125_read32(r, R8125_TNPDS_LOW);
	if (rx_lo != (uint32_t)(r->rx_phys & 0xFFFFFFFF) ||
	    tx_lo != (uint32_t)(r->tx_phys & 0xFFFFFFFF))
	{
		printf("r8125: descriptor bases did NOT stick — rx wrote 0x%08x read 0x%08x, tx wrote 0x%08x read 0x%08x\n",
		       (uint32_t)(r->rx_phys & 0xFFFFFFFF), rx_lo,
		       (uint32_t)(r->tx_phys & 0xFFFFFFFF), tx_lo);
		return false;
	}
	printf("r8125: descriptor bases confirmed (rx 0x%08x, tx 0x%08x)\n", rx_lo, tx_lo);

	printd(DEBUG_NET, "r8125: rings rx phys 0x%lx tx phys 0x%lx (%u/%u descs, %u-byte buffers)\n",
	       r->rx_phys, r->tx_phys, R8125_RX_DESCS, R8125_TX_DESCS, R8125_BUF_SIZE);
	return true;
}

// ── Transmit ────────────────────────────────────────────────────────────────
// Copy-based, per the seam's ownership contract: the caller's buffer must be
// reusable the moment this returns, so the frame goes into OUR standing
// buffer before the descriptor is handed over. Zero-copy transmit is a real
// optimization with real lifetime headaches (whose page is pinned, until
// which interrupt) and is exactly the cleverness you add after the wire has
// proven the simple thing works.
static int32_t r8125_transmit(struct net_device* dev, const void* frame, uint16_t length)
{
	r8125_t* r = (r8125_t*)dev->driver_data;
	if (r == NULL || !r->present)
		return -1;
	if (length == 0 || length > R8125_BUF_SIZE)
		return -1;

	uint64_t flags = spinlock_acquire_irqsave(&r->lock);

	// Reap first: the ring may be "full" only because we have not collected
	// what the device already finished.
	r8125_tx_reap(r->tx, R8125_TX_DESCS, &r->tx_clean, r->tx_next);

	uint16_t index;
	if (!r8125_tx_claim(R8125_TX_DESCS, r->tx_next, r->tx_clean, &index))
	{
		spinlock_release_irqrestore(&r->lock, flags);
		r->netdev.tx_errors++;
		return -1;   // ring full — the caller's to retry, and it is counted
	}

	memcpy(r->tx_buf + (uint64_t)index * R8125_BUF_SIZE, frame, length);

	// Ethernet's 60-byte minimum: pad short frames with zeroes rather than
	// whatever the last frame left in this buffer. The minimum exists
	// because 1980s collision detection needed a frame still to be going
	// out when the far end's collision signal came back — pure ceremony on
	// a full-duplex link, and still mandatory on the wire.
	uint16_t on_wire = length;
	if (on_wire < 60)
	{
		memset(r->tx_buf + (uint64_t)index * R8125_BUF_SIZE + length, 0, 60 - length);
		on_wire = 60;
	}

	r8125_tx_post(r->tx, R8125_TX_DESCS, index, on_wire);
	r->tx_next = r8125_ring_next(r->tx_next, R8125_TX_DESCS);

	// THE DOORBELL. The descriptor is only a message if somebody tells the
	// device to look. [8125-SPECIFIC] and UNCONFIRMED — if frames queue and
	// never leave, this register and this bit are the first two suspects.
	r8125_write16(r, R8125_TPPOLL_8125, R8125_TPPOLL_NPQ);

	r->netdev.tx_frames++;
	r->netdev.tx_bytes += length;
	spinlock_release_irqrestore(&r->lock, flags);
	return 0;
}

static net_operations_t s_r8125_ops = {
	.transmit = r8125_transmit,
};

// ── The drain ───────────────────────────────────────────────────────────────
void r8125_poll(void)
{
	r8125_t* r = &s_r8125;
	if (!r->present)
		return;

	// Re-entrancy guard, the e1000's pattern: processSignals runs on the
	// core that owns scheduling, but a transmit from another core takes the
	// same lock, and nothing here should ever nest.
	uint64_t flags = spinlock_acquire_irqsave(&r->lock);

	// Transmit completions first — cheap, and it frees slots for whatever
	// the receive path below is about to answer.
	r8125_tx_reap(r->tx, R8125_TX_DESCS, &r->tx_clean, r->tx_next);

	// Then every frame the device has finished with. Bounded per pass: a
	// wire delivering faster than we drain must not let one scheduler pass
	// run the whole ring repeatedly and starve everything else on this core.
	// Whatever is left stays owned by us and is collected next pass.
	for (uint16_t drained = 0; drained < R8125_RX_DESCS; drained++)
	{
		uint16_t length;
		uint16_t frame_length;
		bool damaged;
		if (!r8125_rx_ready(r->rx, r->rx_cursor, &length, &damaged))
			break;   // the device still owns this one — nothing more has landed

		if (damaged ||
		    !r8125_rx_strip_fcs(length, R8125_BUF_SIZE, &frame_length))
		{
			// The wire lied, or the hardware says it did. Drop it exactly as
			// the medium would have, and COUNT it — a lying link should be
			// visible from userland, not a shrug.
			r->netdev.rx_errors++;
		}
		else
		{
			const uint8_t* buf = r->rx_buf + (uint64_t)r->rx_cursor * R8125_BUF_SIZE;
			// Up through the SEAM, never straight to the stack: net_device_rx
			// is where the counters, a future pcap tap, and the eventual
			// kworker hand-off live (net_device.h says so explicitly).
			// Released around the delivery because the stack answers some
			// frames by TRANSMITTING (ARP reply, echo reply), and that path
			// takes this very lock.
			spinlock_release_irqrestore(&r->lock, flags);
			net_device_rx(&r->netdev, buf, frame_length);
			flags = spinlock_acquire_irqsave(&r->lock);
		}

		// Hand the descriptor back empty, then advance. Refill BEFORE the
		// cursor moves so a frame is never counted against a descriptor the
		// device could already be refilling.
		r8125_rx_refill(r->rx, R8125_RX_DESCS, r->rx_cursor,
		                r->rx_buf_phys + (uint64_t)r->rx_cursor * R8125_BUF_SIZE,
		                R8125_BUF_SIZE);
		r->rx_cursor = r8125_ring_next(r->rx_cursor, R8125_RX_DESCS);
	}

	spinlock_release_irqrestore(&r->lock, flags);
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

	// Enable memory decoding but actively DISABLE bus mastering, by a LIVE
	// read-modify-write of the command register — never the cached enum copy.
	// That is the ruling from a848273:
	// the cached word can be stale, and writing a stale command word back is
	// how you strip a neighbour's DMA mid-flight. Firmware may have left this
	// device's engines running with descriptor addresses from the previous
	// boot, so allowing it to master the bus before reset would let stale DMA
	// scribble over the new kernel while these bring-up beacons run.
	uint32_t live = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4) & 0xFFFF;
	writePCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4,
	                 (live | R8125_PCI_COMMAND_MEMORY) & ~R8125_PCI_COMMAND_MASTER);

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

	// BEACON 5: which RTL8125 this actually is. Free, now that we know
	// where the chip keeps its name.
	r8125_report_version(r);

	// BEACON 6: the link.
	r8125_report_link(r);

	// ── Soft reset ──────────────────────────────────────────────────────
	// Set CmdReset and wait for the chip to clear it. Firmware or a warm
	// reboot may have left the engines running with rings pointed at memory
	// that is now somebody else's, so this is not politeness — it is the
	// only way to know what state we are starting from.
	//
	// The wait is a bounded ITERATION count, not a clock read: this runs
	// during init when the tick may not be trustworthy, and a spin that
	// waits on a clock which never advances is a hang rather than a
	// timeout. (The e1000's INTx probe learned exactly this lesson the
	// expensive way.)
	r8125_write8(r, R8125_CHIPCMD, R8125_CMD_RESET);
	bool reset_done = false;
	for (uint32_t spin = 0; spin < 1000000u; spin++)
	{
		if ((r8125_read8(r, R8125_CHIPCMD) & R8125_CMD_RESET) == 0)
		{
			reset_done = true;
			break;
		}
		__asm__ volatile("pause");
	}
	if (!reset_done)
	{
		printf("r8125: chip did not clear its reset bit — refusing to program a chip that is not listening\n");
		return false;
	}
	printf("r8125: soft reset complete\n");

	// Reset has stopped the old engines and erased their stale descriptor
	// state. DMA is safe to authorize now, before setup installs our new ring
	// addresses. Re-read the live word so no intervening config change is
	// overwritten with the snapshot taken before the MMIO probing above.
	live = readPCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4) & 0xFFFF;
	writePCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4,
	                 live | R8125_PCI_COMMAND_MEMORY | R8125_PCI_COMMAND_MASTER);

	// ── Rings ───────────────────────────────────────────────────────────
	if (!r8125_setup_rings(r))
		return false;
	printf("r8125: rings built (%u rx / %u tx descriptors)\n",
	       R8125_RX_DESCS, R8125_TX_DESCS);

	// ── Configuration ───────────────────────────────────────────────────
	// The config registers are write-locked until the 93C46 magic word —
	// a 1990s serial-EEPROM interface still standing guard over a 2020s
	// 2.5GbE part, because these registers were once shadowed from that
	// EEPROM at power-on.
	r8125_write8(r, R8125_CFG9346, R8125_CFG9346_UNLOCK);

	// Largest frame we will accept. Our buffers are 2KB and the MTU is
	// 1500; telling the chip the buffer size is what stops it writing past
	// the end of one.
	r8125_write16(r, R8125_RXMAXSIZE, R8125_BUF_SIZE);

	// Receiver policy, stated deliberately:
	//   BROADCAST — NOT optional and not a nicety. ARP asks the whole
	//     segment "who has this IP?", and a NIC filtering broadcast never
	//     hears the question, never answers, and never gets an answer
	//     either. Every "my stack transmits but nothing comes back" bug in
	//     NIC history has this bit at the bottom of it.
	//   MYPHYS — unicast addressed to us.
	//   NOT ALLPHYS (promiscuous): we are not a sniffer.
	//   NOT MULTICAST: nothing here has asked for a group yet.
	r8125_write32_verify(r, R8125_RXCONFIG,
	                     R8125_RX_FETCH_8125 | R8125_RX_DMA_BURST |
	                     R8125_RX_ACCEPT_BROADCAST | R8125_RX_ACCEPT_MYPHYS,
	                     R8125_RXCONFIG_HW_OWNED, "RxConfig");

	// TxConfig's hardware-owned mask is mostly the version ID we just
	// printed, plus the top bit of the DMA-burst field: the P5 read back
	// 0xf00 where we wrote 0x700, i.e. this generation's MXDMA is four bits
	// wide and the chip widens "unlimited" for us.
	r8125_write32_verify(r, R8125_TXCONFIG, R8125_TX_CONFIG_DEFAULT,
	                     R8125_TXCONFIG_HW_OWNED, "TxConfig");

	// Every interrupt masked. This driver POLLS (RTL8125.md's phased plan:
	// prove the silicon moves frames before asking whether an interrupt
	// routes). An unmasked legacy INTx line shared with another device would
	// mean a level-triggered interrupt nobody ever acknowledges — which
	// wedges that line for its rightful owner too.
	r8125_write32(r, R8125_IMR0_8125, 0);
	r8125_write32(r, R8125_ISR0_8125, 0xFFFFFFFF);   // clear anything pending

	// OPEN THE RECEIVE GATE. See R8125_RXDV_GATED above: this generation can
	// come out of reset with receive gated, which presents exactly as the
	// P5's first bring-up did — transmit fine, not one frame in. Read-modify
	// -write so we clear only this bit and leave the rest of MISC alone,
	// whatever else lives there on this part.
	// (Measured innocent on the P5 — see R8125_RXDV_GATED. Kept because the
	// reference driver does it and another board may yet come up gated.)
	uint32_t misc_before = r8125_read32(r, R8125_MISC);
	r8125_write32(r, R8125_MISC, misc_before & ~R8125_RXDV_GATED);
	uint32_t misc_after = r8125_read32(r, R8125_MISC);
	printf("r8125: rx gate: MISC 0x%08x -> 0x%08x (RXDV_GATED %s)\n",
	       misc_before, misc_after,
	       (misc_after & R8125_RXDV_GATED) ? "STILL SET — receive will stay dead"
	                                       : "clear");

	// Engines on, THEN relock the config space.
	r8125_write8(r, R8125_CHIPCMD, R8125_CMD_TX_ENABLE | R8125_CMD_RX_ENABLE);
	r8125_write8(r, R8125_CFG9346, R8125_CFG9346_LOCK);

	uint8_t cmd = r8125_read8(r, R8125_CHIPCMD);
	if ((cmd & (R8125_CMD_TX_ENABLE | R8125_CMD_RX_ENABLE)) !=
	    (R8125_CMD_TX_ENABLE | R8125_CMD_RX_ENABLE))
	{
		// The chip declined to start. Say exactly what it reads back —
		// on a machine with no debugger, that byte is the investigation.
		printf("r8125: engines did not start (ChipCmd reads 0x%02x, wanted Tx|Rx set)\n", cmd);
		return false;
	}
	printf("r8125: tx/rx enabled (ChipCmd 0x%02x)\n", cmd);

	r->netdev.ops = &s_r8125_ops;
	r->netdev.mtu = 1500;
	r->netdev.driver_data = r;
	r->netdev.name[0] = 'r'; r->netdev.name[1] = '8'; r->netdev.name[2] = '1';
	r->netdev.name[3] = '2'; r->netdev.name[4] = '5'; r->netdev.name[5] = '0';
	r->netdev.name[6] = '\0';

	// NOW it may join the seam. The earlier slice deliberately withheld
	// this: a net_device in kNetDevices is a promise the stack believes —
	// ARP hands it frames, dhcp_start dials through it — and registering a
	// device that cannot transmit would have converted "no driver" into "a
	// driver that silently drops everything", which is strictly worse. The
	// promise is only made now that transmit is real.
	//
	// present goes true FIRST: registration makes us reachable, and
	// r8125_poll must be willing to drain the moment anything can arrive.
	r->present = true;
	if (net_device_register(&r->netdev) != 0)
	{
		printf("r8125: net_device_register refused (device table full?)\n");
		r->present = false;
		return false;
	}

	printf("r8125: registered as %s — %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
	       r->netdev.name,
	       r->netdev.mac[0], r->netdev.mac[1], r->netdev.mac[2],
	       r->netdev.mac[3], r->netdev.mac[4], r->netdev.mac[5],
	       r->netdev.link_up ? "up" : "down");
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
