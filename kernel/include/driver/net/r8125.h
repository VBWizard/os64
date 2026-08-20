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
// descriptor-ring lineage and are the ones this author knows cold.
// Offsets marked [8125-SPECIFIC] MOVED in this generation and are the
// dangerous ones — they are written from knowledge of Linux's r8169.c
// (RTL8125 = RTL_GIGA_MAC_VER_60 and up) and MUST be confirmed against the
// vendor's GPL r8125 driver or that file before first light on the P5.
// Any offset this driver has not yet confirmed says so at its definition.
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

// v1 ring sizes. Powers of two, and modest on purpose: the transfer this
// exists for is a few megabytes pulled over a LAN, not line-rate routing.
// The receive ceiling is set by TCP's 8KB window and the scheduler pass
// long before it is set by these (see RTL8125.md's throughput note), so
// bigger rings would buy nothing measurable today.
#define R8125_RX_DESCS 64
#define R8125_TX_DESCS 64

// One RX buffer per descriptor. 2KB covers a standard 1500-byte MTU with
// room to spare; jumbo frames are a non-goal (RTL8125.md).
#define R8125_BUF_SIZE 2048

// Descriptor ring alignment required by the hardware: 256 bytes.
// [8169-family] — unchanged for this generation. We allocate page-aligned,
// which satisfies it several times over.
#define R8125_RING_ALIGN 256

void init_r8125(void);
// The processSignals rider, beside virtio_net_poll and e1000_poll. Cheap
// when the hardware is absent (one pointer test) and cheap when it is idle
// (one descriptor-status read per ring).
void r8125_poll(void);

#endif // R8125_H
