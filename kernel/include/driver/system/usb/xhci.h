#ifndef XHCI_H
#define XHCI_H

// xHCI (USB 3.x host controller) + USB HID boot-protocol keyboard/mouse.
//
// WHY THIS EXISTS: the Bosgame P5 has no PS/2 port. Every keystroke it will
// ever receive arrives over USB, so "os64 runs on real hardware" requires
// this driver. (2026-07-19: "being able to run on actual hardware ...
// that's kind of the whole point." — the owner, correctly.)
//
// SHAPE OF V1 (each limit is a decision, not an accident):
//   - POLLING, not interrupts: no MSI/IOAPIC wiring. The event ring lives in
//     ordinary RAM; xhci_poll() checks one cycle bit per scheduler pass
//     (processSignals), the same liveness path the console reader uses.
//     ~10ms worst-case key latency — the PS/2 wake path's class.
//   - ROOT PORTS ONLY, no hubs: input devices must be plugged straight into
//     the machine. (Devices with built-in hubs enumerate AS hubs — those
//     need the hub slice, which is future work.)
//   - ENUMERATION AT BOOT ONLY: no hotplug. Plug it in, then power on.
//   - Controllers are searched until the first boot-protocol keyboard and
//     first boot-protocol mouse have been found; they may live on different
//     controllers.
//   - Handles BOTH context sizes (HCCPARAMS1.CSZ): QEMU uses 32-byte
//     contexts, real hardware frequently uses 64 — the P5 gets to choose.
//   - Scratchpad buffers allocated when the controller demands them
//     (QEMU demands none; real silicon usually does).
//
// Delivery: keyboard HID reports flow through keyboard_deliver_event(); mouse
// reports flow through input_inject_mouse(). The console and GUI therefore do
// not need to know whether input arrived over PS/2 or USB.

#include <stdint.h>
#include <stdbool.h>

// Probe PCI for an xHCI controller (class 0x0C / subclass 0x03 / prog-if
// 0x30), bring it up, and enumerate root-port devices looking for a HID
// boot keyboard and mouse interfaces. Safe to call when no controller exists.
// Call BEFORE task creation: the MMIO mapping lands in the kernel PML4's
// upper half so every later task inherits it.
void init_xHCI(void);

// Drain every active controller's event ring: completed keyboard/mouse reports
// are translated and delivered, and transfer TRBs are re-armed. Called every
// scheduler pass from processSignals; internally serialized across cores and
// cheap when idle. Safe to call before init or with no USB input devices.
void xhci_poll(void);

#endif
