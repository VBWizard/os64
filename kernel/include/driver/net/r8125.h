#ifndef R8125_H
#define R8125_H

// r8125.h — RealTek RTL8125 2.5GbE, the wire to the P5.
//
// RTL8125.md is the design record and the argument; this header is the
// contract. Read the doc first — especially its AMENDMENT section, which
// records which of its claims were verified against the tree and which
// were corrected before a line of this was written.
//
// WHY THIS DRIVER EXISTS: to retire the sneakernet. The P5 gets its
// userland by a human walking a USB stick across a room; with this driver
// plus os64get it fetches a fresh build over a gigabit switch instead.
// Everything above the seam (ARP, IPv4, TCP) already works and needs no
// change — a NIC that moves frames is the entire missing piece.
//
// ── THE HONESTY SECTION, and it is not decoration ──────────────────────
//
// QEMU DOES NOT EMULATE AN RTL8125. There is no boot on any machine in
// this house where this code meets its silicon except the P5 itself. That
// single fact shapes everything here:
//
//   - every step of bring-up announces itself on serial, because a bare
//     metal failure has no debugger behind it — only the last line printed;
//   - the driver stands down SILENTLY and COMPLETELY when the chip is
//     absent, so every existing QEMU boot stays exactly as green as it was;
//   - the register offsets below carry their PROVENANCE in comments. This
//     family moved several registers between the 8168 and the 8125
//     generations, and a plausible-looking wrong offset is the single most
//     expensive mistake available here: the chip accepts the write, reads
//     back what you wrote, and never moves a frame.
//
// ── REGISTER PROVENANCE, stated once ───────────────────────────────────
//
// Offsets marked [8169-family] are shared across the whole RealTek
// descriptor-ring lineage. Offsets marked [8125-SPECIFIC] MOVED in this
// generation. The driver was first written from knowledge of Linux's
// r8169.c (RTL8125 = RTL_GIGA_MAC_VER_60 and up) with the moved ones
// tagged UNCONFIRMED; on 2026-09-05 every offset and bit was checked
// against Realtek's own GPL r8125 driver (r8125.h / r8125_n.c, the
// awesometic/realtek-r8125-dkms mirror) and the tags came off, each
// definition naming the vendor's spelling. The PHY register map arrived
// the same day, already checked (r8125_phy.h).
//
// The doc's tripwire put it best: trust the shape, verify the numbers.

#include <stdint.h>
#include <stdbool.h>
#include "driver/net/net_device.h"

// PCI identity. 10ec:8125 covers the RTL8125A and RTL8125B; Chris confirmed
// this exact pair from a live lspci on the P5 (2026-08-16). The 5GbE RTL8126
// answers to 10ec:8126 and is deliberately NOT matched — it is a different
// chip and would deserve its own verification pass, not a silent welcome.
#define R8125_VENDOR_REALTEK 0x10EC
#define R8125_DEVICE_8125    0x8125

// THE MMIO WINDOW IS BAR2, NOT BAR0. On this family BAR0 is I/O space and
// BAR2 is the 64-bit memory window (Linux's r8169 selects region 2 for the
// PCIe parts). Worth stating loudly because e1000.c — the driver this one
// is otherwise modelled on — hardcodes BAR0, and a copy-paste lands you on
// an I/O BAR that this driver correctly refuses, after an hour.
#define R8125_BAR 2

// The register file comfortably fits one page; map a page and be done.
#define R8125_REG_WINDOW 0x1000

// Ring sizes. These used to both be 64, with a comment reasoning that "bigger
// rings would buy nothing measurable today" because TCP's window and the
// scheduler pass set the ceiling first. That reasoning was about THROUGHPUT,
// and it was right about throughput — but it missed what a small ring costs
// when it RUNS OUT, which is not slowness. It is RDU: RX-descriptors-exhausted,
// a bit this family LATCHES, and which halted reception outright until
// 2026-08-22 taught the driver to acknowledge it (r8125_ack_status).
//
// THE RECEIVE RING IS 256 SINCE 2026-08-22, and the evidence is specific. The
// P5 logged `ISR=0x00000011` — ROK | RDU — during an `os64get -a` pulling 83
// files back to back: frames arrived faster than one scheduler pass could
// drain 64 descriptors, the card had nowhere to put the next one, and every
// such moment is DROPPED FRAMES and a TCP retransmit. Acknowledging made that
// survivable; it did not make it free. A ring four times deeper absorbs a
// burst that a single pass then drains, which is exactly the shape of this
// traffic — bursty file transfer, not steady line-rate.
//
// The cost is 256 * R8125_BUF_SIZE = 512KB of receive buffer plus 4KB of
// descriptors, and it is charged once at bring-up on a machine with at least
// 8GB (Chris, deciding this: "We're *not* trying to write the world's smallest
// OS"). 0.006% of the smallest supported machine, against a bug that ended
// every large transfer.
//
// TRANSMIT STAYS AT 64, deliberately, because the evidence never accused it:
// no TDU, and the ring-full path has not fired once since the acknowledge
// landed. Growing it too would be changing an untested variable in the same
// breath as a tested one — and transmit here is mostly bare ACKs, which the
// device drains as fast as we can post them.
#define R8125_RX_DESCS 256
#define R8125_TX_DESCS 64

// One RX buffer per descriptor. 2KB covers a standard 1500-byte MTU with
// room to spare; jumbo frames are a non-goal (RTL8125.md).
#define R8125_BUF_SIZE 2048

// Descriptor ring alignment required by the hardware: 256 bytes.
// [8169-family] — unchanged for this generation. We allocate page-aligned,
// which satisfies it several times over.
#define R8125_RING_ALIGN 256

// WHAT THE PHY ADVERTISES, above the 10/100/1000 it always offers
// (r8125_phy_plan_advertisement owns the rest of the policy).
//
// 2.5GBASE-T is ON — which is to say the driver leaves the PHY's own
// default alone. The first draft of this slice turned it off on the
// theory that the P5's gigabit switch mishandled the 802.3bz next pages
// and fell back to 100M; the P5's "as found" dump refuted that the same
// day (2026-09-05: 2.5G advertised, partner offering 10/100/1000, link
// 1000/full, before the driver had touched anything). With it ON, a boot
// whose firmware left the PHY at its default advertises exactly what we
// would write, so nothing is written and the link is never dropped at
// init. With it OFF every such boot renegotiated for no gain.
//
// The receive path could not drain a real 2.5G link (the ring already
// overruns on gigabit bursts — R8125_RX_DESCS), so the day a 2.5G switch
// arrives, that row is the next problem. A good problem to have.
#define R8125_ADVERTISE_2500 1

void init_r8125(void);
// The seam's drain verb, called by knet whenever its doorbell rings
// (DOORBELL.md): by this driver's interrupt handler on arrival once MSI is
// wired, and by the tick every pass until then. Cheap
// when the hardware is absent (one pointer test) and cheap when it is idle
// (one descriptor-status read per ring); returns whether anything moved.
bool r8125_drain(struct net_device* dev);

// Adopt MSI (DOORBELL.md): called from kernel_init at the platform moment the
// e1000 adopts its INTx wire — after the LAPIC is the interrupt controller —
// never from init_r8125, which runs long before that. Programs the PCI MSI
// capability at the BSP and vector 0x46; a chip without the capability, or a
// NETPOLL boot, stays tick-driven and says so. The handler, r8125_isr, is
// called only from handler_r8125_msi_asm.
void r8125_enable_msi(void);
void r8125_isr(void);

#endif // R8125_H
