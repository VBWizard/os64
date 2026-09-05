# DOORBELL.md — the wake-from-interrupt primitive, and the network drainer that is its first customer

*Design record, Fable, 2026-09-05, written BEFORE code per the known-debt
rule. Chris marks this up; the code follows the marked-up version. Slice 2
of the P5 link work: slice 1 (PR #66) put the P5 at 1000/full and left the
download number exactly where it was, which is what this slice is for.*

## The number, and where it comes from

| Measurement | Value | Why |
|---|---|---|
| P5 download, os64get of a 178MB file, Task Manager | ~33 Mbit/s | the receive ceiling, not the wire |
| Same number at 100/full and at 1000/full | unchanged | link speed is not the limit |
| QEMU virtio + slirp download (2026-08-29) | 660 KB/s | slirp is slow; QEMU measures RELATIVE change only |

The mechanism is in `tcp.h`'s own words: the receive buffer is the advertised
window, and the NICs are drained once per scheduler pass, so the ceiling is
window per pass. Every NIC's frames are delivered from `processSignals`,
which runs on the BSP's tick, every 10ms. A sender fills our 64KB window
and waits; its ACKs leave in the next pass; the pass also wakes the reader
only at the NEXT tick. Two tick-bound turnarounds, 64KB each:

    64KB / 10ms = 6.4 MB/s = 51 Mbit/s theoretical, 33 measured.

Three things are tick-bound today and all three have to move for the number
to move:

1. Frames are taken off the ring once per tick.
2. The stack runs inside that tick pass, under `kSchedulerSwitchTasksLock`.
3. A reader blocked in `tcp_conn_read` is woken by the tick's sweep, not by
   the arrival.

## The shape, and where it came from

The split this slice builds is the one 4.2BSD named: a **top half** that
runs in the interrupt and does the least it can, and a **bottom half** that
runs later at a lower priority and does the real work. BSD's bottom half
was a software interrupt; Linux's was "bottom halves", then softirqs, then
NAPI in 2002, whose whole insight was "the interrupt says WAKE, the loop
says DRAIN until empty, and only then re-arm the interrupt". In 2021 Linux
added threaded NAPI: the bottom half is a kernel thread per NIC queue,
scheduled like any other thread. That last shape is what os64 builds, first
try, because os64 already has the kernel threads and the wake machinery
(kworker, the ISLEEP park, the level-triggered sweeps) and has none of the
softirq apparatus that Linux had to build first.

The e1000 already has half of it: its ISR raises `kE1000RxWork` and the next
tick pass drains. That flag IS a doorbell with nobody answering it until the
tick. What is missing is the primitive that answers it now.

## Part 1 — the doorbell (scheduler)

A doorbell is a thread's address and a bit:

```c
typedef struct doorbell {
    thread_t*     thread;    // the sleeper this bell belongs to
    volatile bool rung;      // set by a ringer, cleared by the sleeper
    uint64_t      rings;     // counted, for /sys and for the LATE test
} doorbell_t;
```

Two verbs, and the rule that makes them safe is WHO MAY TAKE THE QUEUE LOCK.

**`doorbell_ring(db)` — interrupt-safe, takes no lock, ever.** It stores
`rung = true` and provokes a scheduler pass on the thread's home core: a
self-IPI on `IPI_MANUAL_SCHEDULE_VECTOR` (0x7A) when that is this core, a
remote IPI otherwise. That is the whole top half. The reason it cannot take
`kSchedulerSwitchTasksLock` is the 9badced doctrine: the BSP's own pass holds
that lock with interrupts ENABLED, so an ISR on the BSP that spun on it
would deadlock the core against itself. Every wake that exists today either
holds the lock in thread context with IF off, or runs inside the pass that
already holds it. An ISR is neither, which is exactly why the wake sweeps
live in `processSignals` and why the e1000's flag waits for the tick.

The self-IPI works because of Fix 3 in SCHEDULER_REENTRANCY.md: 0x7A is in
the scheduler timer's own priority class, so the LAPIC holds it pending
until the ISR EOIs and, if the core is already mid-pass, until that pass
EOIs. It is delivered into a fresh, clean pass, never into a running one.
`send_ipi` already refuses to send a 0x7A at a core whose `mp_inScheduler`
is set, and that refusal is correct here too: a pass is running, and it
will service the bell below.

**`doorbell_service_locked()` — runs inside every core's pass, under the
queue lock, between the outgoing thread's requeue and the pick.** For each
registered bell whose `rung` is set and whose thread is in ISLEEP: cancel the
SIGSLEEP backstop, relink RUNNABLE, and mark the thread `expedite`. The exact
same three lines as `tty_summon_wake` uses to roust kworker, in the exact
same place, for the exact same reasons. The service point sits AFTER the
requeue on purpose: a sleeper that raised SIGSLEEP and was still on the CPU
when the bell rang is moved to ISLEEP by the requeue and pulled straight
back out by the service, in one pass.

**The lost-wakeup question, answered the way the pipe answered it.** The
sleeper's loop is:

    for (;;) {
        db->rung = false;            // clear BEFORE the work, never after
        do all the work there is;
        doorbell_park(db);           // sleep unless the bell rang meanwhile
    }

`doorbell_park` takes the queue lock (thread context, IF off, the type-2
idiom), and if `rung` is already true it returns without sleeping; otherwise
it raises SIGSLEEP with a one-second backstop and provokes the pass that
parks it. A ring that lands after the clear and before the lock is seen at
the lock. A ring that lands after the lock is seen by the service in the
pass that parks the thread, because the requeue runs before the service. A
ring that lands mid-pass after the service point is caught by the next tick,
at most 10ms later, which is today's latency and is also what the one-second
backstop is for. Level-triggered, re-evaluate-do-not-remember-edges, the
house pattern.

**`expedite` — one flag, one check.** Aging resets to zero on every entry to
RUNNABLE, so a freshly woken drainer ties with the thread the interrupt
displaced, and the tie goes to queue order. `scheduler_find_thread_to_run`
takes an expedited thread that can run on this core in preference to aging,
and the dispatch clears the flag. It is a one-pass boost, deliberately: at
the NEXT tick the drainer competes on aging like everyone else, so a
sustained flood cannot starve husk on the BSP. It is not a priority system,
and DEBTS will say so.

**Home core.** v1 pins the drainer to the BSP, because the BSP is where the
NIC interrupts are routed today (the e1000 routes its GSI at the BSP's APIC
id; the r8125's MSI will address the same) and where the tick already
preempts it every 10ms. A "network core" on a tickless AP, with the MSI
addressed there, is the obvious next shape and is left booked.

## Part 2 — the drainer (the first customer)

A kernel daemon minted the way kworker is, in `task_create`'s builtin name
switch. Its body:

    for (;;) {
        rung = false;
        for every registered net_device: dev->ops->drain(dev);   // until the ring is empty
        tcp_poll(); dhcp_poll();                                 // the timers ride here too
        doorbell_park(&kNetDoorbell);
    }

Three consequences, each a DEBTS row this slice retires:

- **The stack leaves the scheduler lock.** `processSignals` loses the three
  driver polls, `dhcp_poll` and `tcp_poll`, and gains one line: ring the net
  doorbell. The tick still rings it every 10ms, so virtio keeps exactly its
  polled cadence and the TCP timers keep theirs, but they run in a thread
  now, on nobody's lock but their own. The wake sweeps
  (`tcp_wake_if_ready` and its siblings) STAY in `processSignals` as the
  level-triggered backstop they always were. Chris's 8/1 ruling said this
  cleanup lands with the bottom half; this is the bottom half.
- **`drain` becomes a seam verb.** The per-driver `*_poll()` calls exist
  because the seam had no verb for "take what you have"; the comment in
  `processSignals` said the third driver would make the abstraction pay.
  The third driver shipped in August. Each driver's drain loops until its
  ring is empty, the NAPI rule; the per-pass bound the r8125 carries today
  existed only because a bound was owed to the pass.
- **Readers wake on arrival.** After enqueueing bytes, `tcp_input` captures
  the parked reader under the connection lock, releases it, and calls
  `scheduler_wake_isleep_thread` exactly as `pipe_read` does. That was
  impossible while the stack ran inside the pass that holds the queue lock;
  in thread context it is the pipe's own idiom. The sweep remains for the
  registered-but-not-yet-parked race, as for pipes. Without this, the reader
  would still wake once per tick and the number would not move.

**Interrupts, per driver, per rig:**

| NIC | Top half | Where proven |
|---|---|---|
| e1000 | INTx exists; its ISR rings the bell instead of setting `kE1000RxWork`, and the `UsesIntx`/`RxWork` pair collapses into the bell | QEMU (see step 0) |
| RTL8125 | **MSI**, new: PCI capability walk, address = BSP APIC id, data = a fixed class-4 vector, one asm stub like the e1000's; the ISR reads ISR0, writes it back, rings. No MSI capability = stays tick-driven and says so | the P5 only |
| virtio-net | none; tick-driven through the bell, out from under the lock | QEMU |

MSI over INTx for the 8125, on merit: it is what Linux uses for this chip;
it is edge-triggered and unshared, so none of the e1000's storm breakers or
stranger etiquette apply; and it needs no GSI probe, which the 8125 could
only have passed by transmitting a frame and listening for TOK. The cost is
about eighty lines of spec-shaped configuration-space code that no emulator
here can run against this chip. It carries the PHY slice's discipline:
read the capability back, print what was programmed, refuse to enable
anything that did not read back, and keep a flashlight. The flashlight is a
`NETPOLL` cmdline token that forces every NIC tick-driven; it does not ship
to the P5 by itself, and the hand-off says so.

## What this slice deliberately does NOT do (booked, discussed here first)

- **Window scaling (RFC 7323) is slice 2b, its own PR after this one.**
  With the three tick bounds gone the LAN ceiling becomes the copy path, and
  a 64KB window covers a gigabit link up to half a millisecond of round
  trip, which a switch does not approach. Scaling matters on the internet:
  64KB at 50ms is 10 Mbit/s whatever the link. It is a SYN option, a shift
  on both sides, 32-bit window arithmetic and a bigger receive buffer per
  connection, all host-testable, and a different risk class from this
  slice. The DEBTS row for it lives on the parked send-window branch and is
  re-booked on the trunk by 2b.
- **No interrupt coalescing.** The 8125 can throttle its interrupts
  (`IntrMitigate`); the drain-until-empty loop makes the first interrupt of
  a burst the only one that costs a pass, which is NAPI's answer too.
- **One MSI vector, no MSI-X.** The table-BAR machinery waits for a device
  with a reason to want more than one.
- **`expedite` is not priority.** Booked as the seed of a scheduler-policy
  slice, not built as one.
- **The drainer is one thread for all NICs on the BSP.** Per-NIC threads
  and the network-core placement wait for a machine with two NICs that
  matter.
- **The e1000 QEMU harness hangs at the INTx probe today** with two NVMe
  drives and eight cores, on master (A/B'd 2026-09-05). That rig is this
  slice's test bench for the interrupt path, so diagnosing it is step 0,
  not a debt.

## The plan, in order

0. Fix or understand the e1000 probe hang in the two-NVMe harness.
   Instrument `processSignals` with a cycle stamp under `DEBUG_NET`
   (Chris's "measure before surgery"), and record the before numbers:
   QEMU download and upload with the existing rig (`tools/os64serve.py`,
   `/tests/netsend` + `tools/tcpsink.py`), the P5 by Chris.
1. The doorbell: `doorbell.h/.c`, `expedite`, the service point in the
   pass, `doorbell_park`. A LATE test rings a bell from a synthetic
   interrupt and asserts the sleeper ran within a bounded number of ticks.
2. The drainer daemon and the `drain` seam verb; the polls and timers move;
   `processSignals` shrinks; the reader fast-path wake in `tcp_input`.
   Suite green, numbers again.
3. The e1000 ISR rings the bell. Numbers again, QEMU.
4. The r8125 MSI top half, `NETPOLL`, beacons. Numbers again, the P5.
5. Codex round: the doorbell ordering argument, the lock scopes, and the
   stack's new concurrency with syscalls on other cores are exactly the
   kernel concurrency and lifetime work an outside round is for.

## Verification

- Host: nothing pure enough to host-test except the MSI capability
  arithmetic, which gets a small harness the way the PHY did.
- QEMU: suite green on virtio and e1000 rigs; the LATE doorbell test;
  download and upload numbers before and after, relative.
- P5: Chris's download in Task Manager, the number this slice exists to
  move; `/sys/net` showing the r8125's interrupt count climbing; a long
  `os64get -a` with the overrun counter, which should now stay near zero
  because the ring is drained on arrival rather than on the tick.

## Rulings (Chris, 2026-09-05)

- **The daemon is `knet`.** ("I'm actually not that much of a name-a-holic.")
- **v1 pins knet to the BSP.** "Keep the complexity down since this will be
  a very complex change." The network-core placement stays booked.
- **MSI for the RTL8125.** The INTx port with a TOK-based probe is not
  built.
- **Window scaling is slice 2b, a separate PR** after this one.

## What shipped (2026-09-05, the same day), and what the counters said

Built in the order the plan named. The QEMU e1000 rig is the interrupt
path's test bench; the P5 is where the MSI half and the real number get
measured, by Chris.

| | Before | After |
|---|---|---|
| Boot to `boot complete`, e1000 rig | never (see step 0) | 22 s |
| Download, 4MB from the valet | 1.51 s | 0.99 s |
| Upload, 4MB to the sink | 29.81 s | 1.76 s |
| Suite | — | pre-boot 28/0, post-boot 29/0, LATE 3/0 |

**Step 0 was not a hang.** QEMU's own interrupt trace put the BSP in the
e1000 probe's settle loop with vector 0x45 never once delivered: the e1000
sits on GSI 20 behind four silent candidates, each candidate spun twenty
million `pause` instructions, and under TCG a `pause` is a trip through
QEMU's main loop. Forty seconds per candidate, four candidates. The bound
is 200,000 now, still milliseconds on bare metal, and the rig boots in 22 s.

**The upload number is the tell.** 134 KB/s before is one 1460-byte
segment per 10ms tick: the ACK arrived, waited for the pass, and only then
released the next segment. Stop-and-wait was tick-bound too, and the
drainer took it from 29.8 s to 1.8 s without touching the send window.

**`/sys/net/knet` caught a design bug the first time it existed.** The
design said "the tick rings the bell every pass" and the code did exactly
that — but `processSignals` runs in EVERY BSP pass, and a pass is also what
knet's own park provokes. So the pass that parked knet rang the bell that
woke knet that parked knet: 201,815 wakes in the first 35 seconds of an
idle link, 5,800 a second. The ring is once per TICK now
(`s_net_rung_at_tick`), and an idle link costs a few dozen wakes a second.
The lesson is the counter's: a primitive that wakes things must count its
wakes before anyone trusts it.

**The per-frame wake, measured.** A 4MB download is ~2,900 frames and cost
2,509 wakes; the upload ~2,740 segments and 4,281 wakes. That is a top half
with no coalescing doing exactly what it says: one interrupt, one wake, one
drain per arrival, with bursts folding into one wake only when they land
inside a drain. Under TCG a wake is a scheduler pass and a context switch
at emulated speed, which is why the QEMU download barely moved; on the P5 a
wake is microseconds, and the 8125's hardware interrupt mitigation
(`IntrMitigate`) is the lever if the count turns out to matter there. Both
booked, neither built.

**What the MSI half prints on the P5**, so the first boot can be read
without the source: `r8125: MSI capability at 0x.. (64-bit), address
0xfee00000 data 0x0046, control ...` under `DEBUG_BOOT`, then `r8125:
interrupts live — MSI vector 0x46 at APIC 0, IMR0 0x...` on the glass. A
missing capability or an enable that did not read back says so and keeps
the tick. `NETPOLL` on the cmdline forces the tick for every NIC — and a
cmdline token does not travel to the P5 by itself.

## The P5 (Chris, 2026-09-05 evening)

| | Before | After |
|---|---|---|
| Download, 169.8MB over the RTL8125 at 1000/full, Windows Task Manager | ~33 Mbit/s | **~180 Mbit/s averaged** |

Above the tick's theoretical 51 Mbit/s by a wide margin, so the MSI took on
the first silicon it ever met. Not the wire yet: the per-frame-wake row in
DEBTS and the 8125's own interrupt mitigation are the next levers, and
`/sys/net/knet` beside `/sys/net/r81250` after a big `os64get` is the
measurement that decides between them. Chris also saw an 8–10 second pause
between the last byte and `os64get` returning; what the program does after
the last byte (its CRC pass over the whole file, the publish rename, the
close) is where to look, and a rate readout in `os64get`/`cp` is his to add.

The `NETPOLL` flashlight was proven on the QEMU e1000 rig the same evening:
no probe, `mode: tick-driven (NETPOLL)`, the download back at its tick-bound
1.44 s, suite green — the path that carried every packet before the
doorbell, one token away.
