#ifndef E1000_H
#define E1000_H

// e1000.h — the Intel 8254x Gigabit NIC (NETWORK.md Phase 1, SECOND driver).
//
// Why this chip, and why second: virtio-net proved the stack; the e1000
// proves the SEAM. net_device.h says it out loud — "a seam proven against
// one driver is just that driver's private wrapper" — and the only way to
// collect on that promise is a second driver whose hardware personality is
// nothing like the first's. virtio is what an interface looks like when
// software people design it (feature negotiation, shared-memory rings, a
// spec you can read in an afternoon). The 8254x is what an interface looks
// like when it has to exist in silicon: a wall of memory-mapped registers,
// a serial EEPROM read through a keyhole one 16-bit word at a time, and
// magic bit patterns for inter-packet gap timing. If ethernet.c, arp.c,
// ipv4.c, icmp.c, udp.c and tcp.c cannot tell which of the two is under
// them, the seam is real.
//
// It is also the canonical teaching NIC of hobby OS development — Intel's
// "PCI/PCI-X Family of Gigabit Ethernet Controllers Software Developer's
// Manual" is the datasheet a generation learned DMA rings from — and it is
// what BOTH our hypervisors hand out by default: QEMU's `-device e1000` is
// an 82540EM, and VirtualBox's default adapter is the same part. One
// driver, two hypervisors, zero excuses. (Chris ratified it on exactly
// that ground.)
//
// Scan PCI for a supported 8254x, bring it up (reset, MAC from the EEPROM,
// RX/TX descriptor rings, link), and register it with the net_device seam.
// Quietly does nothing when no such device is present — a boot with no NIC
// is a configuration, not an error.
void init_e1000(void);

// Drain the rings: hand back every RX descriptor the hardware filled (via
// net_device_rx) and reclaim every TX descriptor it finished. Called from
// processSignals every scheduler pass, beside virtio_net_poll and
// xhci_poll — cheap when idle (a guard branch and one descriptor-status
// read per direction).
//
// WHY POLLED, when NETWORK.md said "interrupts, not polling, from the
// start": because MSI-X does not exist on this chip. The 82540EM is a
// PCI (not PCIe) part from 2002; MSI-X arrived with the PCIe generation
// (82574/e1000e, 82575/igb), and neither hypervisor's 8254x model
// implements even plain MSI — legacy INTx is the whole menu. So the real
// choice here was INTx-plus-IOAPIC-routing versus polling, and polling
// wins for v1 on two grounds: it keeps this slice about the SEAM rather
// than about interrupt plumbing, and it matches the pattern already
// proven twice in this kernel (xhci_poll, virtio_net_poll). The interrupt
// slice belongs with the hardware that actually rewards it — the RTL8125
// on the P5, or an e1000e — where MSI-X vectors and a bottom half buy
// microsecond round trips instead of scheduler-pass ones. When that day
// comes, mind the house gotcha: AP-routed vectors must be >= 0x40 (the
// AP TPR masks low vectors — already bitten once, already documented).
//
// THAT DAY CAME (2026-08-06, event-driven Phase 1): the INTx-plus-IOAPIC
// half of the menu is now real — see e1000_enable_intx below. The
// paragraph above stays because its reasoning was right: v1 WAS about the
// seam, and the vector gotcha it warned about is honored (0x45). What
// changed is only the poll's TRIGGER: with a confirmed wire, processSignals
// drains on the ISR's say-so instead of on faith. No wire confirmed = the
// unconditional poll, exactly as written above, forever the fallback.
void e1000_poll(void);

// Adopt the INTx wire (called from kernel_init AFTER the IMCR switches the
// platform to APIC mode — init_e1000 runs long before that, so the rings
// and the doorbell are separate phases, the keyboard's exact precedent).
// Discovers the IOAPIC input EMPIRICALLY: routes a candidate GSI, asks the
// card to ring its own doorbell (ICS register), and listens — chipset-
// agnostic, no AML interpreter required. On success, packet arrivals /
// overruns / link changes interrupt vector 0x45 on the BSP and
// processSignals drains only when kE1000RxWork says there's work. On
// silence, stays polled and says so — never a dead NIC.
void e1000_enable_intx(void);

// The doorbell flags processSignals reads (set/cleared as e1000.c documents;
// the ISR itself, e1000_isr, is called only from handler_e1000_intx_asm).
extern volatile bool kE1000UsesIntx;
extern volatile bool kE1000RxWork;
extern volatile bool kE1000IntxDivorced;   // runtime fallback happened; announce once

#endif // E1000_H
