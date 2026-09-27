# ARRIVAL_WAKE.md — a datagram wakes its reader when it lands, and `ping` learns to count microseconds

*Design, 2026-09-27. Asked for by Chris ("isn't there an alternative to
UDP/TCP waking up at the tick?"). Small on purpose: two protocols catch up
with a third, and one instrument gets finer graduations so the difference
can be seen.*

## The ask

`ping` prints round trips in scheduler ticks, and on QEMU every reply says
`0 ticks` or `1 tick`. The tick is 10ms. A reply from the host arrives in
well under a millisecond, so the number says nothing about the network and
nothing about the kernel either: it is a clock too coarse to show anything.
Chris asked whether the interrupt could wake the reader instead of the tick.
It already does, for TCP. This slice makes the other two protocols wake on
arrival the same way, and gives ring 3 a clock fine enough to see what the
wire actually costs — which turned out to be the larger half of the work.

## What exists

The bottom half is DOORBELL.md's: a NIC interrupt rings a bell, the bell
provokes a scheduler pass on knet's core, knet drains the card in THREAD
context, and the protocol input functions run there. From knet,
`tcp_input` wakes a parked reader on arrival — it may take the scheduler's
queue lock because it is a thread, which the tick pass it once ran inside
could not (`tcp_input_wake_and_unlock`, tcp.c). The woken thread is made
RUNNABLE, and `scheduler_change_thread_queue_locked` nudges an idle AP for
it with an IPI, so it runs within microseconds when a core is free. On a
single core, knet's own park provokes the pass that picks it. Every TCP
server on the machine is woken by the wire this way.

`udp_conn_rx` and `icmp_conn_deliver` only ENQUEUE. Their readers are woken
by the level-triggered sweeps in `processSignals` (`udp_conn_wake_if_ready`,
`icmp_conn_wake_if_ready`), which run once per scheduler PASS. DEBTS.md
books it as "UDP conns still wake their reader at the tick", and this
design was written believing that. **The measurement below says otherwise
for an idle machine:** knet parks the moment its drain is done, the park is
a scheduler entry, and that pass's sweep wakes the reader within
microseconds of the enqueue. The sweep is a tick behind only when knet does
NOT park — a loaded drain with more frames behind this one — because then
the next pass is the BSP's timer. The context map at the top of
`udp_conn.c` said the enqueue runs "inside the processSignals NIC poll";
that was true before knet existed and is the stale half of the story.

Ring 3's clock is `os64_ticks` (syscall 26): ticks since boot plus the live
rate. Nothing finer reaches ring 3. The kernel has a calibrated TSC rate
(`kCPUCyclesPerSecond`, fixed for the life of the boot since PR #105) and
already converts cycles to microseconds at the read boundary for `/proc`
and `/sys` — "raw cycles never leave the kernel; the ABI speaks TIME".

## The change

### 1. Wake on arrival, UDP and ICMP

At the tail of `udp_conn_rx` and `icmp_conn_deliver`, after the slot is
filled and the count bumped, under the conn lock: if a waiter is registered
and is ISLEEP, claim it (clear the slot) — then release the lock and call
`scheduler_wake_isleep_thread` on the captured pointer. Exactly
`tcp_input_wake_and_unlock`'s order: the conn lock and the queue lock are
never held together, and past the release only the captured thread pointer
is touched.

The sweeps STAY. They are the backstop for the one race the arrival wake
cannot cover: a reader that registered as waiter but had not yet parked
when its datagram landed (its state is RUNNING, so the claim above leaves it
registered; the sweep finds it ISLEEP a pass later and wakes it). Same
race, same cure, as pipes and TCP. The sweep is no longer the READER's
waker on the common path; it is the safety net, and its comment says so.

Who may call the wake: `scheduler_wake_isleep_thread` takes the queue lock
with interrupts off, so the caller must be a thread. `udp_conn_rx` and
`icmp_conn_deliver` have two callers: knet (a thread) and the test suite
injecting at `net_device_rx` from a test thread. Both are threads. The
9badced rule — an ISR may never take the queue lock — is untouched, because
no ISR reaches these functions: the ISR rings the bell and leaves.

### 2. A microsecond stopwatch for ring 3: `micros()`, syscall 59

`int64_t os64_micros(void)` returns microseconds since boot, read from the
TSC and converted at the boot-calibrated rate. No pointer, no struct, no
failure mode but "this kernel has no such call" (the dispatcher's negative
verdict for an unknown number, which a lifeboat kernel may still give).

**Why a new call and not a field on `os64_ticks_t`.** The struct is filled
by the kernel into the CALLER's memory, and the caller sized it at compile
time. Growing it by eight bytes makes every binary built before the growth
get eight bytes past its stack variable clobbered, silently, on every call
— the kind of failure this project builds tripwires against. The four
bytes of padding after `per_second` cannot hold a 64-bit count, and a
32-bit count wraps in 71 minutes, which is a claim ("monotonic, never
jumps") waiting to go false. A new number costs nothing and breaks nothing.

**Why a return value.** The ticks call carries two numbers that are only
useful together, so it fills a struct. Microseconds are one number, already
in the unit the reader wants. A register is the honest shape: it cannot be
handed a bad pointer, and it cannot be half-filled.

**What it counts, and what it promises.** `rdtsc` minus a boot anchor,
divided at `kCPUCyclesPerSecond` in two steps (whole seconds, then the
remainder) so the multiply cannot overflow at any uptime. The rate is
±0.33% at the default 3-second calibration window (TSCCAL=15 for ±0.07%),
which is a scale error on absolute durations and nothing else. The anchor
was read on the BSP and the read runs on the caller's core, and the house
rule (nvme.c's completion poll) is that a TSC from one core may not be
compared with one from another: QEMU and WSL2 have both handed cores
counters with different offsets. So the promise is made by CONSTRUCTION,
not assumption: every value is folded through a global high-water mark
(one compare-and-swap), a caller can only ever see a figure at least as
large as the last one anybody was given, and a core behind the anchor
reads 0 rather than wrapping. A lagging core STALLS the clock by its
offset; it cannot run it backward. That is an interval off by at most the
offset on a machine with the defect, and the boot-time sync check that
would make it exact is booked in DEBTS. (The first draft of this document
called synchronization a declared assumption; Codex's first round pointed
at the nvme.c rule, and the assumption became a mechanism.) `ping` keeps a
belt under it — a negative interval clamps to zero and says so.

**Two clocks that can disagree, stated.** `ticks` counts interrupts the
BSP received; `micros` reads a counter. Under load the tick clock LOSES
ticks (DEBTS § wall-clock hardening: ~2% under a hog, whole ticks
destroyed by IF=0 windows longer than one tick period). The two will drift
by exactly those lost ticks. That is a feature of `micros`, not a bug:
DEBTS' preferred cure (b) is "stop counting interrupts and start reading a
counter", and this is that counter's first ring-3 door. A program that
measures an interval should prefer `micros`; a program that wants the
scheduler's own count (CPU%, which divides two tick deltas) keeps `ticks`.

### 3. `ping` prints milliseconds

Round trips read `micros` before the write and after the matching reply,
and print `time=0.412 ms` — three decimals, the spelling every ping since
Muuss's 1983 original has used, so a reader who has seen one has seen this
one. The summary becomes `round-trip min/avg/max = 0.398/0.451/0.612 ms`.
The payload's stamp becomes the 64-bit microsecond count. The interval
pacing (`-i`) keeps using ticks: `os64_sleep` is tick-granular and a
sub-tick pacing clock would promise precision the sleep cannot keep.

A kernel WITHOUT syscall 59 — a lifeboat, or a `/bin` refreshed ahead of
its kernel — answers the dispatcher's negative verdict. `ping` probes once
at startup, says so on the glass, and measures in ticks scaled to
microseconds instead, so the three decimals it prints are never a lie
about a clock it does not have. (Found by booting the new `/bin` on the old
kernel by accident: every reply read `0.000 ms`, silently.)

`ping` is Chris's program (userland/apps/ping); this is the one-example
change the seam needs to be seen, and it stays as small as that.

## What this slice does NOT do

- **A UDP announce** (`udp!*!53`). The consumer that wants it is the DNS
  server, and the design for it (Plan 9's headers mode, the return address
  as an `os64_netdest_t` prefix on every read and write) is written up in
  the DNS conversation, not here. On the back burner by Chris's call.
- **virtio-net's interrupt.** Under QEMU's virtio the bell is rung by the
  tick, so on that card the arrival wake shaves the SWEEP's tick but not the
  DRAIN's. The e1000 (QEMU's default here) and the RTL8125 (the P5) ring on
  arrival, so those two see the whole gain. DEBTS row unchanged.
- **The tick clock's lost ticks.** `micros` is a door to the counter, not
  the rework of `kTicksSinceStart` DEBTS cure (b) describes.
- **Wall-clock time in microseconds.** `time` (syscall 29) is the calendar;
  this is the stopwatch. Same split as before, one instrument finer.

## Verification

- **Kernel LATE test `test_net_arrival_wake`:** a helper kernel thread naps
  two ticks, stamps the TSC, and injects one UDP frame at `net_device_rx`
  for a conn the test thread is parked on. The test thread measures from
  the helper's stamp to its own return from `udp_conn_read`. PASS below
  one tick's worth of microseconds; before this slice the wait is the
  sweep's, one tick or more. The same shape for ICMP, injected as an echo
  reply carrying the conn's identifier.
- **Ring 3, QEMU, by hand:** `ping -n 5 10.0.2.2` before and after.
  Before: `1 tick` on every line. After: sub-millisecond, three decimals.
  Screendumps in the round summary.
- **`testrun` green**, both roots; `make fsck-ext2` untouched by this slice
  but run anyway, because it is the constitution.
- **Comments re-read against the code** at every site the change makes
  false: `udp_conn.c`'s context map, `icmp_conn.c`/`.h`'s "RX context"
  lines, `signals.c`'s sweep comment, `network_stack.md`, CLAUDE.md's knet
  paragraph, DEBTS' UDP row (paid), the ping header comment.

## What the measurement said (2026-09-27, QEMU q35, 8 cores, slirp)

The kernel test's figure is the enqueue stamp to the reader running again.
The "before" row is the SAME build with the two claim-and-wake blocks
disabled, so the sweep was the only waker, exactly as before this slice.

| | e1000 (bell rung by INTx) | virtio-net (bell rung by the tick) |
|---|---|---|
| enqueue → reader, sweep only ("before") | 15–128 µs over 8 echoes | not measured |
| enqueue → reader, arrival wake ("after") | 87–150 µs over 8 echoes | not measured |
| `ping -n 5 10.0.2.2`, old kernel + old ping | 0/0/1 ticks | — |
| `ping -n 5 10.0.2.2`, this build | 0.603 / 1.079 / 2.629 ms (first echo the outlier) | 6.862 / 9.777 / 10.745 ms |

Three things follow, and the first is the one this document got wrong on
its first draft:

1. **On an idle machine the sweep was never a tick behind.** knet parks the
   instant its drain is done; the park is a scheduler entry; the sweep in
   that pass wakes the reader. Both rows are within noise of each other.
   The arrival wake earns its place in the case the rig cannot idle its
   way into: a drain with more frames queued, where knet stays on the CPU
   and the next pass is the BSP's timer — a DNS server or a stream of
   datagrams under load. That case is not measured here, and the row is
   kept honest by saying so. The code stays because it is the same shape
   TCP already needed for exactly that case, ten lines, two callers.
2. **The instrument was the missing piece.** The old `ping` could not show
   a sub-millisecond wire at all; the new one shows the e1000 at 0.6 ms
   and the P5's RTL8125 will show whatever it costs. `micros()` is the
   deliverable a person can see.
3. **virtio's 10 ms is the DRAIN, not the wake.** The tick rings its bell,
   so a reply waits for the next tick to be read off the ring, and the
   arrival wake cannot help until virtio has an interrupt (its DEBTS row).
   The number on the default rig is now an honest description of that row.

The kernel test keeps its 1000 µs bound: it cannot tell the sweep from
the arrival wake on an idle machine, but it turns red the day a reader is
woken by nothing but its backstop second, which is the failure that
matters.

## Review tier

Kernel concurrency, but a copied and proven shape with two callers, both
threads. Reviewed here and merged on Chris's test; no outside round.

## Review (PR #145)

Round 1 (Codex, on 35aaed72): three findings, all taken.

1. **P1 — `micros()` compared TSC readings across cores.** The design had
   called synchronization a declared assumption; nvme.c's completion poll
   already carried the house rule that a cross-core TSC comparison is not
   safe on QEMU/WSL2. Cure: the global high-water mark above — the clock
   can stall on a desynchronized machine, never run backward or wrap. The
   boot-time sync check stays booked in DEBTS.
2. **P2 — `ping`'s fallback message said "10 ms" regardless of the
   kernel's live tick rate.** It now derives the tick length from
   `os64_ticks_t.per_second`, the rate the ABI promises to report live.
3. **P2 — the eight measurement echoes read with a zero deadline, so one
   dropped reply would hang the post-boot suite.** Every echo read in the
   test — the two that predate this slice included — now waits two
   seconds at most; a lost reply is a lost sample, and the test fails
   cleanly if fewer than half come back.

Round 2 (on d3bb2021): one finding, taken — the sibling of round 1's P1
that the fix had missed.

4. **P2 — the test's own figure was a cross-core TSC delta.** The stamp
   is knet's, on the BSP; the woken reader is nudged onto whichever AP is
   idle, so `rdtsc() - last_arrival_tsc` compared two cores' counters —
   the very rule the syscall had just been fixed for. The test thread now
   pins itself to the BSP for the measurement, parks once so the move has
   happened before the first stamp, and restores its affinity after; a
   pinned wake on knet's own core is picked by the pass knet's park
   provokes, so the figure measures the same thing on one clock.
