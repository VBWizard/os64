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
//   - frames queue but never leave → the doorbell (register 0x90, bit 0)
//     — confirmed against the vendor driver and proven on the P5, so look
//     at the ring state before the register
//   - nothing ever arrives → the broadcast accept bit, or the rings' base
//     registers. ARP dies without broadcast and takes everything with it
//   - link at the wrong speed → the boot log prints both sides of the
//     negotiation (r8125_phy_configure), and since 2026-09-05 the driver
//     restores a stripped advertisement and renegotiates once when the
//     link is below what both sides offer. Both offering 1000 and the
//     link still 100 after that is a training failure — DEBTS' PHY
//     firmware row, not the advertisement

#include "driver/net/r8125.h"
#include "driver/net/r8125_ring.h"
#include "driver/net/r8125_phy.h"
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
#include "strings/sprintf.h"   // snprintf — netdev.location, "02:00.0"

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
// Provenance tags per r8125.h. [8169-family] = shared across the lineage.
// [8125-SPECIFIC] = moved or widened in this generation. The offsets and
// bits defined here were checked against Realtek's GPL r8125 driver
// (r8125.h, the awesometic/realtek-r8125-dkms mirror) on 2026-09-05; the
// ones the P5 had already proven by moving frames were confirmed on paper
// too. The read-back checks (r8125_write32_verify) stay, because a
// confirmed offset on a different board revision is still worth one read.

#define R8125_MAC0        0x00   // [8169-family] 6 bytes, burned-in address
#define R8125_CHIPCMD     0x37   // [8169-family] 8-bit: reset / Tx enable / Rx enable
#define R8125_TXCONFIG    0x40   // [8169-family] 32-bit
#define R8125_RXCONFIG    0x44   // [8169-family] 32-bit
#define R8125_CFG9346     0x50   // [8169-family] 8-bit: config-register lock
#define R8125_PHYSTATUS   0x6C   // [8169-family] address; 32-bit on the 8125 — the
                                 // 2.5G answers sit above the byte the 8169 had
                                 // (bits in r8125_phy.h, R8125_PHYS_*)
#define R8125_RXMAXSIZE   0xDA   // [8169-family] 16-bit (RMS)
#define R8125_RDSAR_LOW   0xE4   // [8169-family] 32-bit: RX descriptor base
#define R8125_RDSAR_HIGH  0xE8   // [8169-family]
#define R8125_TNPDS_LOW   0x20   // [8169-family] 32-bit: TX descriptor base
#define R8125_TNPDS_HIGH  0x24   // [8169-family]

// The three that MOVED. On the 8169/8168 the interrupt mask/status were
// 16-bit at 0x3C/0x3E and the transmit doorbell (TxPoll) was 8-bit at 0x38.
// The 8125 widened the interrupt pair to 32 bits and relocated it to
// 0x38/0x3C — which it could only do because the doorbell moved OUT to
// 0x90. Vendor names: IMR0_8125, ISR0_8125, TPPOLL_8125, same three values.
#define R8125_IMR0_8125   0x38   // [8125-SPECIFIC] 32-bit interrupt mask
#define R8125_ISR0_8125   0x3C   // [8125-SPECIFIC] 32-bit interrupt status
#define R8125_TPPOLL_8125 0x90   // [8125-SPECIFIC] 16-bit transmit doorbell

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
// [8125-SPECIFIC]. The vendor's rtl8125_disable_rxdvgate clears bit 3 of
// the BYTE at 0xF2, which is bit 19 of the 32-bit word at 0xF0 — the same
// bit by a different address width, and Linux's r8169 spells it as this
// driver does (MISC = 0xF0, RXDV_GATED_EN = BIT(19)).
#define R8125_MISC        0xF0        // [8125-SPECIFIC] 32-bit
#define R8125_RXDV_GATED  (1u << 19)  // 1 = receive gated OFF

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

// The DMA-burst / fetch fields. [8125-SPECIFIC]: the 8169 generation put an
// unlimited DMA burst at (7 << 8) (vendor: RX_DMA_BURST_unlimited <<
// RxCfgDMAShift), and the 8125 adds a fetch-count bit the vendor names
// Rx_Fetch_Number_8 at bit 30. Still read back and reported at init — see
// r8125_write32_verify.
#define R8125_RX_DMA_BURST   (7u << 8)
#define R8125_RX_FETCH_8125  (1u << 30)

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

// The transmit doorbell's poke bit. [8125-SPECIFIC] — on the 8169 this was
// NPQ (0x40) in an 8-bit register at 0x38; on the 8125 the doorbell is a
// 16-bit register at 0x90 with ONE BIT PER TRANSMIT QUEUE (the vendor
// writes BIT(ring->index)). This driver has one queue, queue 0.
#define R8125_TPPOLL_QUEUE0 0x0001

typedef struct
{
	pci_device_t* pci;
	volatile uint8_t* regs;
	net_device_t netdev;
	bool present;

	// IDENTITY, kept for the ONE line bring-up prints on success (2026-08-20).
	// The bring-up used to narrate itself to the GLASS in eleven lines —
	// right when this chip met this code on exactly one machine in the world
	// and every step had to say what it did. That debt is paid: the P5 boots
	// it daily, and it has a serial wire now (the very wire this driver
	// carries). So the narration moved to the log and these fields let the
	// screen answer the only question a healthy boot asks — WHAT is this
	// machine and is it up. Set on the way past by the reporters below.
	const char* model;        // "RTL8125B" — from the TxConfig hardware version
	uint8_t bus, slot, func;  // where it lives on the PCI tree
	// (speed/duplex used to live here as strings; they are seam fields now —
	//  net_device_t's link_mbps/full_duplex — so /sys/net can print them for
	//  any card instead of only this driver's own boot line.)
	uint32_t phystatus;       // the last PHYstatus word read, verbatim — the
	                          // boot line and the link-change log print it
	                          // beside the decode, so a link the decode cannot
	                          // name still leaves the number that explains it
	bool phy_ok;              // the PHY OCP window answered and the PHY says
	                          // Realtek — the gate on ever WRITING the PHY

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

	// ── THE STALL WATCHDOG (2026-08-22) ─────────────────────────────────
	//
	// Added the day a P5 `os64get -a` froze three to six files into an
	// 86-file refresh, four times running, taking the whole network with it
	// — ping included — while every other part of the machine stayed
	// perfectly healthy. Chris's report came with the right complaint
	// attached: "there's nothing in os64.log about it... for show-stopper
	// issues there shouldn't need to be a debug on."
	//
	// He is right, and this driver was a good example of the wrong habit: a
	// transmit that cannot get a descriptor bumped tx_errors and returned
	// -1, silently. A counter nobody is watching is not a diagnostic. Worse,
	// the counter only moves once the ring is ALREADY full, which is dozens
	// of frames after the thing that actually went wrong.
	//
	// So these fields watch for the two shapes a dead 8125 can take, and say
	// so ONCE, loudly, on DEBUG_EXCEPTIONS — the bit CONFIG.h keeps
	// permanently lit precisely so must-never-be-silent messages have
	// somewhere to go:
	//
	//   TX WEDGED   descriptors handed over and NONE coming back. The
	//               device has stopped completing; the ring fills and then
	//               every transmit fails forever.
	//   DEAF        we are transmitting steadily and nothing at all is
	//               arriving. The link, the far end, or the receive path is
	//               gone. (Silence while IDLE is not a fault, which is why
	//               this is measured against our own transmits and not
	//               against the clock.)
	uint64_t tx_completions;      // total descriptors reaped, ever
	uint64_t last_progress_tick;  // when a completion or an arrival last happened
	uint32_t tx_since_rx;         // frames sent since the last frame received
	bool     stall_reported;      // one-shot, so a dead wire cannot flood the log
	uint64_t isr_acks;            // how many times the status register was cleared
	uint32_t isr_last;            // the last non-zero status seen
	bool     isr_trouble_reported;// one-shot for the alarming bits
	// Overruns are COUNTED as well as announced, because their frequency is
	// the whole question once acknowledging has made them survivable: one
	// during a big transfer is the wire briefly outrunning us, and hundreds
	// is a receive ring that is too small for this card's speed. The
	// announcement is one-shot; this number is not.
	uint64_t isr_overrun_events;
	uint64_t isr_overrun_next_report;   // the next decade milestone to announce

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
// RTL8125B (the vendor's CFG_METHOD_4). That matters beyond curiosity: it
// is what let the register map be checked against a SPECIFIC part rather
// than against a family.
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
	r->model = model;
	printd(DEBUG_NET, "r8125: chip id 0x%03x (%s), TxConfig reads 0x%08x\n", id, model, txcfg);
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
// Read PHYstatus, decode it onto the seam, keep the raw word. Called at
// bring-up (as found, and again once the PHY has been told what to
// advertise) and from the poll when the chip reports a link change — so the
// seam and /sys/net say what the link IS, not what it was at boot.
//
// ON THE SEAM, not in this driver's private struct (2026-08-20): speed and
// duplex lived here as a string for the boot line only, which is exactly why
// /sys/net could show a speed for no card at all. link_mbps 0 = up but not
// decodable, and the raw word beside it is what explains it.
static void r8125_read_link(r8125_t* r)
{
	uint32_t status = r8125_read32(r, R8125_PHYSTATUS);
	r8125_link_t link = r8125_phy_decode_status(status);
	r->phystatus          = status;
	r->netdev.link_up     = link.up;
	r->netdev.link_mbps   = link.mbps;
	r->netdev.full_duplex = link.full_duplex;
	snprintf(r->netdev.link_raw, sizeof(r->netdev.link_raw), "0x%08x", status);
}

// "1000/full", "up, speed not decoded", or "DOWN" — one spelling for the
// boot line, the negotiation lines and the link-change line, so a reader
// grepping the log for one of them finds all of them.
#define R8125_LINK_WORDS_CAP 32
static void r8125_link_words(const r8125_t* r, char* out, uint32_t cap)
{
	if (!r->netdev.link_up)
		snprintf(out, cap, "DOWN");
	else if (r->netdev.link_mbps == 0)
		snprintf(out, cap, "up, speed not decoded");
	else
		snprintf(out, cap, "%u/%s", r->netdev.link_mbps,
		         r->netdev.full_duplex ? "full" : "half");
}

// ── The PHY ─────────────────────────────────────────────────────────────────
//
// WHY THE DRIVER TALKS TO THE PHY (2026-09-05): the P5 linked at 100/full
// on a gigabit switch — four cables, a cold boot, the switch's own LED
// agreeing — and this driver had never written a PHY register in its life.
// It read one status byte and reported whatever negotiation the PHY's
// power-on state had produced. Linux's r8169 never trusts that state:
// genphy_config_aneg rewrites the advertisement and restarts negotiation at
// every probe, which is why Linux users on the same silicon never see a
// stuck 100M link. This block does the same, and PRINTS BOTH SIDES OF THE
// NEGOTIATION first, so a wrong speed on the P5 is diagnosable from the log
// rather than guessed at from a switch LED.
//
// The window and the register map are r8125_phy.h's business — verified
// against the vendor driver, host-tested. This is only the part that needs
// the silicon: the MMIO shuttle, the bounded waits, and the log lines.
//
// WHAT IS DELIBERATELY NOT DONE HERE, booked in DEBTS.md: the vendor's
// hw_phy_config (a firmware blob plus thousands of per-revision register
// pokes) and any change to the pause advertisement. Standard MII registers
// only; one behaviour change per slice, so the attribution survives.

// An OCP transaction completes in microseconds; the vendor allows 20ms.
// Iteration-counted like the reset wait (one MMIO read per iteration is a
// ceiling of tens of milliseconds on any PCIe bus), so a starved tick can
// only slow it, never hang it.
#define R8125_PHY_OCP_SPINS 100000u

// The wait for the link after an autoneg restart. Gigabit negotiation takes
// one to three seconds against a real switch. TWO bounds, whichever ends
// first: the tick is the intended ceiling, the iteration count (each one an
// OCP read of microseconds) is the backstop for a tick that is not
// advancing — the e1000's INTx probe met exactly that.
#define R8125_PHY_LINK_WAIT_TICKS (5 * TICKS_PER_SECOND)
#define R8125_PHY_LINK_WAIT_SPINS 500000u
// How long a BMCR restart may take to show — BMSR dropping "complete", or the
// MAC seeing the link fall — before the driver stops
// waiting for it, and how long the link-partner page may lag a completed
// negotiation (both loops in r8125_phy_configure). Generous: a PHY that is
// going to react does so in milliseconds.
#define R8125_PHY_RESTART_TAKE_TICKS (1 * TICKS_PER_SECOND)
#define R8125_PHY_PARTNER_WAIT_TICKS (1 * TICKS_PER_SECOND)

static bool r8125_phy_read(r8125_t* r, uint16_t ocp_addr, uint16_t* value_out)
{
	r8125_write32(r, R8125_REG_PHYOCP, r8125_phy_ocp_read_command(ocp_addr));
	for (uint32_t spin = 0; spin < R8125_PHY_OCP_SPINS; spin++)
	{
		uint32_t word = r8125_read32(r, R8125_REG_PHYOCP);
		if (word & R8125_PHYOCP_FLAG)
		{
			*value_out = (uint16_t)(word & R8125_PHYOCP_DATA_MASK);
			return true;
		}
		__asm__ volatile("pause");
	}
	*value_out = 0xFFFF;   // what a dead window reads as, so a caller that
	                       // ignores the verdict still sees an impossible value
	return false;
}

static bool r8125_phy_write(r8125_t* r, uint16_t ocp_addr, uint16_t value)
{
	r8125_write32(r, R8125_REG_PHYOCP, r8125_phy_ocp_write_command(ocp_addr, value));
	for (uint32_t spin = 0; spin < R8125_PHY_OCP_SPINS; spin++)
	{
		if ((r8125_read32(r, R8125_REG_PHYOCP) & R8125_PHYOCP_FLAG) == 0)
			return true;
		__asm__ volatile("pause");
	}
	return false;
}

static bool r8125_phy_mii_read(r8125_t* r, uint8_t mii_reg, uint16_t* value_out)
{
	return r8125_phy_read(r, r8125_phy_mii_ocp_addr(mii_reg), value_out);
}

static bool r8125_phy_mii_write(r8125_t* r, uint8_t mii_reg, uint16_t value)
{
	return r8125_phy_write(r, r8125_phy_mii_ocp_addr(mii_reg), value);
}

// The negotiation registers, both sides, in one read — the ones a person
// needs to answer "why did this link at that speed?".
typedef struct
{
	uint16_t bmcr, bmsr, estatus;
	uint16_t anar, gbcr, adv2500;      // ours
	uint16_t anlpar, gbsr, lpa2500;    // the partner's
} r8125_phy_snapshot_t;

static bool r8125_phy_snapshot(r8125_t* r, r8125_phy_snapshot_t* s)
{
	bool ok = true;
	ok = r8125_phy_mii_read(r, R8125_MII_BMCR,    &s->bmcr)    && ok;
	ok = r8125_phy_mii_read(r, R8125_MII_BMSR,    &s->bmsr)    && ok;
	ok = r8125_phy_mii_read(r, R8125_MII_ESTATUS, &s->estatus) && ok;
	ok = r8125_phy_mii_read(r, R8125_MII_ANAR,    &s->anar)    && ok;
	ok = r8125_phy_mii_read(r, R8125_MII_GBCR,    &s->gbcr)    && ok;
	ok = r8125_phy_read(r, R8125_PHY_OCP_ADV_2500, &s->adv2500) && ok;
	ok = r8125_phy_mii_read(r, R8125_MII_ANLPAR,  &s->anlpar)  && ok;
	ok = r8125_phy_mii_read(r, R8125_MII_GBSR,    &s->gbsr)    && ok;
	ok = r8125_phy_read(r, R8125_PHY_OCP_LPA_2500, &s->lpa2500) && ok;
	return ok;
}

// Print a snapshot: the registers verbatim on one line, the negotiation in
// words on the next. DEBUG_BOOT and not DEBUG_NET, deliberately: the P5's
// boot entries carry no DEBUG_NET, and the P5 is the only machine on which
// these lines are ever true. A link at the wrong speed is diagnosed from
// exactly these numbers, and a diagnosis that lands only in a log nobody
// has switched on is no diagnosis. Negotiation happens once per link, which
// is what makes it a boot fact rather than traffic.
static void r8125_phy_print(r8125_t* r, const char* when, const r8125_phy_snapshot_t* s)
{
	char ours[R8125_ABILITY_TEXT_CAP], theirs[R8125_ABILITY_TEXT_CAP];
	char link[R8125_LINK_WORDS_CAP];
	r8125_phy_abilities_text(r8125_phy_abilities_ours(s->anar, s->gbcr, s->adv2500),
	                         ours, sizeof ours);
	uint8_t partner = r8125_phy_abilities_partner(s->anlpar, s->gbsr, s->lpa2500);
	if (partner == 0 && r->netdev.link_up)
		// A link exists, so the partner offered SOMETHING; an empty page is
		// the PHY not having reported it (the settle loop's story), never a
		// partner with nothing to say. Print the truth we have, not "none".
		snprintf(theirs, sizeof theirs, "(not reported by the PHY)");
	else
		r8125_phy_abilities_text(partner, theirs, sizeof theirs);
	r8125_link_words(r, link, sizeof link);

	printd(DEBUG_BOOT, "r8125: PHY %s: BMCR 0x%04x BMSR 0x%04x ESTATUS 0x%04x | "
	       "ANAR 0x%04x GBCR 0x%04x 2.5G-adv 0x%04x | ANLPAR 0x%04x GBSR 0x%04x 2.5G-lpa 0x%04x\n",
	       when, s->bmcr, s->bmsr, s->estatus,
	       s->anar, s->gbcr, s->adv2500, s->anlpar, s->gbsr, s->lpa2500);
	printd(DEBUG_BOOT, "r8125: PHY %s: we advertise %s, partner offers %s, link %s (PHYstatus 0x%08x)\n",
	       when, ours, theirs, link, r->phystatus);
}

// Put the three advertisement registers back to what a snapshot found. Best
// effort by design: it runs on paths where a write has already failed, and
// a window that stopped answering may refuse these too — the outcome is
// printed by the caller either way, and the link stands as it was.
static void r8125_phy_restore_advertisement(r8125_t* r, const r8125_phy_snapshot_t* found)
{
	r8125_phy_mii_write(r, R8125_MII_ANAR, found->anar);
	r8125_phy_mii_write(r, R8125_MII_GBCR, found->gbcr);
	r8125_phy_write(r, R8125_PHY_OCP_ADV_2500, found->adv2500);
}

// Identify the PHY, print the negotiation as found, set the advertisement,
// and — only if that changed anything — restart negotiation and wait for
// the link. Best effort throughout: a PHY that cannot be read is reported
// and left alone, and the MAC bring-up continues on whatever link the
// power-on state negotiated. The wire still moves frames at 100M; a driver
// that refused to run because it could not IMPROVE the link would be
// strictly worse than the one that linked at 100/full all summer.
static void r8125_phy_configure(r8125_t* r)
{
	uint16_t id1, id2;
	if (!r8125_phy_mii_read(r, R8125_MII_PHYID1, &id1) ||
	    !r8125_phy_mii_read(r, R8125_MII_PHYID2, &id2))
	{
		printf("r8125: PHY window (0x%02x) did not answer — leaving the PHY as found\n",
		       R8125_REG_PHYOCP);
		return;
	}
	if (!r8125_phy_id_is_realtek(id1, id2))
	{
		// Not refusing to run — refusing to WRITE. A window that is not the
		// PHY's reads as all ones or all zeroes, and advertising into that
		// would be programming a register we cannot see.
		printf("r8125: PHY id 0x%04x%04x is not Realtek's — leaving the PHY as found\n", id1, id2);
		return;
	}
	r->phy_ok = true;
	printd(DEBUG_BOOT, "r8125: PHY id 0x%04x%04x (Realtek) through the OCP window\n", id1, id2);

	r8125_phy_snapshot_t found;
	if (!r8125_phy_snapshot(r, &found))
	{
		printf("r8125: PHY registers stopped answering mid-read — leaving the PHY as found\n");
		return;
	}
	// The link as it stands NOW, read beside the registers, not the read
	// from before the MAC soft reset: a negotiation can finish between the
	// two, and a decision about the link has to be made from the same moment
	// as the registers it is compared with (Codex, PR #66).
	r8125_read_link(r);
	r8125_phy_print(r, "as found", &found);

	// THREE REASONS TO RENEGOTIATE, and a healthy boot has none.
	//
	// (1) The advertisement is not what we want — a PHY somebody left at
	//     10/100, or one missing gigabit. Write it, then restart.
	// (2) The PHY was left OUT OF SERVICE: autonegotiation disabled (a
	//     forced mode), powered down, isolated, or in loopback — states
	//     firmware can leave behind and this driver never wants. The one
	//     BMCR write below clears all of them. Without this reason a PHY
	//     whose advertisement already matched would have kept whatever mode
	//     it was left in (Codex, PR #66).
	// (3) The advertisement is fine but the LINK IS BELOW what both sides
	//     offer — speed AND duplex, in annex 28B's priority order: 100/full
	//     between two parties that both listed 1000F is a negotiation that
	//     went wrong, and so is 100/half between two that both listed
	//     100/full (Codex, PR #66 round 3). Not a slow partner; one restart
	//     is what every OS tries before anyone blames a cable. This is the
	//     shape the P5 reported on 2026-09-05, and the "as found" dump is
	//     what makes it decidable rather than guessed.
	//
	// A boot where the PHY already advertises everything, is in service, and
	// negotiated the best common mode touches nothing — restarting a
	// negotiation that produced the right answer would only drop the link
	// for nothing, and a link dropped at init is a link the post-boot
	// network tests run on.
	r8125_phy_adv_t have = { found.anar, found.gbcr, found.adv2500 }, want;
	bool rewrite = r8125_phy_plan_advertisement(&have, &want, R8125_ADVERTISE_2500 != 0);
	const char* why = rewrite ? "the advertisement changed" : NULL;
	if (why == NULL && r8125_phy_bmcr_out_of_service(found.bmcr))
		why = "the PHY was left out of autonegotiating service";
	if (why == NULL && r->netdev.link_up && r->netdev.link_mbps != 0)
	{
		uint32_t best = r8125_phy_best_common_rank(
			r8125_phy_abilities_ours(found.anar, found.gbcr, found.adv2500),
			r8125_phy_abilities_partner(found.anlpar, found.gbsr, found.lpa2500));
		uint32_t linked = r8125_phy_mode_rank(r->netdev.link_mbps, r->netdev.full_duplex);
		if (best != R8125_MODE_RANK_NONE && linked < best)
			why = "the link is below what both sides offer";
	}
	if (why == NULL)
	{
		printd(DEBUG_BOOT, "r8125: PHY advertisement is already what we want, the PHY is in "
		       "service and the link is the best mode both sides offer — not renegotiating\n");
		return;
	}

	if (rewrite)
	{
		// All three, then read back before restarting: a PHY that declines
		// a bit (a part that cannot do what was asked) shows here, and a
		// write that stalls mid-way leaves a mixture that must not be
		// negotiated.
		// A mixture must never be left in the PHY, let alone negotiated: a
		// write that stalls after ANAR took, or a readback that differs,
		// both put back what was found (best effort — a window that stopped
		// answering may refuse the restore too) and leave the link standing
		// as it was. Restarting on a mixture would drop a working link and
		// bring it back without the bit the PHY declined; leaving a mixture
		// unrestarted would negotiate it at the next cable event (Codex, PR
		// #66, both rounds).
		if (!r8125_phy_mii_write(r, R8125_MII_ANAR, want.anar) ||
		    !r8125_phy_mii_write(r, R8125_MII_GBCR, want.gbcr) ||
		    !r8125_phy_write(r, R8125_PHY_OCP_ADV_2500, want.adv2500))
		{
			printf("r8125: PHY advertisement write did not complete — restoring it as found, not renegotiating\n");
			r8125_phy_restore_advertisement(r, &found);
			return;
		}
		r8125_phy_snapshot_t set;
		if (!r8125_phy_snapshot(r, &set))
		{
			printf("r8125: PHY registers stopped answering after the write — restoring it as found, not renegotiating\n");
			r8125_phy_restore_advertisement(r, &found);
			return;
		}
		if (set.anar != want.anar || set.gbcr != want.gbcr || set.adv2500 != want.adv2500)
		{
			printf("r8125: PHY kept a different advertisement than written (ANAR 0x%04x/0x%04x GBCR 0x%04x/0x%04x 2.5G 0x%04x/0x%04x, wrote/read) — restoring it as found, not renegotiating\n",
			       want.anar, set.anar, want.gbcr, set.gbcr, want.adv2500, set.adv2500);
			r8125_phy_restore_advertisement(r, &found);
			return;
		}
	}

	// The state THIS restart will be judged against, read right before the
	// write: "taken" below means a transition from here, never a level that
	// may have been low already.
	uint16_t bmsr_before = found.bmsr;
	(void)r8125_phy_mii_read(r, R8125_MII_BMSR, &bmsr_before);
	uint32_t phystatus_before = r8125_read32(r, R8125_PHYSTATUS);

	printd(DEBUG_BOOT, "r8125: PHY renegotiating because %s\n", why);
	if (!r8125_phy_mii_write(r, R8125_MII_BMCR, R8125_BMCR_RESTART_AUTONEG))
	{
		printf("r8125: PHY autonegotiation restart did not complete\n");
		return;
	}

	// FIRST WAIT FOR THE RESTART TO TAKE. The old negotiation's "complete"
	// bit and the MAC's link bit stay readable for a moment after the BMCR
	// write, and a wait that accepts them returns before the link has even
	// dropped — the P5 did exactly that on 2026-09-05: "took 0 ticks (0
	// polls)", a snapshot taken mid-teardown (partner page already emptied,
	// restart bit still pending), and then the real drop landed on the
	// post-boot network tests. "Taken" is evidence tied to THIS write
	// (r8125_phy_restart_taken, host-tested): the self-clearing restart bit
	// reading back clear, complete going set-to-clear, or the link going
	// up-to-down, each against the state captured above. A link that was
	// already down, or a negotiation already in flight, shows none of those
	// until the restart really initiates — and an older negotiation
	// completing meanwhile shows the opposite transitions, which do not
	// count (Codex, PR #66 round 2).
	//
	// A READ THAT TIMES OUT ENDS THE WAIT. Each failed OCP transaction has
	// already spent its own bound (R8125_PHY_OCP_SPINS polls); a loop that
	// retried it R8125_PHY_LINK_WAIT_SPINS times would spend fifty billion
	// register reads with the tick stalled — the very condition the
	// iteration bound exists for — and that is a boot hang, not a timeout
	// (Codex, PR #66 round 3). So a window that stops answering ends every
	// phase below at once, says so, and leaves the link as it stands.
	uint64_t started = kTicksSinceStart;
	uint32_t spins = 0;
	bool restarted = false;
	bool window_dead = false;
	for (; spins < R8125_PHY_LINK_WAIT_SPINS; spins++)
	{
		uint16_t bmcr, bmsr;
		if (!r8125_phy_mii_read(r, R8125_MII_BMCR, &bmcr) ||
		    !r8125_phy_mii_read(r, R8125_MII_BMSR, &bmsr))
		{
			window_dead = true;
			break;
		}
		if (r8125_phy_restart_taken(bmsr_before, phystatus_before,
		                            bmcr, bmsr, r8125_read32(r, R8125_PHYSTATUS)))
		{
			restarted = true;
			break;
		}
		if (kTicksSinceStart - started >= R8125_PHY_RESTART_TAKE_TICKS)
			break;
		__asm__ volatile("pause");
	}

	// THEN WAIT FOR IT TO FINISH — but only a restart that was SEEN to take
	// has a finish to wait for. If neither sign appeared, the old completion
	// is still what the registers say, and a loop that accepted it would
	// report the old link as a new one: the exact failure the two halves
	// exist to prevent. So "not observed" is a timeout, and the link stands
	// as found; a PHY that applies the write late drops the link later, and
	// the poll reports that as the link change it is (Codex, PR #66).
	// Otherwise: the link comes back one to three seconds later on a gigabit
	// partner, and a boot line printed in between would say DOWN about a
	// wire that is fine. Complete AND the MAC sees the link. Either bound
	// ends the wait; a timeout is reported, not fatal.
	bool negotiated = false;
	if (window_dead)
		;   // reported below, once
	else if (!restarted)
		printd(DEBUG_BOOT, "r8125: PHY never showed the restart taking within %lu ticks (restart bit "
		       "still set, no complete-to-incomplete, no link fall) — not waiting for a completion "
		       "that would be the old negotiation's\n", kTicksSinceStart - started);
	else
	{
		for (; spins < R8125_PHY_LINK_WAIT_SPINS; spins++)
		{
			uint16_t bmsr;
			if (!r8125_phy_mii_read(r, R8125_MII_BMSR, &bmsr))
			{
				window_dead = true;
				break;
			}
			if ((bmsr & R8125_BMSR_ANEGCOMPLETE) &&
			    (r8125_read32(r, R8125_PHYSTATUS) & R8125_PHYS_LINK))
			{
				negotiated = true;
				break;
			}
			if (kTicksSinceStart - started >= R8125_PHY_LINK_WAIT_TICKS)
				break;
			__asm__ volatile("pause");
		}
	}

	// The restart empties the partner page and the new exchange writes it
	// back; completion may be visible a moment before the page is, so give
	// it a bounded moment. The print says so honestly if it stays empty.
	uint32_t settle = 0;
	if (negotiated)
	{
		uint64_t settle_started = kTicksSinceStart;
		for (; settle < R8125_PHY_LINK_WAIT_SPINS; settle++)
		{
			uint16_t anlpar;
			if (!r8125_phy_mii_read(r, R8125_MII_ANLPAR, &anlpar))
			{
				window_dead = true;
				break;
			}
			if (anlpar != 0)
				break;
			if (kTicksSinceStart - settle_started >= R8125_PHY_PARTNER_WAIT_TICKS)
				break;
			__asm__ volatile("pause");
		}
	}

	if (window_dead)
	{
		printf("r8125: PHY window stopped answering while waiting for the renegotiation — the link stands as it is\n");
		r8125_read_link(r);
		return;
	}

	r8125_read_link(r);
	r8125_phy_snapshot_t after;
	if (!r8125_phy_snapshot(r, &after))
	{
		printf("r8125: PHY registers stopped answering after renegotiation\n");
		return;
	}
	r8125_phy_print(r, negotiated ? "renegotiated"
	                  : restarted ? "renegotiation TIMED OUT" : "renegotiation NOT OBSERVED", &after);
	printd(DEBUG_BOOT, "r8125: PHY renegotiation took %lu ticks (%u polls, %u more for the partner page)%s\n",
	       kTicksSinceStart - started, spins, settle,
	       negotiated ? "" : " — no new link yet; the poll reports it if it comes up later");
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
	// proven (the ring tests in tools/test_r8125_host.c assert the EOR
	// invariant after every operation).
	r8125_ring_init_rx(r->rx, R8125_RX_DESCS, r->rx_buf_phys, R8125_BUF_SIZE);
	r8125_ring_init_tx(r->tx, R8125_TX_DESCS, r->tx_buf_phys, R8125_BUF_SIZE);

	r->rx_cursor = 0;
	r->tx_next   = 0;
	r->tx_clean  = 0;

	// Watchdog and status bookkeeping start here too. The decade counter
	// starts at 1 so the FIRST overrun is announced; every zero below is
	// already zero (s_r8125 is static) and is written anyway, because a
	// driver that can be re-initialized should not depend on that.
	r->tx_completions = 0;
	r->last_progress_tick = kTicksSinceStart;
	r->tx_since_rx = 0;
	r->stall_reported = false;
	r->isr_acks = 0;
	r->isr_last = 0;
	r->isr_trouble_reported = false;
	r->isr_overrun_events = 0;
	r->isr_overrun_next_report = 1;

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
	printd(DEBUG_NET, "r8125: descriptor bases confirmed (rx 0x%08x, tx 0x%08x)\n", rx_lo, tx_lo);

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
		// RING FULL, AND SAYING SO. This used to bump a counter and return
		// -1 in silence, which meant the machine could lose its network
		// completely and leave nothing behind but a number nobody had
		// reason to read. Every transmit from here on will fail the same
		// way, so this is not a hiccup — it is the network ending.
		//
		// DEBUG_EXCEPTIONS because it is always lit (CONFIG.h's ruling), and
		// one-shot because a stuck ring would otherwise write this line for
		// every packet the stack still hopefully hands down.
		bool first = !r->stall_reported;
		r->stall_reported = true;
		uint64_t completions = r->tx_completions;
		uint16_t next = r->tx_next, clean = r->tx_clean;
		spinlock_release_irqrestore(&r->lock, flags);
		r->netdev.tx_errors++;
		if (first)
			printd(DEBUG_EXCEPTIONS,
			       "r8125: TRANSMIT RING FULL — the device has stopped completing descriptors. "
			       "tx_next=%u tx_clean=%u of %u, %lu completions since boot, %lu frames sent. "
			       "THE NETWORK IS DOWN until this clears; read /sys/net for the counters.\n",
			       next, clean, (unsigned)R8125_TX_DESCS,
			       completions, r->netdev.tx_frames);
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
	// device to look.
	r8125_write16(r, R8125_TPPOLL_8125, R8125_TPPOLL_QUEUE0);

	r->netdev.tx_frames++;
	r->netdev.tx_bytes += length;
	r->tx_since_rx++;   // reset by the receive path; see the DEAF check in poll
	spinlock_release_irqrestore(&r->lock, flags);
	return 0;
}

static net_operations_t s_r8125_ops = {
	.transmit = r8125_transmit,
};

// ── THE INTERRUPT STATUS REGISTER, ACKNOWLEDGED (2026-08-22) ────────────────
//
// This driver masks interrupts off (IMR0 = 0) and polls, which is a fine
// choice. What it did NOT do — from its first slice until today — is ever
// READ the status register. R8125_ISR0_8125 was defined and referenced
// nowhere.
//
// That is a trap this chip family is famous for. ISR is WRITE-ONE-TO-CLEAR,
// and masking interrupts does not stop the device LATCHING status bits; it
// only stops the wire being pulled. Some of those conditions are sticky in a
// way that matters: RX FIFO OVERFLOW and RX DESCRIPTOR UNAVAILABLE are the
// device saying "I had nowhere to put a frame", and on this family reception
// can stay stopped until the bit is acknowledged. A poller that never
// acknowledges therefore works beautifully right up until the first overrun
// — and then is deaf forever, with the transmit side still cheerfully
// sending into a wire nothing comes back from.
//
// WHY IT SURFACED NOW: nothing had ever pushed sustained traffic at this
// card. Single-file fetches are short bursts with human-scale gaps. Then
// `os64get -a` arrived and streamed 86 files back to back, and the P5 froze
// three to six files in, four times running, taking ping with it while the
// rest of the machine stayed perfectly healthy — the exact fingerprint.
//
// So: read it, log anything alarming ONCE on the always-lit channel, and
// clear it. Acknowledging costs one MMIO read and (only when something
// happened) one write per scheduler pass.
//
// The bits below are the vendor's InterruptStatusBits, name for name
// (RxOK, RxErr, TxOK, TxErr, RxDescUnavail, LinkChg, RxFIFOOver,
// TxDescUnavail, SYSErr).
#define R8125_ISR_ROK      0x0001   // a frame arrived
#define R8125_ISR_RER      0x0002   // receive error
#define R8125_ISR_TOK      0x0004   // a frame went out
#define R8125_ISR_TER      0x0008   // transmit error
#define R8125_ISR_RDU      0x0010   // RX DESCRIPTOR UNAVAILABLE — we were too slow
#define R8125_ISR_LINKCHG  0x0020   // the wire came or went
#define R8125_ISR_RXFOVW   0x0040   // RX FIFO OVERFLOW — the chip dropped frames
#define R8125_ISR_TDU      0x0080   // TX descriptor unavailable
#define R8125_ISR_SERR     0x8000   // system error (a PCI fault — very bad)

// The bits that mean something went WRONG, as opposed to something happened.
#define R8125_ISR_TROUBLE (R8125_ISR_RER | R8125_ISR_TER | R8125_ISR_RDU | \
                           R8125_ISR_RXFOVW | R8125_ISR_SERR)

static void r8125_ack_status(r8125_t* r)
{
	uint32_t status = r8125_read32(r, R8125_ISR0_8125);
	if (status == 0)
		return;

	// Write-one-to-clear FIRST, before any of the work below: hand back
	// exactly the bits just read, so an event that lands from here on —
	// including a second link transition while the link is re-read and
	// logged further down — relatches and survives to the next pass instead
	// of being cleared unseen (Codex, PR #66: a clear that came after the
	// re-read swallowed the transition and left the seam holding the state
	// in between).
	r8125_write32(r, R8125_ISR0_8125, status);
	r->isr_acks++;
	r->isr_last = status;

	if ((status & (R8125_ISR_RDU | R8125_ISR_RXFOVW)) != 0)
	{
		r->isr_overrun_events++;

		// MILESTONES, NOT A FIRE HOSE. The detailed complaint below fires
		// once per boot; this reports the ORDER OF MAGNITUDE as it grows —
		// 10, 100, 1000 — which is the only thing anyone needs to know once
		// overruns are survivable. A handful across a whole refresh is the
		// wire briefly outrunning us and is fine. Thousands means the
		// receive ring is still too small for this card, and the number
		// says so without anybody having to go looking for it.
		if (r->isr_overrun_events == r->isr_overrun_next_report)
		{
			printd(DEBUG_EXCEPTIONS,
			       "r8125: %lu receive overruns so far (ring is %u descriptors) — "
			       "each one is dropped frames and a retransmit, not a stall. "
			       "If this keeps climbing, R8125_RX_DESCS wants to be bigger.\n",
			       r->isr_overrun_events, (unsigned)R8125_RX_DESCS);
			r->isr_overrun_next_report *= 10;
		}
	}

	if ((status & R8125_ISR_TROUBLE) != 0 && !r->isr_trouble_reported)
	{
		r->isr_trouble_reported = true;
		// SAY WHAT IT MEANS, WHICH IS NOT ALWAYS "THE NETWORK DIED".
		//
		// The first version of this line ended "if the network just died,
		// THIS is why" — written the same hour as the acknowledge, before
		// anyone had seen the fixed driver run. Then the P5 completed a
		// full 83-file refresh and logged exactly this with ISR=0x11
		// (ROK|RDU), and the sentence was simply false: the network had not
		// died, because acknowledging is what stopped it dying.
		//
		// The honest distinction is between the two halves of the story.
		// An overrun means FRAMES WERE LOST — the wire outran us and TCP
		// will resend them. That is a cost, not a casualty. What used to be
		// fatal was leaving the bit LATCHED, because reception stays
		// stopped until it is cleared, and until 2026-08-22 nothing ever
		// cleared it. A message that cries "the network is down" every time
		// a busy transfer drops a frame would train its reader to ignore
		// the one time it matters.
		printd(DEBUG_EXCEPTIONS,
		       "r8125: adapter reported trouble (ISR=0x%08x)%s%s%s%s%s — acknowledged. "
		       "%s\n",
		       status,
		       (status & R8125_ISR_RXFOVW) ? " RX-FIFO-OVERFLOW" : "",
		       (status & R8125_ISR_RDU)    ? " RX-DESCRIPTORS-EXHAUSTED" : "",
		       (status & R8125_ISR_RER)    ? " RX-ERROR" : "",
		       (status & R8125_ISR_TER)    ? " TX-ERROR" : "",
		       (status & R8125_ISR_SERR)   ? " SYSTEM-ERROR" : "",
		       (status & (R8125_ISR_RDU | R8125_ISR_RXFOVW))
		           ? "Frames were dropped and TCP will resend them; the link keeps working "
		             "BECAUSE this was cleared (leaving it latched is what used to kill the "
		             "network mid-transfer). Repeated occurrences mean the receive ring is "
		             "too small or drained too rarely for this wire — see R8125_RX_DESCS."
		           : "If the network is misbehaving, start here.");
	}

	if (status & R8125_ISR_LINKCHG)
	{
		// The wire came or went. Re-read, so the seam and /sys/net say what
		// the link IS rather than what it was at boot, and say so on the
		// always-lit channel: a cable falling out ends the network, and
		// nobody should need a debug bit on to learn that from the log.
		// The bit was handed back BEFORE this read (top of the function), so
		// a second transition landing during the read or the log line
		// relatches it and is seen next pass, instead of being cleared
		// unseen with the seam holding the state in between (Codex, PR #66).
		char link[R8125_LINK_WORDS_CAP];
		r8125_read_link(r);
		r8125_link_words(r, link, sizeof link);
		printd(DEBUG_EXCEPTIONS, "r8125: link changed: %s (PHYstatus 0x%08x)\n",
		       link, r->phystatus);
	}
}

// ── THE STALL WATCHDOG ──────────────────────────────────────────────────────
//
// Called from the drain, with the lock held. Says something ONCE when the
// network has plainly stopped working, and once more when it comes back — on
// DEBUG_EXCEPTIONS, so it lands in the log of every configuration without
// anybody having thought to switch a debug bit on first.
//
// The point is to catch the failure EARLY. The ring-full complaint in
// r8125_transmit is honest but late: by then 63 frames have already been
// handed to a device that is not listening. These two checks fire at the
// first opportunity that is unambiguous.
#define R8125_STALL_TICKS   (2 * TICKS_PER_SECOND)  // outstanding, and nothing moving
#define R8125_DEAF_FRAMES   64                      // sent this many, heard nothing back

static void r8125_check_stalled(r8125_t* r)
{
	bool wedged = false;
	const char* what = NULL;

	// TX WEDGED: descriptors are outstanding and the device has completed
	// none of them for two seconds. Two seconds is far longer than a 2.5GbE
	// link needs for anything, and short enough to name the culprit while
	// the person is still looking at the screen.
	if (r->tx_clean != r->tx_next &&
	    (kTicksSinceStart - r->last_progress_tick) > R8125_STALL_TICKS)
	{
		wedged = true;
		what = "device is not completing transmit descriptors";
	}
	// DEAF: we are talking steadily and nothing is coming back. Measured
	// against our OWN transmits rather than against the clock, because
	// silence on an idle machine is not a fault — silence while shouting is.
	else if (r->tx_since_rx >= R8125_DEAF_FRAMES)
	{
		wedged = true;
		what = "sent frames are going out but nothing is arriving";
	}

	if (wedged && !r->stall_reported)
	{
		r->stall_reported = true;
		printd(DEBUG_EXCEPTIONS,
		       "r8125: NETWORK STALLED — %s. "
		       "tx_next=%u tx_clean=%u rx_cursor=%u (rings of %u/%u), "
		       "tx_frames=%lu tx_errors=%lu rx_frames=%lu rx_errors=%lu, "
		       "%lu completions, %u sent since the last arrival, "
		       "%lu status acks, %lu receive overruns.\n",
		       what, r->tx_next, r->tx_clean, r->rx_cursor,
		       (unsigned)R8125_TX_DESCS, (unsigned)R8125_RX_DESCS,
		       r->netdev.tx_frames, r->netdev.tx_errors,
		       r->netdev.rx_frames, r->netdev.rx_errors,
		       r->tx_completions, r->tx_since_rx,
		       r->isr_acks, r->isr_overrun_events);
	}
	else if (!wedged && r->stall_reported && r->tx_clean == r->tx_next)
	{
		// Recovery deserves a line too: a log that records only the fall
		// leaves the reader unable to tell a machine that healed from one
		// that is still broken and merely quiet.
		r->stall_reported = false;
		printd(DEBUG_EXCEPTIONS, "r8125: network recovered — the transmit ring is draining again.\n");
	}
}

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

	// ACKNOWLEDGE FIRST. A latched RX-overflow or descriptors-exhausted bit
	// can be holding reception stopped; clearing it before the drain gives
	// the device permission to resume the moment the refills below give it
	// somewhere to put things. Anything that arrives during the drain
	// re-raises and is acknowledged next pass — the same re-evaluate-don't-
	// remember-edges discipline the e1000 doorbell gate uses in signals.c.
	r8125_ack_status(r);

	// Transmit completions first — cheap, and it frees slots for whatever
	// the receive path below is about to answer.
	uint16_t reaped = r8125_tx_reap(r->tx, R8125_TX_DESCS, &r->tx_clean, r->tx_next);
	if (reaped != 0)
	{
		r->tx_completions += reaped;
		r->last_progress_tick = kTicksSinceStart;
	}

	// Then every frame the device has finished with. Bounded per pass: a
	// wire delivering faster than we drain must not let one scheduler pass
	// run the whole ring repeatedly and starve everything else on this core.
	// Whatever is left stays owned by us and is collected next pass.
	//
	// THE BOUND IS THE RING SIZE, AND THAT COUPLING IS DELIBERATE (restated
	// 2026-08-22, when the receive ring went 64 -> 256). The ring exists to
	// absorb a burst that arrives faster than passes happen; draining less
	// than a full ring per pass would leave the tail of every burst sitting
	// there, which is how the card ran out of descriptors in the first
	// place. One pass may therefore now handle 256 frames instead of 64 —
	// roughly a millisecond of copying and stack work at the sizes this
	// traffic uses, which is the right trade against dropping frames.
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

		// Something arrived: the wire is alive in the direction that matters
		// most for noticing it is not.
		r->tx_since_rx = 0;
		r->last_progress_tick = kTicksSinceStart;
	}

	r8125_check_stalled(r);

	spinlock_release_irqrestore(&r->lock, flags);
}

// ── Bring-up ────────────────────────────────────────────────────────────────
static bool r8125_init_device(pci_device_t* dev)
{
	r8125_t* r = &s_r8125;
	r->pci = dev;

	// BEACON 1: found. Bus/device/function, so a P5 boot can be compared
	// against its own lspci output without guessing. Kept for the summary
	// line at the end of bring-up, which is where the glass hears about it.
	r->bus = dev->busNo; r->slot = dev->deviceNo; r->func = dev->funcNo;
	printd(DEBUG_NET, "r8125: found 10ec:8125 at %02x:%02x.%u\n",
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
	printd(DEBUG_NET, "r8125: BAR%u phys 0x%lx mapped at HHDM\n", R8125_BAR, bar_phys);

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

	printd(DEBUG_NET, "r8125: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
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

	// BEACON 6: the link as the power-on state negotiated it, before this
	// driver has touched the PHY. r8125_phy_configure prints it beside the
	// registers that produced it.
	r8125_read_link(r);

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
	printd(DEBUG_NET, "r8125: soft reset complete\n");

	// ── The PHY ─────────────────────────────────────────────────────────
	// After the MAC reset (which leaves the PHY alone, and leaves the OCP
	// window usable) and before the rings: the advertisement and the
	// renegotiation it may trigger are independent of everything below, and
	// the interrupt-status clear in the configuration step then swallows
	// the link-change the restart latched, so the first poll does not
	// announce a change it already knows about.
	r8125_phy_configure(r);

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
	printd(DEBUG_NET, "r8125: rings built (%u rx / %u tx descriptors)\n",
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
	// Only the BAD outcome earns the glass. A gate that opened (or was never
	// shut, which is the P5's answer every time) is bring-up narration; a gate
	// that stayed shut means receive is dead and the operator needs to know
	// now, not after wondering why nothing arrives.
	if (misc_after & R8125_RXDV_GATED)
		printf("r8125: rx gate STILL SET after clearing it (MISC 0x%08x -> 0x%08x) — receive will stay dead\n",
		       misc_before, misc_after);
	else
		printd(DEBUG_NET, "r8125: rx gate: MISC 0x%08x -> 0x%08x (RXDV_GATED clear)\n",
		       misc_before, misc_after);

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
	printd(DEBUG_NET, "r8125: tx/rx enabled (ChipCmd 0x%02x)\n", cmd);

	r->netdev.ops = &s_r8125_ops;
	r->netdev.mtu = 1500;
	r->netdev.driver_data = r;
	r->netdev.name[0] = 'r'; r->netdev.name[1] = '8'; r->netdev.name[2] = '1';
	r->netdev.name[3] = '2'; r->netdev.name[4] = '5'; r->netdev.name[5] = '0';
	r->netdev.name[6] = '\0';

	// The hardware's own answer, for /sys/net/<card>. Both were already in
	// hand for the summary line; the seam gets them too so a reader can check
	// this entry against /sys/bus/pci without going to the source.
	r->netdev.model = r->model;
	snprintf(r->netdev.location, sizeof(r->netdev.location), "%02x:%02x.%u",
	         r->bus, r->slot, r->func);

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

	// THE ONE LINE THE GLASS GETS (2026-08-20). Everything above this point
	// now narrates to the log; what survives here is what a human watching a
	// healthy machine come up actually wants: which chip, where it lives, who
	// it is, and whether the wire is good. A down link still says so — that
	// is a fact about this boot, not chatter — and every failure above kept
	// its printf, so a bring-up that stops still explains itself on screen.
	// Speed/duplex ride along because they have already earned their place:
	// the P5 negotiated 100/full rather than gigabit on its first light, and
	// that one word is what said "look at the cable, not the driver" — and
	// then, on 2026-09-05, "look at the PHY's advertisement, not the cable".
	// The raw PHYstatus rides beside it for the day the decode is not the
	// whole story. Read once more here, because the PHY may have finished a
	// negotiation the bounded wait gave up on.
	char link[R8125_LINK_WORDS_CAP];
	r8125_read_link(r);
	r8125_link_words(r, link, sizeof link);
	printf("r8125: %s at %02x:%02x.%u, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s (PHYstatus 0x%08x)\n",
	       r->model ? r->model : "RTL8125",
	       r->bus, r->slot, r->func,
	       r->netdev.mac[0], r->netdev.mac[1], r->netdev.mac[2],
	       r->netdev.mac[3], r->netdev.mac[4], r->netdev.mac[5],
	       link, r->phystatus);
	printd(DEBUG_NET, "r8125: registered as %s\n", r->netdev.name);
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
