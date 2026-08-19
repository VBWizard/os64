// e1000.c — the Intel 8254x Gigabit NIC: registers, descriptor rings, frames.
//
// SLICE HISTORY (NETWORK.md Phase 1):
//   virtio-net came first and proved the STACK (ARP through TCP, a real
//   page fetched from the real internet, a real ping answered).
//   This file is the second driver, and its job is to prove the SEAM:
//   net_device.h's charter says a seam designed against one driver is just
//   that driver's private wrapper, and the only way to collect on that is
//   hardware with a completely different personality underneath the same
//   five-line interface. Not one byte of ethernet.c/arp.c/ipv4.c/icmp.c/
//   udp.c/tcp.c changes for this NIC. That silence is the deliverable.
//
// ── A ONE-SCREEN PRIMER ON REAL NIC HARDWARE ────────────────────────────────
// virtio was a conversation: capabilities to walk, features to negotiate, a
// spec written by people who also write drivers. The 8254x is an ARTIFACT.
// It was designed in ~2000 to be cheap in silicon, and every awkwardness
// below is a transistor budget showing through:
//
//   * A wall of 32-bit registers at fixed offsets in a 128KB memory window.
//     No discovery, no negotiation, no versioning — you are expected to
//     have the datasheet on your desk. (You do: Intel's "PCI/PCI-X Family
//     of Gigabit Ethernet Controllers Software Developer's Manual" is the
//     document a whole generation of hobby OS writers learned DMA from.)
//
//   * The MAC address lives in a tiny SERIAL EEPROM — a few hundred bytes
//     of non-volatile memory reached through a one-register keyhole, 16
//     bits per transaction, poll for DONE. Why so baroque? Because in 1999
//     a parallel ROM interface would have cost pins, and pins cost money.
//     The hardware also auto-copies the MAC into the receive-address
//     registers at reset, which is our fallback path below.
//
//   * DESCRIPTOR RINGS for data, which is the part worth learning once,
//     deeply — see the next block. Same instinct as NVMe's submission and
//     completion queues, twenty years earlier and one bus down.
//
// ── HOW THE RINGS WORK ──────────────────────────────────────────────────────
// Each direction is a circular array of 16-byte DESCRIPTORS in ordinary RAM,
// each one saying "there is a buffer at this physical address" plus a little
// status. The device and the driver chase each other around the circle with
// two register cursors:
//
//   HEAD (RDH/TDH) — the DEVICE's cursor. Hardware owns it; we only read it.
//   TAIL (RDT/TDT) — the DRIVER's cursor. We own it; hardware only reads it.
//
// The hardware may work on descriptors from HEAD up to (but not including)
// TAIL. So the tail register is literally the sentence "everything up to
// here is yours," and moving it is how each side is granted work:
//
//   RX is PRE-PAYMENT. We hand the device empty buffers by advancing RDT,
//   and it fills them as packets arrive, setting a DONE bit (DD) in each
//   descriptor's status byte. We walk forward finding DD set, deliver those
//   frames, and re-donate each descriptor by writing its index back to RDT.
//   Run out of donated buffers and the device simply drops packets — which
//   is FINE and by design: networks are lossy, TCP exists precisely because
//   links drop things, and the stack above copes by construction.
//
//   TX is a work queue. We fill a descriptor, then advance TDT to say "go."
//   With the RS (Report Status) bit set, the device writes DD back when the
//   frame is on the wire, which is how we know the buffer is reusable.
//
// The one true trap of this design, and it is a classic: a descriptor's
// status byte is written by the DEVICE via DMA into memory the CPU also
// caches. On x86 that is coherent (the bus snoops), so a `volatile` read is
// enough here — but the volatile is not decoration. Drop it and the compiler
// will happily hoist the status load out of the polling loop and spin
// forever on a value from three milliseconds ago.
//
// ── WHY POLLED (the decision, recorded) ─────────────────────────────────────
// NETWORK.md's Phase 1 sketch said "interrupts, not polling, from the start,
// MSI-X wants doing properly here." That plan met the datasheet and lost:
// the 82540EM is a PCI part from 2002 and has no MSI-X at all (MSI-X is a
// PCIe-generation feature — it arrives with the 82574/e1000e and 82575/igb),
// and neither QEMU's nor VirtualBox's 8254x model implements even plain MSI.
// Legacy INTx is the entire menu on this chip. So the honest choice was
// INTx-plus-IOAPIC-routing versus polling, and polling wins for v1: it keeps
// the slice about the seam instead of about interrupt plumbing, and it is
// the pattern this kernel has already proven twice (xhci_poll, then
// virtio_net_poll). Interrupts get built for hardware that rewards them —
// the RTL8125 on the P5, or an e1000e — where real MSI-X vectors and a
// bottom half buy microsecond round trips. All interrupt sources are
// explicitly MASKED at init below, so a shared INTx line can never scream
// at a handler that does not exist.

#include <stdint.h>
#include <stdbool.h>
#include "paging.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — framebuffer boot lines only
#include "strings/strcpy.h"
#include "memcpy.h"
#include "memset.h"
#include "spinlock.h"
#include "memory/allocator.h"
#include "time.h"            // wait(ms) — the init-time delays the datasheet demands
#include "CONFIG.h"
#include "driver/system/pci.h"
#include "driver/system/apic.h"  // ioapic_route_gsi / ioapic_mask_gsi — the INTx door
#include "smp.h"                 // kCPUInfo — the BSP's APIC ID targets the doorbell
#include "driver/net/net_device.h"
#include "driver/net/e1000.h"

extern pci_device_t* kPCIDeviceHeaders;
extern pci_device_t* kPCIDeviceFunctions;
extern uint8_t kPCIDeviceCount, kPCIFunctionCount;
extern uintptr_t kHHDMOffset;
extern uintptr_t kKernelPML4v;

// ── PCI identity ────────────────────────────────────────────────────────────
// Intel's vendor ID, then the 8254x parts that share ONE programming model —
// the legacy descriptor layout and register map this file speaks. The device
// ID also names the part in the boot line, because "e1000" is a driver name,
// not a chip: it comes from Intel's own naming for the Gigabit family (the
// 100Mbit 8255x family got "e100"), and one driver covering five silicon
// revisions is exactly why that naming existed.
//
// DELIBERATELY ABSENT: 0x10D3, the 82574L that QEMU calls `e1000e`. It is a
// PCIe part with a different init dance (and the MSI-X the older chips lack).
// Claiming it here would half-drive it; it gets its own slice the day it has
// a reason to exist, which is the same day interrupts do.
#define E1000_VENDOR_INTEL  0x8086

typedef struct { uint16_t device; const char* name; } e1000_model_t;
static const e1000_model_t kE1000Models[] = {
	{ 0x100E, "82540EM" },   // QEMU's `-device e1000` AND VirtualBox's default adapter
	{ 0x100F, "82545EM" },   // VirtualBox "PRO/1000 MT Server"
	{ 0x1004, "82543GC" },   // VirtualBox "PRO/1000 T Server"
	{ 0x1010, "82546EB" },   // dual-port, common on real server boards
	{ 0x1026, "82545GM" },
	{ 0x1027, "82545GM(f)" },
};
#define E1000_MODEL_COUNT (sizeof(kE1000Models) / sizeof(kE1000Models[0]))

// ── Register offsets (8254x manual chapter 13) ──────────────────────────────
// Every one of these is a 32-bit register in the BAR0 window. Spelled as
// offsets rather than a struct for the same reason virtio's common config is:
// a packed struct over MMIO invites the compiler to merge or split accesses,
// and device registers do not tolerate either.
#define E1000_CTRL    0x0000   // Device Control — reset, link, duplex
#define E1000_STATUS  0x0008   // Device Status — link up, speed, duplex (read-only)
#define E1000_EECD    0x0010   // EEPROM/Flash Control & Data
#define E1000_EERD    0x0014   // EEPROM Read — the keyhole
#define E1000_ICR     0x00C0   // Interrupt Cause Read (reading it CLEARS it)
#define E1000_ICS     0x00C8   // Interrupt Cause Set — fire a cause ON COMMAND.
                               // The routing probe's whole trick: ask the card
                               // to ring its own doorbell and see which IOAPIC
                               // input hears it. Chipset-agnostic by design.
#define E1000_IMS     0x00D0   // Interrupt Mask Set
#define E1000_IMC     0x00D8   // Interrupt Mask Clear

// The three causes we listen for (ICR/ICS/IMS share one bit layout).
// TXDW is deliberately NOT here: transmit already reclaims its own
// descriptors opportunistically, so a TX-done doorbell buys nothing yet.
#define E1000_INT_LSC     0x04   // Link Status Change — cable events, and the probe's test bell
#define E1000_INT_RXO     0x40   // Receiver Overrun — the no-silent-drops witness
#define E1000_INT_RXT0    0x80   // Receiver Timer — a frame arrived (the whole point)
#define E1000_RCTL    0x0100   // Receive Control
#define E1000_TCTL    0x0400   // Transmit Control
#define E1000_TIPG    0x0410   // Transmit Inter-Packet Gap
#define E1000_RDBAL   0x2800   // RX descriptor base, low 32 bits
#define E1000_RDBAH   0x2804   // RX descriptor base, high 32 bits
#define E1000_RDLEN   0x2808   // RX ring length IN BYTES (128-byte multiple)
#define E1000_RDH     0x2810   // RX head — the device's cursor
#define E1000_RDT     0x2818   // RX tail — ours
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_MTA     0x5200   // Multicast Table Array — 128 dwords of filter
#define E1000_RAL0    0x5400   // Receive Address 0, low  (our MAC, bytes 0-3)
#define E1000_RAH0    0x5404   // Receive Address 0, high (bytes 4-5 + valid bit)

// CTRL bits
#define CTRL_FD       (1u << 0)    // full duplex
#define CTRL_ASDE     (1u << 5)    // auto-speed detection enable
#define CTRL_SLU      (1u << 6)    // SET LINK UP — without this, copper stays dark
#define CTRL_RST      (1u << 26)   // device reset (self-clearing)
#define CTRL_VME      (1u << 30)   // VLAN mode enable — we want this OFF
#define CTRL_PHY_RST  (1u << 31)

// STATUS bits
#define STATUS_LU     (1u << 1)    // link up

// EERD bits — the EEPROM keyhole's protocol (82540/82541/82545 layout).
// NOTE the generational trap: on 82571 and later, DONE moved to bit 1 and
// the address field widened. This driver claims only the older parts (see
// the model table), so this layout is correct for every device it accepts.
#define EERD_START        (1u << 0)
#define EERD_DONE         (1u << 4)
#define EERD_ADDR_SHIFT   8
#define EERD_DATA_SHIFT   16

// RCTL bits
#define RCTL_EN           (1u << 1)    // receiver enable
#define RCTL_SBP          (1u << 2)    // store bad packets (diagnostics only)
#define RCTL_UPE          (1u << 3)    // unicast promiscuous
#define RCTL_MPE          (1u << 4)    // multicast promiscuous
#define RCTL_LPE          (1u << 5)    // long packet enable (jumbo) — off
#define RCTL_BAM          (1u << 15)   // BROADCAST ACCEPT MODE — see the essay below
#define RCTL_BSIZE_2048   (0u << 16)   // buffer size 00b = 2048 bytes
#define RCTL_SECRC        (1u << 26)   // strip the ethernet CRC before DMA

// TCTL bits
#define TCTL_EN           (1u << 1)
#define TCTL_PSP          (1u << 3)    // pad short packets to the 60-byte minimum
#define TCTL_CT_SHIFT     4            // collision threshold
#define TCTL_COLD_SHIFT   12           // collision distance

// Descriptor status/command bits
#define RX_STATUS_DD      (1u << 0)    // descriptor done — hardware filled this
#define RX_STATUS_EOP     (1u << 1)    // end of packet
#define TX_CMD_EOP        (1u << 0)    // this descriptor ends the packet
#define TX_CMD_IFCS       (1u << 1)    // insert FCS (let hardware compute the CRC)
#define TX_CMD_RS         (1u << 3)    // report status — set DD when transmitted
#define TX_STATUS_DD      (1u << 0)

// ── Descriptor layouts (manual 3.2.3 receive, 3.3.3 legacy transmit) ────────
// These structs ARE the hardware's memory format — 16 bytes each, field
// order and width fixed by silicon. packed because a compiler that inserts
// padding here does not produce a driver, it produces confetti.
typedef struct
{
	uint64_t addr;       // buffer physical address (we fill)
	uint16_t length;     // bytes received (device fills)
	uint16_t checksum;
	uint8_t  status;     // DD, EOP, ... (device fills)
	uint8_t  errors;
	uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct
{
	uint64_t addr;       // buffer physical address
	uint16_t length;     // bytes to transmit
	uint8_t  cso;        // checksum offset — unused, we offload nothing
	uint8_t  cmd;        // EOP | IFCS | RS
	uint8_t  status;     // DD (device fills)
	uint8_t  css;
	uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

// Ring sizes. RDLEN/TDLEN must be a multiple of 128 bytes, and a descriptor
// is 16 bytes, so the count must be a multiple of 8. Thirty-two of each is
// 512 bytes per ring and 64KB of frame buffers per direction — generous for
// an OS whose entire network load is a test suite and a ping, and trivially
// growable the day a workload says so.
#define E1000_RX_DESCS  32
#define E1000_TX_DESCS  32
#define E1000_BUF_SIZE  2048   // must match RCTL_BSIZE_2048 above

// Same fence discipline as virtio_net.c: x86 is TSO, so coherent DMA sees
// our stores in program order and the only barrier needed is against the
// COMPILER reordering ring writes past the tail-register publish. The macro
// exists to mark every place that would need real teeth on a weaker ISA.
#define wmb() __asm__ volatile("" ::: "memory")

// ── Driver state ────────────────────────────────────────────────────────────
// ONE static instance: v1 drives the first supported 8254x found. The xHCI
// driver walked this same road (one controller until the P5's five made it
// plural) and the net_device seam is already plural-ready, so growing an
// array here is a small, well-understood change when a second NIC shows up.
typedef struct
{
	pci_device_t* pci;
	volatile uint8_t* regs;        // BAR0 window, HHDM-mapped, uncached

	volatile e1000_rx_desc_t* rx;  // RX ring (HHDM view)
	uint64_t rx_phys;              //   ...and what the device was told
	uint8_t* rx_buf;               // RX frame slab, slot i at i*E1000_BUF_SIZE
	uint64_t rx_buf_phys;
	uint16_t rx_cursor;            // next descriptor we expect DD on

	volatile e1000_tx_desc_t* tx;
	uint64_t tx_phys;
	uint8_t* tx_buf;
	uint64_t tx_buf_phys;
	uint16_t tx_next;              // next descriptor we will fill (== TDT)
	uint16_t tx_clean;             // oldest descriptor not yet reclaimed

	bool up;                       // rings live and receiver/transmitter enabled
	spinlock_t lock;               // guards both rings' driver-side state
	net_device_t netdev;

	// INTx bookkeeping (all written by the ISR — single writer, aligned
	// 32-bit stores, no lock needed; readers are diagnostics).
	uint8_t  intx_gsi;                     // the IOAPIC input the probe confirmed
	volatile uint32_t intx_fires;          // ICR != 0 — the doorbell was for us
	volatile uint32_t intx_strangers;      // ICR == 0 — a shared line fired for someone else
	volatile uint32_t intx_link_changes;   // LSC causes seen (cable events)
} e1000_t;

static e1000_t s_e1000;

// The doorbell's two public flags, read by processSignals (signals.c):
// UsesIntx false = the probe never confirmed a wire, poll unconditionally
// (yesterday's behavior, never a dead NIC); true = drain only when RxWork
// says the ISR heard something. RxWork is set in interrupt context and
// consumed (cleared-then-drained, in that order, so a doorbell during the
// drain schedules the next pass instead of being lost) in processSignals.
volatile bool kE1000UsesIntx = false;
volatile bool kE1000RxWork = false;

// Set (once) when the ISR unilaterally abandons INTx — a shared line gone
// hostile at runtime. processSignals announces it so the fallback is never
// silent; the poll path is already unconditional again by the time anyone
// reads this.
volatile bool kE1000IntxDivorced = false;

// Probe-window state, ISR-visible. The VBox lesson (2026-08-06 evening,
// same day as the first wire): a candidate GSI can be a STORM — the line
// reads asserted the instant it's unmasked (polarity model mismatch, or a
// neighbor whose device model ignores PCI Interrupt Disable), vector 0x45
// fires back-to-back forever, and the boot freezes mid-probe with the CPU
// trapped in a polite argument with a wire. Defense in depth: the ISR
// itself watches the stranger count during a probe window and MASKS the
// candidate from interrupt context (one MMIO write, ISR-safe) once it's
// clearly a storm, which un-wedges the CPU and lets the probe loop move on.
static volatile int16_t  sProbeGsi = -1;        // candidate under probe, else -1
static volatile uint32_t sProbeStrangerBase = 0;
static volatile bool     sProbeStormed = false;

// ── MMIO accessors ──────────────────────────────────────────────────────────
// volatile at the single choke point so no call site can forget it. The
// 8254x register file is little-endian like the CPU, so no swapping — noted
// because the protocol stack above this driver WILL meet big-endian fields
// (network byte order), and the difference deserves to be conscious rather
// than coincidental.
static inline uint32_t e1000_read32(e1000_t* e, uint32_t reg)
{
	return *(volatile uint32_t*)(e->regs + reg);
}

static inline void e1000_write32(e1000_t* e, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t*)(e->regs + reg) = value;
}

// ── BAR mapping ─────────────────────────────────────────────────────────────
// Resolve BAR0 to a physical base, honoring the 64-bit encoding (bits 2:1 ==
// 10b means the high half lives in the next BAR) and refusing I/O-space BARs
// (bit 0). The 8254x's BAR0 is always a memory BAR; an I/O BAR here would
// mean we misread the header, and saying so beats poking ports.
static uint64_t e1000_bar_phys(pci_device_t* dev, uint8_t bar)
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

// The register window is 128KB by specification (manual 13.1 — the whole
// register file, including the multicast table at 0x5200, fits inside it).
// Mapped at the kernel's HHDM alias for the same reason the xHCI and virtio
// BARs are: an UPPER-half VA is visible under EVERY task's CR3, and the poll
// below runs from processSignals under whoever's page tables happen to be
// live. PAGE_PCD because device registers must never be cached — a cached
// read of a ring cursor returns stale truth, which in a producer/consumer
// protocol is indistinguishable from a lie.
#define E1000_REG_WINDOW  0x20000

static volatile uint8_t* e1000_map_regs(uint64_t bar_phys)
{
	uint64_t first_page = bar_phys & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t page_count = (E1000_REG_WINDOW + (bar_phys - first_page) + PAGE_SIZE - 1) / PAGE_SIZE;

	paging_map_pages((pt_entry_t*)kKernelPML4v, kHHDMOffset + first_page,
	                 first_page, page_count,
	                 PAGE_PRESENT | PAGE_WRITE | PAGE_PCD);
	return (volatile uint8_t*)(kHHDMOffset + bar_phys);
}

// ── The EEPROM keyhole ──────────────────────────────────────────────────────
// Read one 16-bit word: write the address with the START bit, then spin until
// the hardware sets DONE and parks the data in the top half of the same
// register. One transaction per word, a few microseconds each — which is why
// nobody reads an EEPROM on a hot path, and why the MAC gets read exactly
// once, at init, forever.
static bool e1000_eeprom_read(e1000_t* e, uint8_t word, uint16_t* out)
{
	e1000_write32(e, E1000_EERD, ((uint32_t)word << EERD_ADDR_SHIFT) | EERD_START);

	// Bounded spin. A device that never answers is a device we stop asking:
	// the caller falls back to the receive-address registers, which the
	// hardware itself loaded from this same EEPROM at reset.
	for (int spin = 0; spin < 100000; spin++)
	{
		uint32_t v = e1000_read32(e, E1000_EERD);
		if (v & EERD_DONE)
		{
			*out = (uint16_t)(v >> EERD_DATA_SHIFT);
			return true;
		}
	}
	return false;
}

// The MAC address, read from silicon two ways, never invented.
//
// PATH 1 — the EEPROM: words 0/1/2 hold the six bytes, little-endian pairs.
// This is the authoritative copy, the one burned in at the factory (or, on a
// hypervisor, the one the config file made up on our behalf).
//
// PATH 2 — RAL0/RAH0: at reset the hardware AUTOLOADS the MAC from that same
// EEPROM into the receive-address filter registers, so reading them back
// gets the same six bytes without the keyhole. This is not merely a
// fallback for stubbornness; some real parts (certain ICH-integrated MACs)
// genuinely deny software EEPROM access, and their drivers live on this
// path. Two sources, one truth, and the OS makes up neither.
static bool e1000_read_mac(e1000_t* e, uint8_t* mac)
{
	uint16_t w0, w1, w2;
	if (e1000_eeprom_read(e, 0, &w0) &&
	    e1000_eeprom_read(e, 1, &w1) &&
	    e1000_eeprom_read(e, 2, &w2))
	{
		mac[0] = (uint8_t)(w0 & 0xFF);  mac[1] = (uint8_t)(w0 >> 8);
		mac[2] = (uint8_t)(w1 & 0xFF);  mac[3] = (uint8_t)(w1 >> 8);
		mac[4] = (uint8_t)(w2 & 0xFF);  mac[5] = (uint8_t)(w2 >> 8);
		printd(DEBUG_NET, "e1000: MAC from EEPROM\n");
	}
	else
	{
		uint32_t ral = e1000_read32(e, E1000_RAL0);
		uint32_t rah = e1000_read32(e, E1000_RAH0);
		mac[0] = (uint8_t)(ral & 0xFF);        mac[1] = (uint8_t)((ral >> 8)  & 0xFF);
		mac[2] = (uint8_t)((ral >> 16) & 0xFF); mac[3] = (uint8_t)((ral >> 24) & 0xFF);
		mac[4] = (uint8_t)(rah & 0xFF);        mac[5] = (uint8_t)((rah >> 8)  & 0xFF);
		printd(DEBUG_NET, "e1000: no EEPROM access — MAC from RAL0/RAH0 (hardware autoload)\n");
	}

	// An all-zero MAC is not an address, it is a failure wearing one. Refuse
	// rather than register a NIC that can never receive its own unicast.
	for (int i = 0; i < NET_MAC_LEN; i++)
		if (mac[i] != 0)
			return true;
	return false;
}

// ── Transmit ────────────────────────────────────────────────────────────────
static int32_t e1000_transmit(net_device_t* dev, const void* frame, uint16_t length)
{
	e1000_t* e = (e1000_t*)dev->driver_data;

	if (!e->up || length == 0 || length > NET_FRAME_MAX)
	{
		dev->tx_errors++;
		return -1;
	}

	uint64_t irqflags = spinlock_acquire_irqsave(&e->lock);

	// Opportunistic reclaim before declaring the ring full: a burst of small
	// frames can outrun the poll, and the completions are usually already
	// sitting there with DD set. Cheap (a status read per finished slot) and
	// it keeps a fast sender from getting a spurious refusal.
	while (e->tx_clean != e->tx_next && (e->tx[e->tx_clean].status & TX_STATUS_DD))
	{
		e->tx[e->tx_clean].status = 0;
		e->tx_clean = (uint16_t)((e->tx_clean + 1) % E1000_TX_DESCS);
	}

	uint16_t next = (uint16_t)((e->tx_next + 1) % E1000_TX_DESCS);
	if (next == e->tx_clean)
	{
		// Ring full. The bound IS the flow control (the pipe doctrine, wire
		// edition): refuse loudly and let the caller decide. An unbounded TX
		// queue is a memory leak with a modem attached.
		spinlock_release_irqrestore(&e->lock, irqflags);
		dev->tx_errors++;
		return -2;
	}

	uint16_t slot = e->tx_next;
	memcpy(e->tx_buf + (uint32_t)slot * E1000_BUF_SIZE, frame, length);

	e->tx[slot].addr   = e->tx_buf_phys + (uint64_t)slot * E1000_BUF_SIZE;
	e->tx[slot].length = length;
	e->tx[slot].cso    = 0;
	e->tx[slot].css    = 0;
	e->tx[slot].special = 0;
	e->tx[slot].status = 0;
	// EOP: this descriptor completes a packet (we never split a frame).
	// IFCS: hardware appends the ethernet CRC — the four bytes at the end of
	//       every frame since 1980, and the reason SECRC exists on the RX
	//       side to take them back off.
	// RS:   tell us when it is gone, which is the only way a buffer becomes
	//       reusable without guessing.
	e->tx[slot].cmd    = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;

	// Publish the descriptor BEFORE handing it over. The tail write is the
	// "go" signal; the device may DMA the instant it lands.
	wmb();
	e->tx_next = next;
	e1000_write32(e, E1000_TDT, next);

	dev->tx_frames++;
	dev->tx_bytes += length;
	spinlock_release_irqrestore(&e->lock, irqflags);
	return 0;
}

static net_operations_t s_e1000_ops = {
	.transmit = e1000_transmit,
};

// ── Poll (processSignals, the xhci_poll/virtio_net_poll precedent) ──────────
void e1000_poll(void)
{
	e1000_t* e = &s_e1000;
	if (!e->up)
		return;

	// Cheap reentry guard: if another core is mid-drain, skip. Nothing is
	// lost — the descriptors keep their DD bits and the next pass collects
	// them.
	static volatile uint32_t busy = 0;
	if (__sync_lock_test_and_set(&busy, 1))
		return;

	// TX reclaim: every descriptor the hardware finished returns to the
	// pool. Status is cleared as we go so a stale DD from three trips around
	// the ring ago can never be mistaken for a fresh completion.
	uint64_t irqflags = spinlock_acquire_irqsave(&e->lock);
	while (e->tx_clean != e->tx_next && (e->tx[e->tx_clean].status & TX_STATUS_DD))
	{
		e->tx[e->tx_clean].status = 0;
		e->tx_clean = (uint16_t)((e->tx_clean + 1) % E1000_TX_DESCS);
	}
	spinlock_release_irqrestore(&e->lock, irqflags);

	// RX drain: one frame per lock hold, DELIVERED WITH THE LOCK RELEASED.
	// That shape is load-bearing, not stylistic, and virtio_net.c earned the
	// lesson first: the protocol stack's RX handler TRANSMITS in direct
	// response to arrivals — an ARP request in begets an ARP reply out, an
	// echo request begets an echo reply — and e1000_transmit takes this same
	// lock. Holding it across delivery would deadlock the machine on its
	// first answered ping. So each frame is staged onto the stack, its
	// descriptor re-donated, the lock dropped, and only THEN does the stack
	// see it. The busy flag above already guarantees a single drainer, so
	// cycling the lock mid-drain races nothing.
	uint8_t staged[NET_FRAME_MAX];
	for (;;)
	{
		irqflags = spinlock_acquire_irqsave(&e->lock);

		uint16_t slot = e->rx_cursor;
		if (!(e->rx[slot].status & RX_STATUS_DD))
		{
			spinlock_release_irqrestore(&e->lock, irqflags);
			break;   // hardware has not finished this one yet
		}

		uint16_t len    = e->rx[slot].length;
		uint8_t  status = e->rx[slot].status;
		uint8_t  errors = e->rx[slot].errors;
		uint16_t flen   = 0;

		// Three ways a descriptor arrives unusable, each counted rather than
		// guessed at. `errors` non-zero is the hardware telling us the frame
		// was damaged in flight (bad CRC, alignment, symbol error): the wire
		// lied, and we drop it exactly as the medium would have. EOP absent
		// means the frame was split across descriptors, which cannot happen
		// while our buffers (2048) exceed our MTU (1500) — so if it ever
		// does, something is deeply wrong and half a packet is worse than
		// none.
		if (errors != 0)
			e->netdev.rx_errors++;
		else if (!(status & RX_STATUS_EOP))
			e->netdev.rx_dropped_too_big++;
		else if (len > NET_FRAME_MAX)
			e->netdev.rx_dropped_too_big++;
		else if (len > 0)
		{
			flen = len;
			memcpy(staged, e->rx_buf + (uint32_t)slot * E1000_BUF_SIZE, flen);
		}

		// Re-donate: clear the status so the DD bit means something next
		// time around, advance our cursor, and hand the descriptor back by
		// writing its index to the tail. Writing `slot` (not the new cursor)
		// is the protocol: the tail names the LAST descriptor the device may
		// use, so this says "you may now go through this one."
		e->rx[slot].status = 0;
		e->rx_cursor = (uint16_t)((slot + 1) % E1000_RX_DESCS);
		wmb();
		e1000_write32(e, E1000_RDT, slot);

		spinlock_release_irqrestore(&e->lock, irqflags);

		if (flen)
			net_device_rx(&e->netdev, staged, flen);
	}

	__sync_lock_release(&busy);
}

// ── Ring construction ───────────────────────────────────────────────────────
// Both rings come from allocate_memory_aligned, which hands back PAGE-aligned
// physical memory that is HHDM-reachable while allocated (the lazy-HHDM
// rule) — so one allocation gives us both the physical address the device
// needs and a kernel pointer we can write through. Page alignment more than
// satisfies the manual's 16-byte descriptor-base requirement.
//
// HISTORY NOTE: this ring construction refused kmalloc_dma when it was
// written (2026-08-06) because kmalloc_dma then IDENTITY-mapped its pages —
// a high physical address became a high virtual address in kernel territory,
// a hazard this comment named and DEBTS booked. That refusal WON the
// argument: on 2026-08-19 kmalloc_dma itself adopted exactly this doctrine
// (HHDM pointer for the kernel, physical handed back separately for the
// device), so the two styles are now the same style. This code keeps its
// explicit spelling as the original of the species. The 8254x takes full
// 64-bit descriptor and buffer addresses (that is what RDBAH/TDBAH are for),
// so nothing here needs a low-memory guarantee; we simply take the
// allocator's word and hand over the physical address, which is the only
// address the device ever sees.
static bool e1000_setup_rx(e1000_t* e)
{
	e->rx_phys = allocate_memory_aligned(E1000_RX_DESCS * sizeof(e1000_rx_desc_t));
	if (e->rx_phys == 0)
		return false;
	e->rx = (volatile e1000_rx_desc_t*)(e->rx_phys | kHHDMOffset);

	e->rx_buf_phys = allocate_memory_aligned((uint64_t)E1000_RX_DESCS * E1000_BUF_SIZE);
	if (e->rx_buf_phys == 0)
		return false;
	e->rx_buf = (uint8_t*)(e->rx_buf_phys | kHHDMOffset);

	// Every descriptor points at its standing buffer. The allocator zeroes
	// every allocation at the choke point (house doctrine), so status starts
	// clear — which is exactly the "hardware has not touched this" state.
	for (uint16_t i = 0; i < E1000_RX_DESCS; i++)
	{
		e->rx[i].addr   = e->rx_buf_phys + (uint64_t)i * E1000_BUF_SIZE;
		e->rx[i].status = 0;
	}
	e->rx_cursor = 0;

	e1000_write32(e, E1000_RDBAL, (uint32_t)(e->rx_phys & 0xFFFFFFFF));
	e1000_write32(e, E1000_RDBAH, (uint32_t)(e->rx_phys >> 32));
	e1000_write32(e, E1000_RDLEN, E1000_RX_DESCS * (uint32_t)sizeof(e1000_rx_desc_t));
	e1000_write32(e, E1000_RDH, 0);
	// Tail = last index, so the device owns descriptors 0..N-2 and one slot
	// stays ours. That spare slot is what makes "full" and "empty"
	// distinguishable in any head/tail ring — the same arithmetic the pipe
	// buffer and the virtqueues live by.
	e1000_write32(e, E1000_RDT, E1000_RX_DESCS - 1);

	// Receiver policy, stated deliberately:
	//   EN     — on.
	//   BAM    — accept BROADCAST. Not optional, not a nicety: ARP asks the
	//            whole segment "who has this IP?", and a NIC that filters
	//            broadcast never hears the question, never answers, and
	//            never gets an answer either. Every "my stack transmits but
	//            nothing ever comes back" bug in NIC history has this bit at
	//            the bottom of it.
	//   SECRC  — strip the 4-byte ethernet FCS. The hardware already checked
	//            it (that is what the errors field reports); delivering it
	//            upward would just be four bytes of trailing garbage the
	//            protocol layer would have to know to ignore.
	//   BSIZE  — 2048-byte buffers, matching the slab above.
	// NOT set: UPE/MPE (promiscuous — we are not a sniffer; unicast filtering
	// is what RAL0/RAH0 is for), LPE (jumbo frames, a non-goal), SBP (store
	// bad packets, a diagnostic we would have to then discard by hand).
	e1000_write32(e, E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
	return true;
}

static bool e1000_setup_tx(e1000_t* e)
{
	e->tx_phys = allocate_memory_aligned(E1000_TX_DESCS * sizeof(e1000_tx_desc_t));
	if (e->tx_phys == 0)
		return false;
	e->tx = (volatile e1000_tx_desc_t*)(e->tx_phys | kHHDMOffset);

	e->tx_buf_phys = allocate_memory_aligned((uint64_t)E1000_TX_DESCS * E1000_BUF_SIZE);
	if (e->tx_buf_phys == 0)
		return false;
	e->tx_buf = (uint8_t*)(e->tx_buf_phys | kHHDMOffset);

	for (uint16_t i = 0; i < E1000_TX_DESCS; i++)
	{
		e->tx[i].addr   = e->tx_buf_phys + (uint64_t)i * E1000_BUF_SIZE;
		e->tx[i].status = 0;
		e->tx[i].cmd    = 0;
	}
	e->tx_next  = 0;
	e->tx_clean = 0;

	e1000_write32(e, E1000_TDBAL, (uint32_t)(e->tx_phys & 0xFFFFFFFF));
	e1000_write32(e, E1000_TDBAH, (uint32_t)(e->tx_phys >> 32));
	e1000_write32(e, E1000_TDLEN, E1000_TX_DESCS * (uint32_t)sizeof(e1000_tx_desc_t));
	e1000_write32(e, E1000_TDH, 0);
	e1000_write32(e, E1000_TDT, 0);   // head == tail: the ring starts empty

	// Transmitter policy: enable, pad runt frames to ethernet's 60-byte
	// minimum (PSP — the minimum exists because 1980s collision detection
	// needed a frame to still be going out when the far end's collision
	// signal came back), and the datasheet's recommended collision threshold
	// and distance for full duplex. On a full-duplex link there are no
	// collisions at all, which makes these two fields pure ceremony — but
	// ceremony the hardware checks, and QEMU-only correctness is exactly the
	// kind of thing that ambushes a driver on real silicon.
	e1000_write32(e, E1000_TCTL, TCTL_EN | TCTL_PSP |
	                             (0x10u << TCTL_CT_SHIFT) |
	                             (0x40u << TCTL_COLD_SHIFT));

	// Inter-packet gap: the enforced silence between frames, in bit times.
	// The magic number is the manual's recommended IEEE 802.3 value
	// (IPGT=10, IPGR1=8, IPGR2=6). Hypervisors ignore it entirely; real
	// copper does not, and a wrong gap is a link that mysteriously
	// underperforms rather than one that visibly fails.
	e1000_write32(e, E1000_TIPG, 0x0060200A);
	return true;
}

// ── Bring-up ────────────────────────────────────────────────────────────────
static bool e1000_init_device(pci_device_t* dev, const char* model)
{
	e1000_t* e = &s_e1000;
	e->pci = dev;

	// Memory space + bus mastering on (command register at config offset 4).
	// Bus mastering is what legalizes the rings' DMA — without it the device
	// is a well-configured paperweight, and the failure mode is the cruelest
	// kind: every register reads back exactly as written, and not one packet
	// ever moves.
	writePCIRegister(dev->busNo, dev->deviceNo, dev->funcNo, 4, dev->command | 0x6);

	uint64_t bar_phys = e1000_bar_phys(dev, 0);
	if (bar_phys == 0)
	{
		printd(DEBUG_NET, "e1000: BAR0 is not a memory BAR (0x%08x) — refusing\n", dev->baseAdd[0]);
		return false;
	}
	e->regs = e1000_map_regs(bar_phys);
	printd(DEBUG_NET, "e1000: %s at %02x:%02x.%u, BAR0 phys 0x%lx\n",
	       model, dev->busNo, dev->deviceNo, dev->funcNo, bar_phys);

	// Mask every interrupt source before anything else can raise one. We
	// poll; an unmasked legacy INTx line shared with another device would
	// mean a level-triggered interrupt nobody ever acknowledges, which wedges
	// the line for its rightful owner too. Reading ICR clears whatever was
	// already pending.
	e1000_write32(e, E1000_IMC, 0xFFFFFFFF);
	(void)e1000_read32(e, E1000_ICR);

	// Reset. RST is self-clearing; the manual asks for a short settle before
	// touching anything (the internal state machines reload defaults, and
	// the MAC address is re-autoloaded from the EEPROM into RAL0/RAH0).
	e1000_write32(e, E1000_CTRL, e1000_read32(e, E1000_CTRL) | CTRL_RST);
	wait(10);
	for (int spin = 0; (e1000_read32(e, E1000_CTRL) & CTRL_RST) && spin < 100; spin++)
		wait(1);
	if (e1000_read32(e, E1000_CTRL) & CTRL_RST)
	{
		printd(DEBUG_NET, "e1000: device refuses reset\n");
		return false;
	}

	// And mask again: reset restored the default interrupt mask.
	e1000_write32(e, E1000_IMC, 0xFFFFFFFF);
	(void)e1000_read32(e, E1000_ICR);

	// Link up, auto-negotiate speed and duplex, VLAN tagging off. SLU is the
	// one that matters on copper: leave it clear and the PHY never brings the
	// link up, so the driver works perfectly and the cable stays dark.
	uint32_t ctrl = e1000_read32(e, E1000_CTRL);
	ctrl |= CTRL_SLU | CTRL_ASDE;
	ctrl &= ~CTRL_VME;
	e1000_write32(e, E1000_CTRL, ctrl);

	if (!e1000_read_mac(e, e->netdev.mac))
	{
		printd(DEBUG_NET, "e1000: no usable MAC address — refusing\n");
		return false;
	}

	// Program our own MAC into receive-address slot 0 with the ADDRESS VALID
	// bit, so unicast filtering accepts frames aimed at us. The hardware
	// autoload usually did this already; doing it explicitly costs two
	// register writes and removes an "usually" from the boot path.
	uint32_t ral = ((uint32_t)e->netdev.mac[0])       | ((uint32_t)e->netdev.mac[1] << 8) |
	               ((uint32_t)e->netdev.mac[2] << 16) | ((uint32_t)e->netdev.mac[3] << 24);
	uint32_t rah = ((uint32_t)e->netdev.mac[4])       | ((uint32_t)e->netdev.mac[5] << 8) |
	               (1u << 31);   // AV — address valid
	e1000_write32(e, E1000_RAL0, ral);
	e1000_write32(e, E1000_RAH0, rah);

	// Clear the multicast hash filter. Reset should have zeroed it; the
	// manual says clear it anyway, and a stale hash entry means accepting
	// somebody else's multicast traffic forever.
	for (uint32_t i = 0; i < 128; i++)
		e1000_write32(e, E1000_MTA + i * 4, 0);

	if (!e1000_setup_rx(e) || !e1000_setup_tx(e))
	{
		printd(DEBUG_NET, "e1000: descriptor ring setup failed\n");
		return false;
	}

	e->up = true;

	// Link status is best-effort and READ, never assumed: on copper the PHY
	// needs a moment to auto-negotiate, so a false here at boot may simply
	// mean "not yet". We register either way and let the counters tell the
	// story — a NIC that is up but unplugged is a legitimate machine state,
	// not a driver failure.
	uint32_t status = e1000_read32(e, E1000_STATUS);
	e->netdev.link_up = (status & STATUS_LU) != 0;

	strcpy(e->netdev.name, "e1000_0");
	e->netdev.mtu = 1500;
	e->netdev.ops = &s_e1000_ops;
	e->netdev.driver_data = e;

	printd(DEBUG_NET, "e1000: rings up (rx %u, tx %u), link %s, STATUS 0x%08x\n",
	       E1000_RX_DESCS, E1000_TX_DESCS, e->netdev.link_up ? "UP" : "down", status);

	return net_device_register(&e->netdev) == 0;
}

// ── Entry point (called from kernel_init, gated by NONET) ───────────────────
void init_e1000(void)
{
	// Scan the device table AND the function table — the xHCI precedent.
	// QEMU puts the e1000 on a plain slot, but real topologies multi-function
	// everything (the 82546 is a dual-port part: one chip, two NICs, two
	// functions), and the P5 taught this kernel not to assume.
	pci_device_t* found = NULL;
	const char* model = NULL;

	for (uint32_t m = 0; m < E1000_MODEL_COUNT && found == NULL; m++)
	{
		for (int i = 0; i < kPCIDeviceCount && found == NULL; i++)
			if (kPCIDeviceHeaders[i].vendor == E1000_VENDOR_INTEL &&
			    kPCIDeviceHeaders[i].device == kE1000Models[m].device)
			{
				found = &kPCIDeviceHeaders[i];
				model = kE1000Models[m].name;
			}
		for (int i = 0; i < kPCIFunctionCount && found == NULL; i++)
			if (kPCIDeviceFunctions[i].vendor == E1000_VENDOR_INTEL &&
			    kPCIDeviceFunctions[i].device == kE1000Models[m].device)
			{
				found = &kPCIDeviceFunctions[i];
				model = kE1000Models[m].name;
			}
	}

	if (found == NULL)
	{
		// No such NIC is a configuration, not a failure — most boots today
		// have none. One quiet debug line so a "why no network?"
		// investigation starts with an answer instead of a search.
		printd(DEBUG_NET, "e1000: no device on PCI\n");
		return;
	}

	if (e1000_init_device(found, model))
		printf("e1000 (%s): %02x:%02x:%02x:%02x:%02x:%02x (rx/tx rings %u/%u, link %s)\n",
		       model,
		       s_e1000.netdev.mac[0], s_e1000.netdev.mac[1], s_e1000.netdev.mac[2],
		       s_e1000.netdev.mac[3], s_e1000.netdev.mac[4], s_e1000.netdev.mac[5],
		       E1000_RX_DESCS, E1000_TX_DESCS,
		       s_e1000.netdev.link_up ? "up" : "down");
	else
		printf("e1000: device found but init failed (DEBUG_NET for details)\n");
}

// ── INTx: the doorbell (first wired 2026-08-06) ─────────────────────────────
// Interrupts entered computing in 1956 (UNIVAC 1103A) because polling was
// recognized as the machine wasting its life checking an empty mailbox.
// Seventy years later this driver relives the exact realization: everything
// below exists so a packet ANNOUNCES itself instead of waiting up to a
// scheduler pass to be noticed.

// The top half, in BSD's vocabulary: entered from handler_e1000_intx_asm
// (vector 0x45). Microseconds, no locks — read ICR (the read itself acks
// every pending cause AND deasserts the level-triggered line), tally what
// happened, raise the work flag. The ring drain deliberately stays out of
// interrupt context: the RX path transmits in direct response to arrivals
// (ARP reply, echo reply) and takes the ring lock — top halves that take
// protocol locks are how a NIC interrupt becomes a 2 a.m. deadlock hunt.
void e1000_isr(void)
{
	e1000_t* e = &s_e1000;
	if (!e->up)
		return;

	uint32_t icr = e1000_read32(e, E1000_ICR);
	if (icr == 0)
	{
		// Shared-line etiquette: a level-triggered INTx line can be
		// wire-OR'd by several cards, and ICR == 0 means this ring wasn't
		// ours. Counted, never assumed away — a climbing stranger count is
		// how we'd learn a neighbor moved onto our GSI.
		e->intx_strangers++;

		// Storm breaker #1 (probe window): if the candidate under probe is
		// firing stranger interrupts back-to-back, it isn't our wire — it's
		// a line that reads asserted the moment it's unmasked. Mask it RIGHT
		// HERE, from interrupt context, because the probe loop can't run
		// while we're re-entering forever. 64 strangers in one 2ms-ish probe
		// window is decisive; a legitimate shared line ticks over in ones.
		if (sProbeGsi >= 0 && (e->intx_strangers - sProbeStrangerBase) > 64)
		{
			ioapic_mask_gsi((uint8_t)sProbeGsi);
			sProbeStormed = true;
			sProbeGsi = -1;
		}

		// Storm breaker #2 (runtime divorce): a wire that probed clean can
		// still turn hostile later (hotplugged neighbor, model quirk). If
		// strangers utterly swamp genuine fires, stop trusting the doorbell:
		// mask our GSI, flip back to unconditional polling, and let
		// processSignals announce it. Never a dead NIC, never a wedged core.
		else if (kE1000UsesIntx &&
		         e->intx_strangers > (e->intx_fires * 16u) + 4096u)
		{
			ioapic_mask_gsi(e->intx_gsi);
			e1000_write32(e, E1000_IMC, 0xFFFFFFFF);
			kE1000UsesIntx = false;
			kE1000IntxDivorced = true;
			kE1000RxWork = true;   // hand the baton back to the poll cleanly
		}
		return;
	}

	e->intx_fires++;
	if (icr & E1000_INT_LSC)
	{
		// Cable event: re-read reality rather than inferring it. One MMIO
		// read; the counters and the seam's link_up tell the story.
		e->intx_link_changes++;
		e->netdev.link_up = (e1000_read32(e, E1000_STATUS) & STATUS_LU) != 0;
	}

	// Any confirmed cause raises the flag — RXT0 obviously, RXO because the
	// drain is exactly what relieves an overrun, LSC harmlessly (one spare
	// drain per cable event). processSignals consumes it.
	kE1000RxWork = true;
}

// Adopt the wire. Called from kernel_init AFTER the platform switches to
// APIC mode (init_e1000 runs long before that — rings at driver init,
// doorbell at platform init, the keyboard's exact precedent).
//
// The routing problem: PCI INTx reaches the IOAPIC through chipset wiring
// that ACPI describes only in AML (_PRT), and os64 has no AML interpreter.
// The config-space interrupt_line byte is firmware's note about LEGACY-PIC
// routing — a hint, not an answer. So instead of trusting anyone's map, we
// PROBE: for each candidate IOAPIC input, route it to vector 0x45, ask the
// card to ring its own doorbell (ICS — a register that exists on this 2002
// chip as if it knew), and listen. The wire that answers is the truth, on
// q35, PIIX3, or a motherboard neither of us has met.
//
// Probe safety: before touching any candidate we set PCI Interrupt Disable
// (command bit 10, PCI 2.3) on every OTHER device, so a stranger with a
// pending INTx can't storm a freshly-unmasked level line mid-probe. Nothing
// else in os64 uses INTx today — every other driver polls — so the masks
// simply make the de facto official at the source. The day a second driver
// wants a doorbell, it clears its own bit the way we clear ours below.
void e1000_enable_intx(void)
{
	e1000_t* e = &s_e1000;
	if (!e->up)
		return;

	if (e->pci->interrupt_pin == 0)
	{
		// The device itself says it has no INTx pin. Config-space law, not
		// a probe failure — stay polled, say so once.
		printd(DEBUG_NET, "e1000: no INTx pin per config space — staying polled\n");
		return;
	}

	// Silence every other device's INTx at the source (see header comment).
	// READ-MODIFY-WRITE THE LIVE REGISTER, never the enumeration-time cache:
	// drivers (NVMe, AHCI) set Bus Master Enable AFTER enumeration, so
	// writing back the cached command word would strip their DMA mid-flight
	// — the first boot of this code did exactly that, and the NVMe timed out
	// its completion 7.5 seconds into an otherwise-perfect probe. Keeping
	// only the low half also leaves the status word's RW1C bits untouched.
	for (int i = 0; i < kPCIDeviceCount; i++)
	{
		pci_device_t* d = &kPCIDeviceHeaders[i];
		if (d == e->pci)
			continue;
		uint32_t live = readPCIRegister(d->busNo, d->deviceNo, d->funcNo, 4) & 0xFFFF;
		writePCIRegister(d->busNo, d->deviceNo, d->funcNo, 4, live | 0x400);
	}
	for (int i = 0; i < kPCIFunctionCount; i++)
	{
		pci_device_t* d = &kPCIDeviceFunctions[i];
		if (d == e->pci)
			continue;
		uint32_t live = readPCIRegister(d->busNo, d->deviceNo, d->funcNo, 4) & 0xFFFF;
		writePCIRegister(d->busNo, d->deviceNo, d->funcNo, 4, live | 0x400);
	}
	// And make sure OURS is enabled (bus mastering and memory space were set
	// at init; Interrupt Disable clear is the third leg of the tripod).
	uint32_t ourLive = readPCIRegister(e->pci->busNo, e->pci->deviceNo, e->pci->funcNo, 4) & 0xFFFF;
	writePCIRegister(e->pci->busNo, e->pci->deviceNo, e->pci->funcNo, 4,
	                 (ourLive | 0x6) & ~0x400u);

	// Candidate IOAPIC inputs, most-likely first:
	//   1. interrupt_line, if it names a plausible GSI (16-23 is where every
	//      chipset we know parks PCI links; SeaBIOS on q35 writes the GSI
	//      here, so on QEMU this usually hits on the first try),
	//   2. the classic swizzle 16 + ((slot + pin - 1) & 3) — the barber-pole
	//      pattern boards use so four neighbors don't pile on one line,
	//   3. the full 16-23 sweep, because the probe makes guessing free,
	//   4. interrupt_line even below 16 — PIIX3-era boards that really do
	//      wire PCI onto ISA pins get one honest last chance.
	uint8_t pin = e->pci->interrupt_pin;                    // 1=INTA .. 4=INTD
	uint8_t candidates[13];
	uint8_t candidateCount = 0;
	if (e->pci->interrupt_line >= 16 && e->pci->interrupt_line <= 23)
		candidates[candidateCount++] = e->pci->interrupt_line;
	candidates[candidateCount++] = (uint8_t)(16 + ((e->pci->deviceNo + pin - 1) & 3));
	for (uint8_t gsi = 16; gsi <= 23; gsi++)
		candidates[candidateCount++] = gsi;
	if (e->pci->interrupt_line > 2 && e->pci->interrupt_line < 16)
		candidates[candidateCount++] = e->pci->interrupt_line;

	uint8_t bspApicId = kCPUInfo[0].apicID;
	bool confirmed = false;

	for (uint8_t c = 0; c < candidateCount && !confirmed; c++)
	{
		uint8_t gsi = candidates[c];

		// Skip a candidate we already tried (the list overlaps by design).
		bool seen = false;
		for (uint8_t p = 0; p < c; p++)
			if (candidates[p] == gsi)
				seen = true;
		if (seen)
			continue;

		// Breadcrumb ON THE GLASS, before the unmask: if a candidate wedges
		// anyway (a storm shape the breakers don't catch), the frozen screen
		// names the culprit GSI instead of ending at the scheduler banner —
		// which is exactly how the VBox hang presented before this line
		// existed. One short line per candidate is cheap; a mystery isn't.
		printf("e1000: probing GSI %u for the INTx wire...\n", gsi);

		if (!ioapic_route_gsi(gsi, 0x45, bspApicId, true /*level*/, true /*active low*/))
			return;   // no IOAPIC at all — polled it is

		// Clean slate, then ring the test bell: unmask ONLY the link-status
		// cause and fire it via ICS. If this GSI is our wire, the ISR runs
		// within the settle window and the fire counter moves.
		(void)e1000_read32(e, E1000_ICR);
		uint32_t before = e->intx_fires;
		sProbeStormed = false;
		sProbeStrangerBase = e->intx_strangers;
		sProbeGsi = (int16_t)gsi;               // arms storm breaker #1
		e1000_write32(e, E1000_IMS, E1000_INT_LSC);
		e1000_write32(e, E1000_ICS, E1000_INT_LSC);

		// Settle window: a bounded PAUSE spin, deliberately CLOCKLESS. The
		// old wait(2) here rode kTicksSinceStart — but a storming candidate
		// starves IRQ0 (vector 0x45 outranks 0x20), the tick clock freezes,
		// and wait() never returns: the whole boot hangs inside the probe.
		// A pure iteration bound cannot be starved, only slowed — tens of
		// milliseconds at worst, on any clock, under any storm.
		for (volatile uint32_t spin = 0;
		     spin < 20000000u && e->intx_fires == before && !sProbeStormed;
		     spin++)
			__asm__ volatile("pause");
		sProbeGsi = -1;

		if (e->intx_fires > before)
		{
			confirmed = true;
			e->intx_gsi = gsi;
		}
		else
		{
			// Not our wire — silent, or a storm the ISR already masked. Mask
			// the device side, drain any latent cause, and (re-)mask the
			// IOAPIC side so the candidate isn't left aimed at us — on a
			// level line, an unowned route is a future wedge.
			e1000_write32(e, E1000_IMC, 0xFFFFFFFF);
			(void)e1000_read32(e, E1000_ICR);
			ioapic_mask_gsi(gsi);
			if (sProbeStormed)
				printf("e1000: GSI %u is a STORM (%u strangers) — masked and skipped\n",
				       gsi, e->intx_strangers - sProbeStrangerBase);
			else
				printd(DEBUG_NET, "e1000: GSI %u stayed silent — not our wire\n", gsi);
		}
	}

	if (!confirmed)
	{
		// Every candidate stayed silent. Yesterday's behavior, never a dead
		// NIC: the poll keeps running unconditionally and packets still move.
		e1000_write32(e, E1000_IMC, 0xFFFFFFFF);
		(void)e1000_read32(e, E1000_ICR);
		printf("e1000: INTx probe found no wire — staying polled\n");
		return;
	}

	// The wire is real. Open for business: packet arrivals, overruns, and
	// cable events ring the bell; everything else stays masked.
	(void)e1000_read32(e, E1000_ICR);
	e1000_write32(e, E1000_IMS, E1000_INT_RXT0 | E1000_INT_RXO | E1000_INT_LSC);
	kE1000UsesIntx = true;

	printd(DEBUG_NET, "e1000: INTx confirmed on GSI %u (probe fired %u, strangers %u)\n",
	       e->intx_gsi, e->intx_fires, e->intx_strangers);
	printf("e1000: interrupts live — INTx GSI %u -> vector 0x45 (probe-confirmed)\n", e->intx_gsi);
}
