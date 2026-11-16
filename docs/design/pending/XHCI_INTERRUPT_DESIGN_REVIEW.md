# XHCI_INTERRUPT.md — D1–D12 design review

Reviewed by Codex, 2026-11-12, in `xhci-x2`, branch
`fable/xhci-x2-daemon`, at `ec880c6d87db3659cb91f57882f268a24367ab8e`.
The reviewed [design](XHCI_INTERRUPT.md) has SHA-256
`5a646483ff86953c2781442cc42874ed1357b9ee8305dbb2a0fa7ade9a778f1c`.
Line numbers below identify that snapshot. The previous brief review remains
in [XHCI_INTERRUPT_REVIEW.md](XHCI_INTERRUPT_REVIEW.md).

The architecture is sound: one event consumer, a separate BSP-pinned daemon,
stable controller identity in the top half, and disk I/O outside the USB flag.
The three findings from the brief are addressed. I would resolve the four
findings below before treating D1–D12 as the implementation contract. They
affect the hardware protocol and the timing evidence, rather than the choice
of daemon or interrupt transport.

1. **[P2] Do not write an unchanged ERDP on every idle timer wake.**
   D2, lines 228–232; D6, lines 349–358.

   The unconditional write includes an empty controller with no new interrupt
   and no events consumed since the previous pass. At 100 Hz it repeatedly
   writes the same dequeue address and EHB bit. Intel's register contract
   prohibits consecutive identical ERDP writes except for a full-to-empty
   ring advancement. An idle tick is not that exception. This would introduce
   out-of-contract MMIO traffic even in X.2, before interrupts are enabled.
   See [xHCI 1.2b §§4.9.4 and 5.5.2.3.3, pages 165 and 394][xhci12b].

   Separate publishing consumed progress from completing a pending interrupt.
   An idle visit with neither progress nor outstanding EHB work should perform
   no ERDP write. Specify the stale-IRQ/tick overlap separately, including how
   EHB retirement is handled without treating every timer visit as new work.
   Do not simply restore the old `handled > 0` rule without accounting for
   that overlap.

   **Proof:** record register writes over multiple empty tick wakes, an empty
   wake after an already-serviced interrupt, and a genuine full-ring wrap.
   Assert the first case does not repeatedly write ERDP. This is a hardware
   contract finding; a specific controller malfunction was not reproduced.

2. **[P2] A truncated drain needs a different ERDP value from an empty drain.**
   D2, lines 228–236; D6, the budget-exhausted case at line 356.

   The design introduces `xhci_drain_events(max)` and explicitly defines the
   outcome where it stops with the next slot still valid, but retains the
   current dequeue-pointer write rule. In the existing code, `evt_dequeue`
   advances after each event and the final write uses that next slot
   (`xhci.c:473–486`). That works when the loop has found an empty slot. It is
   not the correct acknowledgement when the next slot holds unprocessed work.

   Keep the software cursor separate from the hardware acknowledgement. On an
   early stop with work remaining, publish the last handled TRB; after finding
   a cycle mismatch, publish the empty slot. Otherwise the acknowledgement
   includes an event the software has not handled. This exact distinction is
   explained in [the Linux xHCI maintainer's correction][linux-erdp].

   **Proof:** use a small drain limit with several valid events, then exercise
   both early-stop and empty exits across ring wrap. Assert that the early-stop
   write names the last consumed event and that the remaining events survive.
   The proposed runtime limit is a whole ring, which restricts when this case
   can arise; this finding concerns the early-stop outcome the new helper and
   D6 explicitly promise to support, especially if the time budget interrupts
   a batch. It is not a claim that the current unbounded drain already loses
   these events.

3. **[P2] Checking time between whole rounds does not bound timer service to one tick.**
   D2, lines 214–223 and 234–236.

   A round visits every controller and can process `RING_TRBS` callbacks per
   controller before the outer loop checks its time budget. The proposed
   helper takes a count limit, not a deadline. One round can therefore exceed
   10 ms, and a round begun just before the deadline can overshoot it by nearly
   its entire duration. The statement that a busy controller cannot hold the
   Bluetooth step off beyond the budget does not follow from this algorithm.

   This matters because Bluetooth receive rearming happens in that delayed
   step: `xhci_bt_transfer` sets `rearm_pending`, and
   `xhci_bt_runtime_poll` posts the next receive. HID callbacks also include
   terminal operations and optional logging, so a TRB count is not a time
   bound. Copying `knet`'s round-boundary check does not establish USB's stronger
   timing claim.

   Check an absolute deadline within bounded event batches and between
   controllers, retain the next controller to service, and run the runtime
   step before starting more drain work once that deadline is reached. State
   the residual bound as one indivisible callback/batch plus scheduling delay;
   measure it. If a callback can do lengthy work, the input audit must decide
   where that work belongs.

   **Proof:** inject callback costs into the host model and assert timer
   service and rotation under load. A synthetic round of three 4 ms controller
   batches already reaches the timers at 12 ms despite a 10 ms budget. This is
   an algorithm counterexample, not a benchmark of the target hardware.

4. **[P2] Preserve cadence counters, but do not use them as an event-wait bound.**
   D10, lines 420–422; D11, lines 430–436.

   The existing instrument records poll-start to poll-start time and assigns
   that gap to a poll that consumed events (`xhci_poll_began` /
   `xhci_poll_ended`, `xhci.c:331–354`). Its explanatory comment calls this an
   upper bound on event residence. D10 carries that meaning into a daemon
   which can spend a substantial budget between its start and a particular
   controller's drain. The start gap excludes that time. If work carries over,
   it also cannot establish when the retained event first became pending.

   For example: previous wake starts at 0 ms, an event arrives at 9 ms, the
   next wake starts at 10 ms, and its controller is reached at 22 ms. The gap
   bucket records 10 ms while the event waited 13 ms. A prompt wake followed
   by a slow drain can therefore appear healthy in the proposed acceptance
   metric. The generic doorbell stamp also combines tick, IRQ, and release
   wakes; it is not a per-event completion timestamp.

   Retain `poll_gap_*` as cadence measurements. Add per-controller service
   timing that includes work before that controller is reached, and preserve
   pending/backlog age across unfinished service where needed. Label IRQ-to-
   service observations as such; use a QEMU/model producer timestamp for exact
   completion-to-drain tests. D11 should compare a metric with the same meaning
   on both sides, rather than accepting the change from gap buckets alone.

   **Proof:** include the synthetic trace above and a deliberately delayed
   controller visit after an immediate wake. The report must expose the delay
   or explicitly state that its cadence metric does not measure it.

The earlier findings and the remaining design choices were checked as follows:

- D7 retains the interrupt-visible upper-half mappings and keeps the top half
  away from `s_hc`. The ring-3 interrupt test remains in the proof plan.
- D6 clears `USBSTS.EINT` before `IMAN.IP` and retains `IE`. Its two marked
  specification facts check out: interrupt assertion sets EHB, and MSI/MSI-X
  message completion clears IP automatically. Further signalling also depends
  on moderation and enable state, so “signals again” is eventual, subject to
  those gates. See [xHCI 1.22 §§4.17.2, 5.4.2 and 5.5.2.3.3][xhci122].
- The three-case baseline/IRQ/tick experiment is preserved, and the X.1
  measurements replace the old assumption that today's input waits seconds.
  Those measurements were read from the design; I did not repeat them.
- D4's set-before-try / release-before-exchange handshake has no lost-release
  wake in a small sequentially consistent atomic model: all 20 interleavings
  of the daemon's three steps and one holder's three release steps passed.
  Implementation still needs ordered atomic accesses; this model does not
  validate compiler choices, weak ordering, scheduler behavior, or starvation.
- D3 removes both worker policy calls while preserving the existing runtime
  policy call. Sleeping between post-I/O lock attempts retains the snapshot
  and generation check without spinning for a daemon-length critical section.
  No additional design finding was established in that handoff.
- D5/D7 publication should be implemented as a stable published prefix if
  controllers are enabled one at a time: initialized pointer first, count
  published with ordering, then that controller's interrupt gates. The prefix
  can grow while an earlier controller interrupts; it must not expose a
  partially initialized entry. This is implementation guidance, not a finding
  that count-last publication is inherently unsafe.

Validation was source inspection of the current USB drain, transfer/runtime,
bond and measurement paths; `doorbell.c`, `knet.c`, `kernel.c`, RTL8125 MSI,
and selected input-delivery paths; the linked primary specification and Linux
patch; and the small abstract interleaving/timing checks described above. The
full D3 transitive input audit remains an implementation prerequisite. No
kernel build, sanitizer campaign, QEMU boot, or hardware run was performed.
The design, tracker, implementation, and previous review were left unchanged.

[xhci12b]: https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf
[xhci122]: https://www.intel.in/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf
[linux-erdp]: https://lkml.iu.edu/hypermail/linux/kernel/2402.0/03211.html
