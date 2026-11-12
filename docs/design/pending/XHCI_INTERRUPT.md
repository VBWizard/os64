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

## The tracker

`XHCI_TRACKER.csv` beside this file is where the arc stands: three slices
(`X.1` measure, `X.2` the daemon, `X.3` the interrupt), their sub-tasks,
and the questions of § Not decided as `Q` rows. `~/tracker.sh xhci` draws
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
   daemon; what is disk I/O stays in `kworker`, which the daemon nudges
   when there is a bond to write. The daemon never touches a disk.
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

## Not decided

1. MSI-X or MSI first, where a controller offers both, and whether the
   controllers share one vector and one bell or have their own.
2. Whether the daemon is new, or `knet` grows a second kind of work. (My
   lean: new. `knet`'s longest wake is a number people read, and USB
   bring-up is slow work that would spoil it. Quinn leans the same way,
   for the budgets and the diagnostics, and says to keep the doorbell's
   take-before-work and park-and-recheck protocol as it is.)
3. Whether any call to the drain stays on the scheduler's path as a
   backstop.
4. The moderation value.
