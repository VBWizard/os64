#ifndef XHCI_H
#define XHCI_H

// xHCI (USB 3.x host controller), HID input and AX210 Bluetooth transport.
//
// WHY THIS EXISTS: the Bosgame P5 has no PS/2 port. Every keystroke it will
// ever receive arrives over USB, so "os64 runs on real hardware" requires
// this driver. (2026-07-19: "being able to run on actual hardware ...
// that's kind of the whole point." — the owner, correctly.)
//
// SHAPE OF V1 (each limit is a decision, not an accident):
//   - TICK-DRIVEN, not interrupts: no MSI/IOAPIC wiring. The event rings
//     live in ordinary RAM and a daemon, kusb (kusb.h), drains them when
//     the tick rings its bell, so an event waits a tick at most
//     (docs/design/pending/XHCI_INTERRUPT.md is the arc that adds the
//     interrupt).
//   - ROOT PORTS ONLY, no hubs: input devices must be plugged straight into
//     the machine. (Devices with built-in hubs enumerate AS hubs — those
//     need the hub slice, which is future work.)
//   - ENUMERATION AT BOOT ONLY: no hotplug. Plug it in, then power on.
//   - Root ports on up to eight controllers are probed, binding the first
//     boot-protocol keyboard and mouse and probing the Intel AX210's USB
//     descriptors and firmware image type. A matching cold AX210 receives the
//     embedded Intel firmware and DDC configuration. The first operational
//     AX210 keeps its USB slot and DMA storage for manual and background scans
//     and two LE HID peers through /sys/bluetooth. Failed bring-up attempts
//     Disable Slot before freeing DMA storage. LE input requires encryption
//     after passkey pairing or an explicit Just Works request. LE keyboards
//     use Boot Protocol or a supported keyboard Report Map.
//     LE mice use a relative X/Y/wheel Report Map. USB mice can fall back
//     to boot protocol when their report descriptor is unsupported.
//   - Handles BOTH context sizes (HCCPARAMS1.CSZ): QEMU uses 32-byte
//     contexts, real hardware frequently uses 64 — the P5 gets to choose.
//   - Scratchpad buffers allocated when the controller demands them
//     (QEMU demands none; real silicon usually does).
//
// Delivery: keyboard HID reports flow through keyboard_deliver_event(); mouse
// reports flow through input_inject_mouse(). The console and GUI therefore do
// not need to know whether input arrived over PS/2, USB or Bluetooth.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Probe PCI for xHCI controllers (class 0x0C / subclass 0x03 / prog-if 0x30),
// enumerate their root-port devices, bind HID input and query the AX210.
// Safe to call when no controller exists.
// Call BEFORE task creation: the MMIO mapping lands in the kernel PML4's
// upper half so every later task inherits it.
void init_xHCI(void);

// How many controllers came up. kusb exists only when this is not zero.
uint32_t xhci_controller_count(void);
// One pass by kusb, and nobody else: drain each controller's event ring
// until it is empty or the TSC passes `deadline` (completed keyboard and
// mouse reports are translated and delivered, transfer TRBs re-armed), then
// advance Bluetooth discovery, the LE peers, the connection policy and the
// keyboards' typematic repeat. `wake_began` is the TSC at the top of the
// wake, for the measurements. Returns false, having done nothing, when the
// serializing flag was held; the holder's release rings kUsbDoorbell.
// `*work_left` says a controller was stopped by the deadline or not
// reached, and the caller should come straight back.
bool xhci_pass(uint64_t wake_began, uint64_t deadline, bool *work_left);
// Task-context bond storage; publishes restored bonds to the connection
// manager and rings kusb, whose pass runs the connection policy.
void xhci_bluetooth_maintain(void);
// Enable the manager after boot-time mounts and configuration have settled.
void xhci_bluetooth_start_manager(void);

// Non-blocking sysfs commands refuse absent/busy/failed adapters or invalid input.
bool xhci_bluetooth_scan(void);
bool xhci_bluetooth_connection(const char *data, size_t bytes);
// file: 0 scan status, 1 discovery results, 2 LE connection status.
size_t xhci_bluetooth_read(char *out, size_t capacity, unsigned file);
// /sys/usb: the controllers and what is on each, kusb's passes and
// latencies, how long an event on each controller may have waited, and the
// Bluetooth timeline. Returns the bytes written, terminated;
// a caller that finds USB busy is told so in the text.
size_t xhci_usb_report(char *out, size_t cap);

#endif
