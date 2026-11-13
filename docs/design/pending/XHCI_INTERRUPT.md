# XHCI_INTERRUPT.md — brief: the USB controller interrupts, and a daemon answers

*Written 2026-11-12 by Fable for Chris, who ruled the same day that this is
its own arc with one agent on it and nothing else. It is a BRIEF: what is
wanted, what the tree has, the shape I would build, what must be proved,
and what is not decided. It was written from a partial read of the driver
(the functions named below, not all 2,000 lines), so the builder checks
every anchor against the source before relying on it, and where the source
disagrees with this page the source is right and the page is corrected in
the same PR. It is a kernel concurrency change: the design it becomes gets
a Fable read before code, and the code gets Codex.*

*Revised the same day after Quinn's review (XHCI_INTERRUPT_REVIEW.md, beside
this file). Her three findings were right and are folded in: the top half
runs under whatever CR3 was interrupted, so the register mapping that makes
that safe stays; the acknowledgement has an order; and the token that
forces the tick is a third case to measure, not a way back to the old
behaviour. Her list of what the design must prove is § What the design
must prove.*

*§ The design (added 2026-11-12, tracker row X.2.1) is the brief become a
design: it answers § What the design must prove item by item and is what
X.2 and X.3 build. Where an earlier section and § The design differ, § The
design is the later word.*

## The tracker

`XHCI_TRACKER.csv` beside this file is where the arc stands: three slices
(`X.1` measure, `X.2` the daemon, `X.3` the interrupt), their sub-tasks,
and the questions of § Decided, and what is still open as `Q` rows. `~/tracker.sh xhci` draws
it; `tools/TRACKERS.md` is the format. The rules are this arc's:

- The design (`X.2.1`) may add, split or drop sub-task rows, in the PR
  that carries it. A dropped row stays, with the reason in its notes.
- A row is flipped by the branch that does its work, in that PR; `done`
  and the PR number land as the PR's last commit. A branch touches its
  own slice's rows.
- A `Q` row is ruled by Chris, and the ruling is written into its notes.
- `python3 -I tools/tracker.py --csv docs/design/pending/XHCI_TRACKER.csv
  --quiet` passes before a push that touches the file.

## What is wanted

Every USB device on an xHCI controller is heard when it speaks, not when
the scheduler next happens to pass: the keyboard, the mouse, the Bluetooth
adapter and everything behind it (the LE keyboard and trackball today,
headphones after BLUETOOTH_AUDIO.md).

Two things are waiting on it:

- **Bluetooth audio** needs a packet out every 15 to 20 ms and its credits
  back as they are returned. It cannot be built on a poll that can be
  seconds apart (BLUETOOTH_AUDIO.md § Pacing).
- **The LE keyboard and trackball are slow to connect** when switched on
  (Chris), and DEBTS records a boot where they were ready after about 118
  and 153 seconds with the cause unresolved. Polling is the first suspect.
  It is NOT proven to be the cause; § Measure first is how that is
  settled, and the arc is worth doing whichever way it comes out.

## What the tree has

- `xhci_poll` (`kernel/src/driver/system/usb/xhci.c`) is called from
  `processSignals` (`kernel/src/signals.c`), so on every scheduler pass and
  possibly from any core. For each controller it runs `xhci_drain_events`,
  `xhci_bt_runtime_poll` and the HID keyboard's typematic tick, under a
  non-blocking busy flag (`s_poll_busy`): a caller that loses leaves the
  work for a later pass.
- `xhci_drain_events` carries the comment "This is the whole interrupt
  handler, minus the interrupt", and controller setup leaves interrupter 0
  disabled on purpose. The driver was written expecting this arc.
- Several controllers can be live at once (`s_controllers[]`); the P5 has
  one in the CPU and more beside it. Each is drained.
- `xhci_bluetooth_maintain` runs from `kworker` and owns Bluetooth's disk
  I/O (bonds) and its policy step (`bt_host_policy`: reconnection scans and
  the like). It takes the same busy flag.
  It also holds a second flag, `s_bond_io_busy`, across the disk I/O, and
  after the I/O it SPINS for the poll's flag rather than leaving: the one
  caller that waits for it.
- Reconnection is scan-driven, with a backoff (`bt_manager.c`,
  `manager_retry`): after a scan round that did not connect the peer, the
  next round waits 2 s, doubling to a ceiling of 30 s. A device switched on
  just after a round therefore waits out the backoff before anything can
  see it, however often the poll runs.
- The DEBTS row "xHCI is polled, and the poll rides dispatch frequency"
  has the measurement: on an idle tickless GUI boot with no logd heartbeat,
  HID reports arrived spaced exactly 1000 ticks apart.
- **The precedents are in the network stack and should be copied, not
  reinvented.** DOORBELL.md and `kernel/src/knet.c`: an interrupt handler
  rings a doorbell (lock-free: a store and a self-IPI; it never takes a
  lock the bottom half holds), and a kernel daemon pinned to the BSP wakes,
  drains, runs timers and parks. The tick rings the same bell, which keeps
  a device with no interrupt at its old cadence. `r8125.c` is the MSI
  precedent (the vector, addressed at the BSP, enabled at the platform
  moment the LAPIC can take it, with `fires` and `spurious` counters);
  `e1000.c` is the INTx one, with a probe and the lesson written beside it
  about a level-triggered line that is not acknowledged.

## The shape I would build

1. **A top half per controller.** MSI where the controller offers it
   (MSI-X if that is all it offers), INTx otherwise, and a controller that
   can be given neither stays tick-driven and says so. The handler does
   what makes the interrupt stop and nothing else, IN THIS ORDER: it
   clears the controller's event-interrupt status (`USBSTS.EINT`), then
   the interrupter's pending bit (`IMAN.IP`), each with a deliberate
   write-one-to-clear mask that leaves `IMAN.IE` set; rings the USB
   doorbell; and returns. The other order can lose an indication that
   arrives between the two writes (xHCI 1.2 § 5.4.2). It touches no event
   ring and takes no lock. Telling the controller the event ring has been
   read (`ERDP` with its busy bit) is the BOTTOM half's, after it has
   consumed events, and the design says what happens when an interrupt
   and a tick overlap, or a drain stops at its budget with events left.
   The handler finds its own controller from state that never changes
   after bring-up: `s_hc` is scratch the poll's owner points at whichever
   controller it is working on, and the handler may not read it.
2. **A bottom half that is a daemon**, in `knet`'s shape and probably named
   for USB: woken by the bell, it does exactly what `xhci_poll` does today,
   for every controller, then parks. The tick rings it too, which is what
   runs the keyboard's typematic repeat, Bluetooth's timers and any
   controller left without an interrupt.
3. **`xhci_poll` leaves `processSignals`.** One caller, on one thread, is
   what makes the event ring's ownership simple; the busy flag stays for
   the callers that remain (sysfs snapshots, the Bluetooth requests,
   `xhci_bluetooth_maintain`). Whether a backstop call survives somewhere
   is the builder's to argue.
4. **Bluetooth's clockwork moves with it.** What is timing (the policy
   step, reconnection, later the audio pump's neighbour) runs from the
   daemon; what is disk I/O stays in `kworker`. The daemon never touches a
   disk. (This item proposed that the daemon nudge `kworker` when there is
   a bond to write; § The design, D3, declines that and says why.)
5. **A report**: `/sys/usb`, which X.1 created with the poll's own
   numbers, grows one block a controller: how it is driven (MSI, INTx or tick), fires and
   spurious, the daemon's wakes, and how long a ring waited to be answered,
   since boot and since last read. `/sys/net/knet` is the model, down to
   reading it FIRST when a device feels late.
6. **A flashlight.** A cmdline token that leaves every controller
   tick-driven, as `NETPOLL` does for the NICs. It is NOT the old
   behaviour: the old path was drained when the scheduler happened to
   pass, and this one is drained by a daemon the tick wakes. It is a
   third thing to measure (§ Proof) and the fallback for a controller
   whose interrupt cannot be had. Like every token it does not travel to
   the P5: say so where it is added.

## Things that will bite

- **A level-triggered INTx line shared with another device** storms if the
  handler returns without quieting its own device, and wedges the line for
  its other owner. The e1000's probe met this; read its comment first.
- **Which vector, and which core.** The RTL8125 has vector 0x46, addressed
  at the BSP. The APs mask low vectors by their task priority, and the
  tickless idle path masks AP timers, so the daemon is pinned to the BSP
  as `knet` is unless the design shows why not.
- **The top half runs under whatever CR3 was interrupted.** The register
  file is mapped at an upper-half alias so that it is reachable from every
  task's page tables (the comment where the BAR is mapped says why: the
  poll ran under whoever was live). The DRAIN no longer needs that once it
  is a daemon's, but the HANDLER does, more than the poll ever did: it
  writes two registers while an arbitrary program is interrupted, as the
  RTL8125's handler does without switching CR3. Keep that mapping, and
  keep everything the handler reads reachable the same way. (The first
  version of this brief said the constraint goes away. It does not; Quinn
  caught it.) A QEMU case must take USB interrupts while a ring-3 program
  is running: a test in which only the daemon drains cannot show this.
- **Interrupt moderation** (the interrupter's IMOD register): unset, a
  mouse at a high report rate interrupts for every report. Pick a value,
  write down why, and measure the wakes.
- **Firmware startup** on the AX210 interrupts a receive without data (the
  driver already notes it). Bring-up runs before the daemon exists, or the
  daemon must be told to stand back.
- **Suspend and resume** of a controller, if the tree does either, must
  leave the interrupt as it found it.

## The design (X.2.1)

*Written 2026-11-12 by Fable from a read of `xhci.c`'s boot, poll, drain
and Bluetooth paths, `doorbell.c`, `knet.c`, `kworker.c`, the RTL8125's
MSI, and § What was measured. It is what X.2 and X.3 build. It awaits a
read before code; where the builder finds the source disagrees, the source
is right and this section is corrected in the same PR.*

*Revised the same day after Quinn's read (XHCI_INTERRUPT_DESIGN_REVIEW.md,
beside this file). Her four findings were right and are folded in: an idle
pass must not rewrite an unchanged `ERDP`; a drain that stops early
acknowledges the last event it handled, not the next slot; a budget
checked between whole rounds bounds nothing; and the gap between passes
is a cadence, not a bound on how long an event waited. She also checked
the two statements about the specification that D6 had marked as from
memory, and they hold.*

### D1. Who owns what, after the arc

| Thing | Owner at boot (before `scheduler_enable`) | Owner at run time |
|---|---|---|
| A controller's event ring, `evt_dequeue`, `ERDP` | the boot thread, synchronously (`xhci_run_command`, `xhci_control_request`, `xhci_bt_wait`) | `kusb`, under the serializing flag |
| `s_hc` (the scratch "controller I am working on") | the boot thread | `kusb`; nothing else reads or writes it |
| `USBSTS.EINT`, `IMAN` | nobody (interrupter left off) | the top half writes them; `kusb` writes them once, when it enables interrupts |
| Bluetooth host state (`bt_host_t`, scans, LE peers, managers) | the boot thread | whoever holds the serializing flag: `kusb` for timing, sysfs for a snapshot or a request, `kworker` for a bond |
| Bond files | nobody | `kworker`, outside the flag |
| The poll's measurements and the timeline | nobody | `kusb`, under the flag |

There is ONE runtime consumer of every event ring. That is the property
the arc buys before any interrupt exists, and X.2 is the slice that
establishes it.

### D2. The daemon: `kusb`

A kernel task created beside `knet` in `kernel_init`, before
`scheduler_enable`, pinned to the BSP, only when USB is enabled and at
least one controller came up. Its bell is `kUsbDoorbell`, registered at
the top of its body. The tick rings it once a tick from `processSignals`,
with `knet`'s once-per-tick guard and for `knet`'s reason; `xhci_poll`'s
call there is deleted, and no call to the drain remains on the scheduler's
path (Q3).

Its loop keeps the doorbell's protocol untouched (take before work; park
re-checks under the queue lock). The drain is NOT `knet`'s rounds: a
budget checked between whole rounds bounds nothing, because one round can
itself run past it (Quinn's counterexample: three controllers at 4 ms a
batch reach the timers at 12 ms under a 10 ms budget).

1. `doorbell_take`. Time the wait as `knet` does, wake and response kept
   apart.
2. Take the serializing flag (D4). If it cannot be had, go to 6.
3. **Drain, against a deadline.** The deadline is this wake's start plus
   ONE tick of TSC time: one tick, where `knet` takes two, because the
   consumer this arc exists for paces at 15 to 20 ms and step 4 is its
   clock. The controllers are visited once each, starting at a cursor that
   persists across wakes. Each is drained until its ring is empty or the
   deadline passes, and the deadline is checked BEFORE EVERY EVENT, not
   between batches. A controller stopped by the deadline with work left
   moves the cursor to the controller after it, so the one that used the
   time is served last next wake, and the visit ends there.
4. **Once per wake, after the drains, whatever the deadline did**: for
   each controller, the Bluetooth runtime step (`xhci_bt_runtime_poll`,
   which is also what re-arms the adapter's receive endpoints) and the
   keyboards' typematic ticks.
5. Release the flag. If any controller was left with work, count it and
   raise the bell so the park returns at once.
6. `doorbell_park` with a one-second backstop.

**What that bounds.** From a wake's start to step 4 is at most one tick
plus ONE event's handling: the deadline cannot interrupt a callback. A
callback is a HID report or a Bluetooth packet delivered to its consumer,
and X.2.11's audit says how long the longest of those can run (a terminal
operation, a log line) and moves out of the drain any that can run long.
The scheduler's delay in running `kusb` at all is on top, and is what the
wake latency measures. The longest start-to-step-4 time is reported.

**`ERDP` is written for two reasons, and never for neither** (D6 has the
protocol). A drain that handled at least one event publishes its
progress. A drain on a controller whose top half has acknowledged an
interrupt since the last write retires the busy bit. A pass that did
neither writes nothing: an idle tick must not rewrite an unchanged
pointer (xHCI 1.2b § 4.9.4 and § 5.5.2.3.3 forbid consecutive identical
writes except for a ring that went from full to empty).

**What is written depends on why the drain stopped.** `evt_dequeue` is
the software's cursor: the next slot to look at. When the drain stopped
because that slot is not yet valid, the ring is empty and `ERDP` names
that slot. When it stopped at the deadline with the next slot valid,
`ERDP` names the LAST EVENT HANDLED, so the acknowledgement never covers
an event the software has not handled. The cursor and the acknowledgement
are two values, kept apart.

`xhci_drain_events` takes the deadline and returns what it handled and
which way it stopped. The boot-time callers pass no deadline.

### D3. What moves, and what does not

- **Input delivery leaves the scheduler's pass for a thread.** Today a HID
  report is delivered inside `processSignals`, under the queue lock, and
  its reader is woken by the console check a few lines below in the same
  pass. From `kusb` the report is delivered in thread context and the
  reader is woken by the pass `kusb`'s own park provokes, as DOORBELL.md
  found for `knet`. The delivery code takes its own locks
  (`gui/input.c`'s `s_input_lock`, irqsave) and calls no `_locked`
  scheduler verb. THE BUILDER AUDITS, before coding, every function a HID
  report or a Bluetooth input report reaches (the keyboard interpreter and
  its VT chords, the tty input ring, the GUI queue, pointer registration)
  for an assumption that the queue lock is held or interrupts are off, and
  lists what it found in the PR.
- **Bluetooth's clockwork** is `bt_host_tick`, reached from the runtime
  step, so it moves with the drain and needs no change of its own.
- **`bt_host_policy` has one timing caller: the end of `bt_host_tick`.**
  The call at the top of `xhci_bluetooth_maintain` is deleted: the daemon
  runs the policy every wake, which is at least every tick, and
  `kworker`'s visit every two seconds adds nothing to that. The call
  after a bond is loaded becomes a ring of `kUsbDoorbell`: `kworker`
  publishes what it read and the daemon's next wake, microseconds later,
  starts the discovery. `kworker` no longer calls the policy at all.
- **`kworker` keeps the disk, reading as well as writing**, exactly as it
  has it: the load of each slot's bond at startup, the snapshot taken
  under the flag and the I/O done outside it, the generation check that
  stops a stale save acknowledging a forget, `next_save`'s retry after a
  failed write, and `s_bond_io_busy`. It is NOT nudged. A bond saved two
  seconds after it changed costs nothing, and a wake primitive for
  `kworker` would be a second thing to prove. (The brief proposed the
  nudge; this is the design declining it.)
- **`kworker` stops spinning for the flag.** After its disk I/O it spins
  with `pause` until the poll lets go. That was honest against a poll
  that held the flag for microseconds; `kusb` may hold it for a tick. It
  sleeps a tick between attempts.

### D4. The serializing flag, and work that is kept

`s_poll_busy` stays, a test-and-set flag with a new rule: it has ONE
release door, and the door knows whether the daemon was turned away.

- `kusb` sets `s_kusb_waiting`, then tries the flag. If it gets it, it
  clears `s_kusb_waiting` and works. If not, it counts the refusal,
  stamps the time, and parks.
- Every other holder releases through `xhci_serial_release()`: release
  the flag, then, if an atomic exchange finds `s_kusb_waiting` set, ring
  `kUsbDoorbell`.

Set-then-try on one side and release-then-check on the other close the
window: either the daemon's try succeeds, or the releaser sees the mark.
The bell was taken and the work was not done, and the holder's release is
what brings the daemon back, in the time the holder held and no longer.
The tick is behind that as it is behind everything. The report shows how
many times it happened and the longest wait from refusal to the pass that
did the work.

The other holders are bounded and never drain: a sysfs read renders a
status text; a sysfs write queues a request; `kworker` snapshots or
publishes a bond. None of them touches `s_hc` or an event ring.

### D5. From boot to running

Enumeration and the AX210's firmware load run on the boot thread before
`scheduler_enable`, draining synchronously, as today. `kusb` does not
exist then and no interrupt is enabled then, so they stay serialized with
the event consumer by construction: there is no moment with two.

**`kusb` enables the interrupts itself**, at the top of its body, after
its bell is registered, holding the flag (X.3). By then the handler's
state is complete and unchanging (D7), the IDT entry exists, and the wake
path works. For each controller, in this order:

1. Decide how it is driven (D8). If by the tick, stop here.
2. Program MSI or MSI-X at the vector, addressed at the BSP; set the PCI
   command register's INTx-disable bit.
3. Write the moderation interval (D9).
4. Clear `USBSTS.EINT` and `IMAN.IP`, whatever boot left there, and mark
   the controller as owing an `ERDP` write (D6).
5. Publish the controller to the handler's list: its entry written whole,
   then the count raised past it with a barrier. The list is a prefix
   that only grows; an earlier controller may already be interrupting
   while a later one is added, and the handler never sees a half-written
   entry.
6. Set `IMAN.IE`, then `USBCMD.INTE`.

Then it falls into its loop, whose first pass drains every controller and,
because step 4 marked each as owing one, writes its `ERDP` with the busy
bit. That is what accounts for events the boot left on a ring and for an
event pending at the moment of enabling: nothing depends on the
controller sending a message for a condition that existed before `IE` was
set.

A controller whose programming does not read back as written stays
tick-driven and says so, on the glass and in the report, as the RTL8125
does.

### D6. The interrupt protocol

An event sets the interrupter's pending bit and its EVENT HANDLER BUSY
bit (`ERDP.EHB`) together, and the controller signals. While `EHB` is
set, a further event signals nothing. Software clears `EHB` by writing
`ERDP` with that bit; if the ring is not empty at that write, the
controller signals again, in its own time: after the moderation interval
and only while the enables are set. With MSI or MSI-X the pending bit
clears itself when the message is sent; `USBSTS.EINT` does not. (xHCI
1.2 § 4.17.2, § 5.4.2, § 5.5.2.3.3; Quinn checked these against the
text.)

So the two halves divide like this:

- **The top half** (one shared vector, D7), for each published
  controller: read `USBSTS`; if `EINT` is clear, next controller. Else
  write `USBSTS` with `EINT` alone (the register's other write-one-to-clear
  bits stay as they are), then write `IMAN` with `IP` and `IE` both set
  (clears pending, keeps enable); set that controller's `ehb_owed` mark,
  stamp the time, count a fire. After the walk: if no controller had
  `EINT`, count a spurious entry; else ring `kUsbDoorbell`. No lock, no
  event ring, no `s_hc`, no print. EOI in the stub.
- **The bottom half** owns `ERDP`. Before it drains a controller it takes
  that controller's `ehb_owed` mark (an atomic exchange). After the
  drain:
  - handled events, stopped at an empty slot: write `ERDP` = that slot,
    with `EHB`.
  - handled events, stopped at the deadline: write `ERDP` = the last
    event handled, with `EHB`, UNLESS that is the slot the register
    already names. (The ring was last written empty at slot N and this
    drain handled the one event at N: the last event handled is N again.
    Writing it would be the same value twice running; nothing is
    written, and the next drain publishes past it. Codex, #559.)
  - handled nothing, mark taken: READ `ERDP`; if `EHB` reads set, write
    the pointer back with `EHB`, which is the one write that retires it;
    if it reads clear, write nothing.
  - handled nothing, no mark: write nothing.

  The third line is the only place an unchanged pointer is written, it is
  written once per acknowledged interrupt and only when the read shows the
  bit set, and it is what Linux's handler does at the end of every
  interrupt. THE BUILDER CHECKS that write against § 4.9.4's wording
  before coding; if the text forbids even that, the answer is in the
  specification's own description of how `EHB` is retired on an empty
  ring, and this line is corrected.

| Case | What happens |
|---|---|
| An event arrives while `kusb` is mid-drain | `EHB` is still set, so no message. Either the drain's loop sees the event, or its closing write finds the ring not empty and the controller signals again. |
| The drain stops at the deadline with events left | `ERDP` names the last event handled and clears `EHB` with the ring not empty: the controller signals again, and `kusb` has raised its own bell besides. The events left are untouched. |
| A tick wake and an interrupt overlap | One consumer, so there is no race on the ring. If the tick's pass took the event before the handler ran, the handler still sets the mark, and the pass that answers it finds nothing, reads `ERDP`, and retires `EHB` only if it is set. |
| Idle ticks, no interrupt, nothing on the ring | No mark, nothing handled: no write. A hundred ticks write `ERDP` zero times. |
| Boot leftovers; an event pending when `IE` is set | D5 marks every controller once at enabling, so the first pass retires whatever `EHB` state boot left. |
| The flashlight, or a controller left tick-driven | `IE` is never set and no mark is ever made: `ERDP` is written on progress alone, as the poll writes it today. |

### D7. The vector, the handler's state, and the core

One vector for every controller, `0x47`: class 4, beside the e1000's and
the RTL8125's, above the APs' priority bar and below the scheduler's
class. The P5 has five controllers; five vectors and five stubs would buy
only the attribution of a spurious entry. A shared vector is sound for
edge-triggered messages because the handler walks EVERY published
controller on every entry: a second message arriving in service sets the
LAPIC's request bit again and the handler runs again.

The handler reads a list of controller pointers and a count, written once
by `kusb` before any `IE` is set and never changed after. Through them it
reads `op` and `rt`, set at bring-up and never changed. The register file
is mapped at its upper-half alias, so all of it is reachable under
whatever CR3 was interrupted; that mapping and its comment stay, and the
comment gains the handler as its second reason.

Messages are addressed at the BSP and `kusb` is pinned there, for `knet`'s
reasons: the ring is then a self-IPI, and the tick that rings the bell
and preempts a busy drainer is the BSP's own (the tickless idle path
masks the APs' timers).

### D8. How a controller is driven (Q1, Q5)

A pure function of what `/sys/bus/pci` already knows and the flashlight:
MSI if the function offers it; else MSI-X; else the tick. The flashlight
forces the tick. No INTx. The P5's five offer both and take MSI; QEMU's
`qemu-xhci` offers MSI-X alone and `nec-usb-xhci,msi=on,msix=off` offers
MSI alone (read from `/sys/bus/pci` in QEMU, 2026-11-12), so QEMU
exercises both paths and the P5 proves MSI on hardware.

MSI is programmed as the RTL8125 programs it. That code becomes one
helper in `pci.c` that both drivers call; the RTL8125 moves to it in X.3,
and the P5 pass covers both. MSI-X needs its table: the capability names
a BAR and an offset; the table's pages are mapped at the upper-half alias
if the register mapping does not already cover them; entry 0 gets the
address and data and is unmasked, every other entry is masked, then the
capability's enable bit is set with its function mask clear.

### D9. Moderation (Q4, proposed)

`IMOD`'s interval is written as 1000, which is 250 µs: at most four
thousand interrupts a second from one controller, and at most a quarter
of a millisecond added to an event. The reason for a value at all is a
mouse at a high report rate; the reason for this one is that it is far
below a tick and far below audio's 15 ms. It is a proposal. X.3.5 measures
`kusb`'s wakes under the fastest mouse to hand with the value at 0, 1000
and 4000 (the controller's own default, 1 ms), and Chris rules on Q4 from
those numbers. QEMU's model may not moderate at all; the P5 is where this
is measured.

### D10. The report and the flashlight

`/sys/usb` grows. Per controller: `driven=` `msi`, `msix` or `tick`, and
why when it is the tick (`no capability`, `USBPOLL`, `did not take`), and
its `fires`. For the daemon, in `/sys/net/knet`'s words: wakes, wakes
that left work, longest wake, longest time from a wake's start to the
Bluetooth step (D2's bound), wake and response latency with their late
counts, the bell's rings and wakes; the handler's spurious entries; and
the refusals of the flag with the longest wait (D4).

**The gap between passes is a cadence, and is reported as one.** X.1's
`poll_gap_*` and `event_gap_*` measure start to start. For the old poll,
which held nothing and drained every ring to empty in microseconds, that
is also a bound on how long an event sat. For a daemon it is not: a wake
can start on time and reach a controller late (Quinn's trace: wakes at 0
and 10 ms, an event at 9 ms, its controller reached at 22 ms, a 10 ms gap
recorded for a 13 ms wait), and work carried over from one wake to the
next is older than any gap. So those lines keep their names and are
described as how often `kusb` comes round, nothing more.

**How long an event may have waited is measured per controller**, from
the last moment that controller's ring was SEEN EMPTY to the END of the
drain that found events on it. "Seen empty" is a time read BEFORE the
look that found the ring empty, inside the drain: read after it, an event
could arrive between the look and the stamp and be reported younger than
it is (Codex, #559 round 2). That interval includes whatever `kusb` did
before reaching the controller and the handlers that ran ahead of an
event inside the drain (to the drain's start, as first built, it missed
those: Codex, #559), and it keeps growing across wakes while work is
carried over, because a drain stopped by the deadline does not move the
stamp. Reported per controller as a maximum and the same four
buckets. For an interrupt-driven controller the time from the top half's
stamp to the start of that controller's drain is reported beside it, named
for what it is (interrupt to service), with a maximum and a late count.
Neither is a per-event completion time: the controller does not stamp its
events. The host model's producer does, and the model's tests assert
completion-to-drain exactly (D11).

The flashlight is the cmdline token `USBPOLL`: no controller is given an
interrupt. It does not travel to the P5; that is said where it is added
and in CLAUDE.md's table.

### D11. What each slice proves

- **X.2** ends with `kusb` running, every controller `driven=tick`, and
  `xhci_poll` gone from `processSignals`. Its proof is that nothing got
  worse, compared in ONE metric: the per-controller seen-empty interval
  of D10. X.1's start-to-start gaps were taken on the old poll, where
  they are an upper bound on that interval, so "no longer than X.1's
  reading, on the same QEMU entry and on the P5" is a fair comparison and
  errs against the new code. Beside it: input, Bluetooth pairing,
  reconnection and bond save and load as before; and D4's refusal path
  provoked on purpose (a loop of sysfs reads while a mouse moves) with the
  refusals counted and no event lost.
- **X.2's host model** drives the drain and the daemon's visit with a
  fake controller whose producer stamps each event and whose register
  file records every write, and with callbacks that cost injected time.
  It asserts: the Bluetooth step is reached within the deadline plus one
  callback under load (Quinn's three-controller trace among the cases);
  the cursor rotates so a busy controller does not starve the others;
  many idle passes write `ERDP` zero times; an early stop names the last
  handled event and the events left survive, across a ring wrap; an empty
  stop names the empty slot; a genuine full-to-empty wrap is written; and
  the reported seen-empty interval covers a deliberately late controller
  visit after a prompt wake.
- **X.3** adds the handler, the enabling, the choice, moderation and the
  flashlight, and proves D6's table: the same register model on the host
  for every row (the empty pass after an already-serviced interrupt among
  them), and QEMU with `fires` counted on both controller models.

### D12. What this design does not settle

- The one write D6 asks the builder to check against § 4.9.4: retiring
  `EHB` on an empty ring by writing the pointer it already holds.
- Whether a controller that is suspended and resumed exists in this tree
  at all. If it does, it must leave `IE`, `INTE` and the message
  programming as it found them; if it does not, the line is dropped.
- Whether the timeline and the gap buckets stay once the arc is done, or
  shrink to what `/sys/net/knet` has. They stay through X.3 because they
  are the proof.
- How long the longest delivery callback runs, until X.2.11's audit has
  measured it. D2's bound is "one tick plus one callback", and it is only
  as good as that number.

### X.2 as built (2026-11-12)

What the code is, where it says more than D1 to D12 or differs.

- **The arithmetic is its own file.** `xhci_service.c` holds the drain
  against a deadline, what `ERDP` is owed, the wait bound and the visit's
  order, with no kernel headers. `xhci.c` keeps the registers and calls
  it. The `ehb_owed` rule of D6 is in that file and tested there; X.2's
  `xhci.c` always passes "nothing owed", because no interrupt is
  acknowledged yet, and X.3 supplies the mark.
- **The pass is `xhci_pass`**, called by `kusb` and nobody else. `kusb.c`
  is the doorbell loop and the latencies.
- **The report has "since read" maxima** for the gap between passes and
  for each controller's wait. They start again at every read, which is
  the brief's "since boot and since last read" for the two numbers a
  workload is judged by.
- **`xhci_visit` has no guard for a cursor past the count.** The mutants
  showed it could not matter: the cursor is taken modulo the count.

**The audit (X.2.11).** Every function a HID report or a Bluetooth input
report reaches was read for an assumption that the queue lock is held or
interrupts are off: the keyboard interpreter and its chords
(`hid_keyboard.c`), `keyboard_deliver_event` and `keyboard_emit_event`,
`tty_input_event`, `tty_input_push`, `tty_focus` and its siblings,
`console_intr_intercept_tty` (which raises SIGINT with a word-OR per
thread), `input_inject_key`, the pointer paths in `gui/input.c`, and the
Bluetooth pointer registration. None assumes either. Each takes its own
lock with interrupts saved, as it must: the PS/2 keyboard reaches the same
functions from an interrupt handler.

THE AUDIT ASKED THE WRONG QUESTION ONCE, and Codex's third round found
what it missed (#559, P1). "Does it assume a lock or interrupts off?" is
not the only thing a thread changes: a thread can be PREEMPTED, for long
enough that something it is pointing at dies. The Ctrl+C intercept read
the terminal's foreground task bare and then dereferenced and signalled
it; from an interrupt handler or inside the scheduler's pass nothing could
exit and be buried in between, and from `kusb` it can. The foreground is
now taken HELD (`console_fg_hold`: under the terminal's foreground lock,
with a lifetime hold the reaper honours) by every reader that
dereferences it: the intercept, the raw-mode query, and `/proc`'s terminal
file, which had the same exposure before this arc. The question an audit
of this kind has to ask of every pointer the delivery path reads is "who
keeps this alive while I am off the CPU?". What the audit did find is the
LONGEST of them: a terminal switch repaints the terminal synchronously
from the key (`tty_focus`, by its own comment a deliberate choice), and
that one event took 37 ms under QEMU's emulation. That is D2's "one
callback". On the P5 it is owed; if it is long there too, the repaint
moves out of the drain before audio depends on the bound.

**What QEMU measured** (`/sys/usb`, USB keyboard and mouse, 2026-11-12):

| Boot | Wait, worst | Events within 20 ms | Gap between passes after boot |
|---|---|---|---|
| default entry (logd), `qemu-xhci` | 20.1 ms | 150 of 151 | 20.5 ms |
| the same, `/sys/usb` read every 10 ms while the mouse moves | 20.2 ms | 212 of 214 | 20.3 ms |
| `/QEMU GUI Boot` (no logd) | 47.7 ms | 75 of 77 | not read |
| two controllers | 20.2 ms | 77 of 78 | not read |
| 24 CPUs, the sparse layout | 20.4 ms | 75 of 76 | not read |
| `-smp 2` | 20.2 ms | 75 of 76 | not read |
| no xHCI controller | no `kusb`, `controllers: 0` | | |

The read-every-10-ms run refused `kusb` the flag seven times; the longest
wait from a refusal to the pass that did the work was 1.6 ms, so the
holder's release is what brings it back (D4). No event was lost in any
run. The GUI entry's 47.7 ms is the terminal switch above.

**Against D11's "nothing got worse": one thing did, in QEMU.** After
boot the worst gap between passes is 20 ms where the poll's was 10 ms: now
and then a tick passes without a pass of `kusb`, and about one event in a
hundred waits a second tick. That is the price of a thread that has to be
picked where the poll ran inside the pass itself, and it is what X.3's
interrupt takes off the common path. The P5's reading decides whether it
is felt there; DEBTS has the row.

**A second regression was recorded here and was not one.** Early runs
showed a single gap of 1.0 to 1.4 s during boot, and this section said the
BSP made no scheduler pass for that long while the old poll was carried by
other cores. Both halves were wrong, and the cold read caught the second:
`processSignals` has only ever run on the BSP (`scheduler.S`), so the old
poll was never carried by another core. Instrumenting the gap showed what
it is: `kernel_init` enables the scheduler, WAITS ONE SECOND, wakes the
APs and only then sets `kProcessSignals`. `kusb` gets its first pass
before that wait, nothing rings its bell during it (112 ticks, one ring),
and the old poll did not run in it either; its gap never showed the second
only because its first poll came after.

That early pass was worse than a wrong number. On the P5 it started
Bluetooth's clockwork, which sent the adapter its reset and then went
unserved for 1.2 s of the reset's two-second answer window (`/sys/usb`
showed one event on the adapter's controller waiting 1.2 s). `kusb` now
does no work until signal processing is on, which is where the old poll
began. (The boot that showed this also failed the adapter's reset
outright, with the signature DEBTS already has for a warm reboot with
devices connected; whether the early pass contributed is not known.)

**What the P5 measured** (Chris's pass, 2026-11-12, the build of
22:40 UTC, about fifteen minutes of use; `/sys/usb`):

- Bluetooth through `kusb`: both bonds loaded at boot through `kworker`;
  both devices reconnected after being switched off and on; a forget of
  slot 1 was saved; a NEW mouse was paired into slot 1 and its bond saved
  (`mouse ready`, `bond storage: saved`). Ctrl+C from the keyboards and
  terminal switches work.
- On the adapter's controller, 3,271 drains that found events were within
  20 ms of the ring being seen empty, 29 between 20 and 200 ms, none
  beyond. The worst gap between passes is 68 ms, during boot; after the
  first read it is 20 ms, as in QEMU.
- `step_delay_max_us` is 20.8 ms: with terminal switches among the events,
  the Bluetooth step ran at most that long after a wake began. QEMU's 37
  ms for one switch was the emulator; on the P5 the bound is two ticks.
- No pass was refused the flag.
- Not this slice's, and in DEBTS: one boot's adapter reset went unanswered
  (a warm reboot with devices connected, cured by a cold boot), and one
  boot's keyboard link dropped with reason `3d` during HID setup and was
  not retried until `reconnect` was written.

## Measure first

Before any change, on the P5, with the LE keyboard and the trackball each:
switch it on and record the time to its first report, several times, and
where the time goes. The stack keeps its state under `/sys/bluetooth/`;
what is missing to tell "the controller had the event and nobody looked"
from "the scan had not yet seen the device" is added FIRST, as its own
small commit, because it is the before reading. If the time is all in the
scan cadence and none in the poll, the arc still lands (audio needs it),
and the slow connection gets a DEBTS row that says what it really is.

### What was measured (X.1)

`/sys/usb` is the instrument: per controller what is on it, the poll's
counts and gaps, and a timeline of Bluetooth's scans and LE phase changes
with the polls, events and longest gap between each line.

- **QEMU, 2026-11-12, today's tree** (`qemu-xhci`, USB keyboard and mouse,
  the `/QEMU GUI Boot` entry: GUI, no logd, `BACKSTOP=10`): 10,815 polls in
  about 110 s, and every one of the 83 polls that drained an event came
  within 20 ms of the poll before it (longest 10.3 ms; the longest gap of
  any poll, 73 ms). The DEBTS row's reports a thousand ticks apart do NOT
  reproduce: on this entry the poll comes round every tick. The default
  entry (logd) reads the same. So in QEMU the interrupt will buy about one
  tick, not seconds, and case (a) of § Proof is this reading.
- **The P5's controllers** (`/sys/bus/pci`): five xHCI, each offering MSI
  and MSI-X with one vector, none with an INTx line routed.
- **The P5's poll, 2026-11-12** (`/sys/usb`, about 20 minutes of uptime
  on the `Bosgame GUI` entry, logd running): 118,474 polls, and of the
  1,834 that drained an event, 1,833 came within 20 ms of the poll before
  and the other within 27 ms. The poll comes round every tick on the P5
  too. The AX210 sits on controller `64:00.4`, with the USB mouse; the USB
  keyboard is on `64:00.3`.
- **Where a reconnection's time goes on the P5** (the same reading's
  timeline; four reconnections of the LE keyboard and trackball after Chris
  re-paired the keyboard, each 5 to 10 s by his watch):
  - the scan round that finds the device: 2.03 s, every time;
  - connecting and encrypting with the saved key: 0.25 to 0.36 s;
  - finding the HID service again over the air: 1.4 to 3.2 s, about a
    hundred events at roughly one every 30 ms, which is the link's own
    pace and three times slower than the poll;
  - the steps that only wait for a command to complete take two polls
    each, 10 ms apiece, about 60 ms in all.
  One reconnection failed to resolve the keyboard's address, stopped, and
  went round again: 4 s lost to a second scan.

**The verdict (X.1.5).** The poll is not why the LE devices are slow to
connect. On the P5 an event waits at most one tick, and what an interrupt
would remove from a reconnection is that tick at each step: by this
reading a few tenths of a second out of five to ten. The time is in the
scan round, in the backoff before it when a round has just missed the
device, and in discovering the HID service on every reconnection although
the device is bonded. Those are Bluetooth's own and are booked in DEBTS,
not folded into this arc. The arc still lands for the reason that never
depended on this: audio needs a packet out every 15 to 20 ms and its
credits back as they return, and a 10 ms poll that can slip to 27 is not
a clock to build that on.

## Proof

- **Host**: whatever logic is new and pure (deciding how a controller is
  driven from what it offers; the daemon's "is there work" test) in a host
  harness, with mutants, as the house does.
- **A register model on the host** for the acknowledgement: the order of
  the two writes, `IMAN.IE` kept, and an event arriving between the
  acknowledgement and the re-arm.
- **QEMU, three cases, not two**: `qemu-xhci` with a USB keyboard and
  mouse on an idle GUI boot with no logd, which is the DEBTS row's own
  experiment. (a) The build BEFORE the change, measured again on today's
  tree: DEBTS records reports a thousand ticks apart in August, and that
  is a measurement of then. (b) The new build with interrupts: fires
  counted, and the time from an event's completion to its drain. (c) The
  new build with the token forcing the tick: no fires, and progress at
  the tick's cadence. The counters are what tell (b) from (c): input that
  works proves neither that interrupts are arriving nor that they are
  not. Then USB interrupts arriving while a ring-3 program runs. Then the
  ordinary boots (`make run`, `make run-sparse`) for regressions, a boot
  with several controllers and an event pending when interrupts are
  enabled, and a boot with no xHCI controller at all.
- **The P5**: the measurement above, after. The USB mouse and keyboard on
  every port combination Chris uses (DEBTS has a row about rear ports
  whose interrupt endpoints never deliver; this arc must not make it
  worse, and may change what that row says). The LE keyboard typing while
  the trackball moves. A reboot, a cold boot, and the adapter's firmware
  load.
- **The books**: the DEBTS row is struck or rewritten with what is left;
  CLAUDE.md's driver section gains the daemon and the report; DOORBELL.md
  gains its second ringer.

## What the design must prove

From Quinn's review. None of these says the code that does not exist yet
is wrong; each is a thing the design has to show before code.

- **Bounded work, and work kept.** `xhci_drain_events` drains until the
  ring is empty; `knet` has a budget of time and rounds and rings itself
  again when it spends it. The design gives USB a budget and says how the
  next controller and Bluetooth's timers are served when one controller
  is busy. When the busy flag is held by a sysfs read or a bond
  operation, the bell has been taken and the work has not been done: say
  how it is retried, and let the report show that delay.
- **From boot to running.** Enumeration and the adapter's firmware load
  stay serialized with whoever consumes events. The handler's state and a
  working wake are published BEFORE interrupts are unmasked, and events
  the boot-time polling left on a ring are accounted for.
- **Bonds, read as well as written.** Loading the bonds at startup, the
  generation check that stops a stale save acknowledging a forget, and
  the retry after failed I/O all survive. The handoff to `kworker` covers
  reading existing bonds, not only writing new ones.
- **One caller of the policy step where there are two today.**
  `bt_host_policy` is already called at the end of `bt_host_tick` (reached
  from the poll) and from `xhci_bluetooth_maintain`. Moving the poll to
  the daemon moves an existing call. Reconcile the two; do not add a
  third. `kworker`'s part is the disk and publishing what it read.
- **A backstop, if one is kept on the scheduler's path, is not a second
  consumer of the event ring without the protection the first has.**

## Not this arc

Bluetooth audio and the audio seam (BLUETOOTH_AUDIO.md); Classic
Bluetooth; USB power management and the protocol walk the DEBTS row calls
this arc's neighbour; isochronous endpoints; any change to what a HID
report means. If one of these turns out to be needed to make the
interrupt work, it is raised with Chris, not folded in.

## Decided, and what is still open

1. **MSI first** where a controller offers it, MSI-X when that is all it
   offers; one bell, and one shared vector (Chris, 2026-11-12, for the
   order and the bell; D7 for the vector).
2. **A new daemon**, `kusb`, not a second kind of work for `knet` (Chris,
   2026-11-12; Quinn leaned the same way).
3. **No call to the drain stays on the scheduler's path.** The tick
   ringing the daemon is the backstop (Chris, 2026-11-12).
4. **The moderation value is OPEN.** D9 proposes 250 µs and says how it is
   measured; Chris rules from the numbers.
5. **No INTx in this arc** (Chris, 2026-11-12, on condition that the P5's
   controllers offer MSI or MSI-X; they offer both).
