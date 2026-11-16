# XHCI_INTERRUPT.md — design review

Reviewed by Codex, 2026-11-12, against local `userland` at
`249803cb385aa7bfb37966d514e8d223b2c6f8bd`.
The brief was untracked; the reviewed file's SHA-256 is
`b6760485bf4196a41d72e18e52d0fc26d8e658e4c9f9f12f945c550e09ce9a1c`.
Line references below identify that snapshot of [the brief](XHCI_INTERRUPT.md).

The separate USB daemon, lock-free interrupt top half, retained timer wake,
and separation of bond disk I/O are the right direction. The brief correctly
requires measurement before attributing the slow LE reconnections to polling.
Resolve the following three findings when expanding it into the concurrency
design. The first two affect correctness; the third affects the acceptance
experiment.

1. **[P1] The interrupt handler still needs mappings visible under the interrupted CR3.**
   Lines 102–105 say the any-CR3 constraint goes away with the kernel daemon
   and invite removal of the machinery that served it. That is true for the
   daemon's drain, but not for the proposed top half: it must access operational
   and runtime MMIO registers while an arbitrary task is interrupted.

   The cited precedent, `handler_r8125_msi.S:22–41`, calls C and acknowledges
   the LAPIC without switching CR3. `xhci.c:1693–1700` maps the register BAR
   through the shared upper half precisely to make such accesses valid. If that
   mapping is replaced with a kernel-only mapping under this instruction, USB
   interrupts during user execution can fault in ring 0.

   Retain interrupt-visible mappings for the registers and state the handler
   touches, or explicitly design and prove a different entry path. Require a
   QEMU case with USB interrupts arriving during ring-3 execution; a daemon-only
   drain test cannot establish this property.

2. **[P2] Make the interrupt acknowledgement order explicit and correct.**
   Lines 66–68 list clearing interrupter pending before controller
   event-interrupt status. Implemented in that order, the proposed handler
   violates the register contract: clear `USBSTS.EINT` before `IMAN.IP`.
   Intel describes a lost-indication race if another IP transition occurs
   between the reverse-ordered writes. See
   [xHCI 1.22 §5.4.2, page 398][xhci-spec].

   State the order in the design, preserve `IMAN.IE`, and use deliberate
   write-one-to-clear masks. Define the bottom half's `ERDP.EHB` completion
   separately from those top-half acknowledgements. The existing drain clears
   EHB only after consuming at least one event (`xhci.c:383–388`); the final
   design needs to account for IRQ/tick overlap and any drain budget rather
   than treating the polling implementation as the complete IRQ protocol.

   Add a register-model test for acknowledgement ordering and an arrival
   racing acknowledgement/rearm. The finding is the specified write order;
   an additional lost-EHB bug in the eventual implementation is not asserted
   here.

3. **[P2] The force-tick mode cannot reproduce the old scheduler-poll behavior.**
   Lines 131–135 ask the fallback token to demonstrate the old behavior remains
   reachable. But lines 69–73 and 88–91 define a different fallback: a daemon
   explicitly woken each tick. The old path was drained opportunistically by
   `processSignals`; that is the source of the historical 1,000-tick symptom.
   Disabling hardware IRQs while retaining the new timer wake does not restore
   that old scheduling path.

   Keep the new fallback, but make the experiment three separate cases:
   measure the pre-change build; measure the new build with IRQs; measure the
   new build with IRQs forced off. Require progress at the new timer cadence
   in the third case. Use IRQ/fire counters and completion-to-drain latency to
   distinguish it from the second. Otherwise successful timer-driven input can
   either fail an impossible expected result or hide broken IRQ delivery.
   Re-measure the historical baseline on the current tree; DEBTS records an
   earlier experiment, not a guaranteed result on this revision.

The implementation design should carry these ownership and progress proofs:

- **Controller identity in the top half.** `s_hc` is mutable scratch selected
  while boot code or the poll owner processes a controller (`xhci.c:249–252`).
  An interrupt must identify its own stable controller without reading that
  selector or taking `s_poll_busy`. A shared vector can scan stable controller
  descriptors; individual vectors can resolve directly to them.
- **Bounded work and retained work.** `xhci_drain_events` drains until empty;
  `knet` has a time/round budget and rerings itself when work consumed that
  budget. Define USB's budget and how the next controller and Bluetooth timers
  get service. If `s_poll_busy` is held by a sysfs or bond operation, specify
  how the consumed bell is retried; the tick can provide a bounded backstop,
  but the measurements should expose that delay.
- **Boot-to-runtime handoff.** Keep synchronous enumeration and firmware load
  serialized with the event consumer. Publish stable handler state and a usable
  wake path before unmasking interrupts, and account for events left by boot
  polling. Cover multiple controllers and a pending event at enable time.
- **Bond lifecycle, including reads.** Preserve startup bond loading, stale-save
  generation checks, and retries after failed I/O. The worker handoff must
  support loading existing bonds as well as writing new ones.

These are required proofs for the design expansion, not claims that a missing
implementation already fails them. Likewise, keeping a scheduler backstop is
an open design choice; it must not accidentally create a second unprotected
event consumer.

One source fact helps scope the work: Bluetooth policy is already invoked at
the end of `bt_host_tick`, reached from `xhci_bt_runtime_poll`, as well as from
`xhci_bluetooth_maintain`. Moving the poll to the daemon therefore moves an
existing policy call. Audit and reconcile both callers rather than adding a
third. The worker's separate role is disk I/O and publishing its result.

I favor a separate USB daemon so USB service budgets and diagnostics remain
independent of network traffic. The existing doorbell implementation is the
appropriate primitive; retain its take-before-work and park/recheck protocol.

Validation performed: source inspection of `xhci.c`, `bt_host.c`, `bt_le.c`,
`knet.c`, `doorbell.c`, `signals.c`, `kworker.c`, the input delivery paths,
`r8125.c`, `handler_r8125_msi.S`, and `idt.c`; acknowledgement order checked
against the Intel specification. No implementation, build, QEMU boot, or
hardware test was performed for this review. The brief was left unchanged.

[xhci-spec]: https://www.intel.in/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf
