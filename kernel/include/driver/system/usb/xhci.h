#ifndef XHCI_H
#define XHCI_H

// xHCI (USB 3.x host controller) + USB HID boot-protocol keyboard — v1.
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
//   - ROOT PORTS ONLY, no hubs: the keyboard must be plugged straight into
//     the machine. (Keyboards with built-in hubs enumerate AS hubs — those
//     need the hub slice, which is future work.)
//   - ENUMERATION AT BOOT ONLY: no hotplug. Plug it in, then power on.
//   - One controller, first boot-protocol keyboard found wins.
//   - Handles BOTH context sizes (HCCPARAMS1.CSZ): QEMU uses 32-byte
//     contexts, real hardware frequently uses 64 — the P5 gets to choose.
//   - Scratchpad buffers allocated when the controller demands them
//     (QEMU demands none; real silicon usually does).
//
// Delivery: HID reports are translated (usage -> ascii, shift/caps/ctrl
// semantics identical to the PS/2 driver, Ctrl+letter strips to control
// codes) and handed to keyboard_deliver_event() — the shared choke in
// keyboard.c — so husk, cat, the console EOF logic and the GUI queue
// cannot tell which century of keyboard the bytes came from.

#include <stdint.h>
#include <stdbool.h>

// Probe PCI for an xHCI controller (class 0x0C / subclass 0x03 / prog-if
// 0x30), bring it up, and enumerate root-port devices looking for a HID
// boot keyboard. Safe to call when no controller exists (does nothing).
// Call BEFORE task creation: the MMIO mapping lands in the kernel PML4's
// upper half so every later task inherits it.
void init_xHCI(void);

// Drain the event ring: completed keyboard reports are translated and
// delivered, transfer TRBs re-armed. Called every scheduler pass from
// processSignals (BSP only — one consumer, by design); cheap when idle
// (one cached-RAM read). Safe to call before init / with no keyboard.
void xhci_poll(void);

#endif
