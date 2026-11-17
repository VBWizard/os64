# The frame clock

*Design, 2026-11-16, Fable. Ratified by Chris the same day (§ Rulings). The video arc (VIDEO.md § 7) asked what a player needs from the
glass; two measurements on the P5 answered, and this is what follows.
Evidence: `~/os64-opus-kit/GLASS_MEASURE_REPORT.md` and
`BEAM_PROBE_REPORT.md` (with its register lookup in `beam_src/`).*

## The problem, measured

On the P5 (2560x1440, the firmware's mode), with `/tests/glassbench`:

- **Every moving edge tears, at every window size.** The compositor copies
  finished pixels into the framebuffer at whatever moment its pass runs.
  The display reads that memory top to bottom once a refresh, and the copy
  is about eight times faster than the sweep, so it overtakes the sweep
  and the picture splits there.
- **The compositor runs on the 10 ms tick**, about 104 passes a second
  whatever is published. A 60 fps frame stays up for 10 or 20 ms by turns.
- **A program's sleep is on the same tick.** A 60 fps publisher holds its
  average and starts 59 % of its frames more than 2 ms late.

And from the beam probe: the display core has a counter of the line being
read (`OTG_STATUS_POSITION`), half a microsecond to read. The mode is
**59.92 Hz, steady to a microsecond**; a frame is 1481 lines, 1440 shown
and 41 blank (462 µs).

## The idea

**Start the copy as the blank begins, and run it top to bottom.** The copy
then leads the next sweep all the way down and can never be overtaken.
Doing that once a refresh is a frame clock, and the clock cures the other
two problems as well: every frame is shown for exactly one refresh, and a
program can wait for "the next frame" instead of sleeping on the tick.

No graphics driver is involved. os64 reads one register and never writes
the chip.

## What is built

### 1. The beam: a seam with one driver

`kernel/src/driver/display/`, os64's first display code.

- **The seam** is what the frame clock asks: is there a beam; how many
  lines a frame has; which line the blank starts at; and, at any moment,
  the line being read. Nothing else.
- **The driver** is `beam_amd_dcn`: Radeon display cores of the DCN 3.1
  family, matched by PCI device ID (the P5's is `1002:1681`). Each ID's
  register addresses are a table row citing the Linux source they were
  derived from. It maps the pages holding those registers read-only and
  uncached, so a stray write is a page fault. It samples the counter for a
  few frames at start to measure the period.
- **No beam** is an ordinary answer: QEMU, another graphics chip, a
  counter that reads all ones or stops making sense. The clock then runs
  free (below), and `/sys/gui` says which and why.

A BAR is never sized (sizing writes to it). The driver searches both of
the kernel's PCI device lists; the probe's first run missed the chip by
searching one.

### 2. The clock's arithmetic, pure

`frame_clock.c`, no kernel headers, host-tested beside a model display
with mutants, in the pattern of `xhci_service.c` and `audio_stream.c`:

- from a line reading and a time, when the next blank begins;
- the lead: how long before the blank the compositor must wake, from what
  its last passes cost;
- the verdicts: on time, late, torn;
- the order of the copy (§ 4);
- when the clock may stop (§ 5).

### 3. The compositor's frame

While the clock runs, one frame is:

| Step | When | What |
|---|---|---|
| wake | the lead before the blank | the timer of the compositor's own CPU, set for that instant |
| pass | at once | today's pass: input, damage, composite into the back buffer, under `kGuiLock` |
| aim | until the blank begins | read the beam until the line reaches the blank's first line |
| copy | from the blank's first line | back buffer to framebuffer, top to bottom (§ 4) |
| check | after the copy | read the beam once: did the sweep pass the copy? |
| tell | after the check | frame number and time, to whoever asked (§ 6) |

- **The wake is a timer, the aim is the beam.** The timer only has to be
  early; the beam decides the moment. So timer jitter costs a little
  polling and never a tear. After its pass the compositor sleeps again
  until 250 µs before the blank, so the aim is that margin, whatever the
  lead.
- **The lead adapts**: the worst of the last 64 passes (about a second of
  frames), plus half of that, between half a millisecond and eight (the
  P5, 2026-11-17: with the worst of 16 plus a quarter, every late frame was
  a pass longer than its recent neighbours).
- **Late is held once, then shown.** If the pass ends after the blank's
  first lines have gone, the copy waits for the next blank. A late frame
  costs one refresh of delay and is counted; it does not tear. A frame
  held once that misses again is FORCED: copied at its next pass wherever
  the sweep is, and counted. The property: **what the compositor has
  composed is on the glass within two of its own passes.** Without the
  bound, anything that makes every frame late froze the screen for as long
  as it lasted (the P5, 2026-11-17: a thread pinned to the compositor's
  CPU took it at every wake); with it, a starved or slow compositor tears,
  as before the clock, and `/sys/gui` says how often.
- **Torn is counted, not assumed absent.** If something held the
  compositor off mid-copy and the sweep passed it, the check sees it.
  The number says whether the copy needs protecting (§ For Chris 3).

**How the compositor wakes at an instant.** The kernel has no "call me at
time T": every wait resolves to the 10 ms tick, and the one one-shot timer
is the scheduler's preemption lease, in whole milliseconds. What is added
is small and is the compositor's alone:

- **The compositor is pinned again**, to the last CPU brought up (ruling
  5). Not the BSP, which takes every device interrupt, and with the
  roster's order never the BSP's SMT sibling. Input interrupts follow the
  pin, as the routing code already arranges. The pin alone does not make
  the CPU the compositor's; the reservation does (§ 9).
- **Before it halts, it sets its own CPU's timer** for the wake instant,
  in timer counts, and halts with the existing `sti; hlt`. Every return
  from the halt, whatever ended it, recomputes and sets it again, so a
  scheduler pass that re-arms the lease in between cannot lose the wake.
- **Damage wakes it.** `gui_damage_add` sends the compositor's CPU an
  interrupt when the compositor is halted with the clock stopped. This is
  the nudge the DEBTS row asks for, and that row is retired.

The compositor keeps its present shape: a thread that halts, not one that
parks. Making it a parked daemon like `knet` would free its CPU for other
threads between frames, and needs a sub-tick timed wake in the scheduler;
that is a later slice (§ Slices), not a precondition.

### 4. The copy's order

A full-screen copy takes about 1.8 ms and the sweep 16.2 ms, so one
rectangle copied from the blank is always safe. Several rectangles are
not, if each is copied whole in turn: a second one starting near the top
can begin after the sweep has already entered it.

So the copy runs in **horizontal strips, top to bottom, across all the
damage at once**: for each strip, the part of every rectangle that lies in
it. The copy's position then only ever moves down, and the total is never
more than one full screen. The pure file owns the order.

### 5. When the clock runs

The clock runs while there is something to show or someone waiting for a
frame, and for a few frames after. Otherwise the compositor halts with no
frame timer, as today, and damage wakes it. An idle desktop costs what it
costs now.

The mouse pointer is damage like any other, so it moves once a refresh.

### 6. A frame for ring 3

One call: **ask for the next frame.** A program asks once; when the next
frame its window is part of has been copied, its window gets an event
carrying the frame's number and time. It asks again for the one after.
This is the frame callback GRAPHICS.md names as "the more general design",
and it arrives through the event wait a GUI program already blocks in.

- A waiting program keeps the clock running.
- A covered window is told nothing until it is shown again, so an
  animation nobody can see stops by itself.
- `libos64`'s `os64_frame_wait` is rebuilt on it, so every program using
  the frame clock helper is paced by the display with no change of its
  own. yonder's ticker takes its animation frames from it.
- `/tests/glassbench` gains it as a third pacer.

### 7. Without a beam: the clock runs free

Same loop, no aim and no check: a period of one sixtieth of a second from
`micros_now()`, the copy right after the pass. QEMU and every machine
without a driver get the even cadence and the ring-3 frame. They do not
get the tear cure, and `/sys/gui` says so.

### 8. What the machine says about it

`/sys/gui` gains a `frame clock:` block: the source (which driver, or
free, and why); the refresh rate and the frame's lines; frames, late,
torn; the lead in use; the aim's time, average and worst; how many are
waiting for a frame. With `publish to glass:` beside it, a read before and
after a workload measures it. CLAUDE.md gains the paragraph.

**When the clock runs, readable at a glance** (Chris's ask): the block
says the state now (off and why, running aimed or free, stopped and why),
the compositor's CPU and whether it is reserved, and how long the clock
has run and stood stopped and how many times it started, since boot and
since the file was last read. So "was a CPU withheld, and for how much of
the time" is one `cat`.

**The stall tripwire** (added 2026-11-17, after the P5's freeze, whose
compositor kept running while no frame reached the glass). With the hold
rule, composed damage is on the glass within two of the compositor's
passes, and its loop turns at least once a lease even when stopped. So
from the BSP's pass, once a tick, the clock is watched for half a second
or more with damage owed and nothing shown, or with the loop standing
still. Each episode is logged once under `DEBUG_EXCEPTIONS`, with where
in its frame the compositor was, its thread state and whether it was
expedited, whether the frame was held, what its CPU was running, that
CPU's passes, frame wakes and lease expiries since the loop last turned,
the frame's window and the reservation; its end is logged with its
length. It logs and never acts. `/sys/gui` counts the episodes and the
longest (`frame clock stalls:`), and none is the expected number. Writing
`stall <ms>` (1 to 5000) to `/sys/gui` makes the compositor stand still
once, which is how the tripwire is proved on any machine.

**`NOFRAMECLOCK`** on the cmdline keeps today's tick-driven compositor for
that boot: the flashlight, as `NETPOLL` and `USBPOLL` are. It does not
travel to the P5 and does not need to.

### 9. The compositor's states

*Written for slice F2, before its code.*

Two writers share the compositor's CPU's LAPIC timer:
- the scheduler's dispatch tail, which arms the preemption lease
  (`sched_backstop_arm`) or stops it (`sched_backstop_disarm`) at every
  pass on a tickless AP, and for a thread sharing the compositor's CPU
  arms the lease cut short at the compositor's wake (`frame_wake_set`, the
  cut in The wake keeps the CPU, below);
- the compositor, which sets the frame wake (`frame_wake_set`) immediately
  before each `sti; hlt`.

**The invariant that lets them share it:** the compositor sets the timer
again after every return from its halt, whatever ended the halt. A pass
that re-arms the lease in between therefore cannot lose a frame wake: the
pass ended the halt, so the compositor recomputes. The frame wake fires
on the lease's own vector (0x7E), so a wake *is* an early lease expiry and
runs the same pass. The count set is never longer than the lease, so the
compositor's CPU is preempted at least as often as it is today. CPU-local
flags set with the wake tell the pass which fired, so the compositor's own
wakes (`kFrameWakeFires`) and a co-tenant's cut leases (`kFrameCutFires`)
are counted apart from `kSchedBackstopFires`; a sleep that halted ends at
one of the first two, so together they match the compositor's halts. The dispatch tail clears the flags whenever it
re-arms or stops the timer.

| State | The timer is set for | Ends when | Who moves it on, and how |
|---|---|---|---|
| **off** (`NOFRAMECLOCK`, one CPU, `SCHED=periodic`, `NOBACKSTOP`) | as today: the lease, the periodic tick, or nothing | never; it is the boot's | chosen once at `gui_start`, and `/sys/gui` says why. F1's measurement still runs |
| **stopped** (nothing to show, nobody waiting) | the lease, set by the compositor before it halts (which also cancels a frame wake still pending) | a nudge, a PS/2 interrupt (routed to this CPU), or the lease; each one ends with a re-check | the nudge comes from `gui_damage_add` (under `kGuiLock`, interrupts off) or from the input queue's push (under its lock, interrupts off, often on `kusb`'s thread on the BSP). It takes no lock: one atomic exchange of the "halted and stopped" flag, then, if the flag was set, `kSchedFrameNudged` raised and `send_ipi(IPI_MANUAL_SCHEDULE_VECTOR)`, whose pass keeps the CPU for the compositor (the claim, below). The compositor sets the flag with interrupts off just before `sti; hlt` and clears it on waking. A re-check that finds work goes to **running** |
| **running**, waiting for the lead | the earlier of the wake instant (the next blank less the lead) and the lease | the frame wake (a 0x7E pass, which keeps the CPU for the compositor: the claim, below), or any other interrupt, after which it recomputes. At or after the wake instant it goes to **composing**. A lease expiry before the wake may hand the CPU to a co-tenant, whose lease is cut at the wake | compositor thread, with interrupts off from the set to `sti; hlt`; it publishes the wake (`kSchedFrameDueUs`) first |
| **composing** | the lease, re-armed by the wake's pass | the pass ends: **composed, waiting** with a beam, **copying** without one | today's pass, under `kGuiLock` (interrupts off inside the lock) |
| **composed, waiting** | the earlier of 250 µs before the blank (`FC_AIM_MARGIN_US`) and the lease, set immediately before `sti; hlt` and recomputed on every return, as in **running** | that instant: **aiming**. Already past when the pass ends: **aiming** at once | compositor thread, interrupts off from the set to `sti; hlt`. The window stays open, so the claim keeps the CPU; damage arriving now is the next frame's |
| **aiming** | the lease | the sweep enters the blank: **copying**. If the first reading was already in the blank, the counter must change within 3 lines first, or the beam is withdrawn as frozen and **free** (a counter stopped at a blank line would otherwise look like a perfect aim every frame); only a window actually watched counts, so a gap longer than a line between readings, or a failed read, makes it inconclusive, nothing is withdrawn, and a fresh reading decides that frame. The sweep is in shown rows having left the blank: **late**. The counter does not move within the bound (250 µs plus 8 lines): withdrawn as frozen, and **free**. The counter moves but no blank arrives within the bound: **late**, counted as an overrun. No reading succeeds within the bound: the frame goes unaimed | compositor thread, interrupts on, no lock. It polls the beam |
| **copying** | the lease | the strips are done; F1's two readings judge the copy (`torn` now means held off mid-copy). Then **running**, or **stopped** if `frame_clock_keep_running` says so. The screen is asked for again before every strip: a text terminal that took it during the sleep or the aim voids what is carried, and the copy ends | compositor thread, interrupts on, outside `kGuiLock` |
| **late** (holding) | the next wake, one frame on | as **running** | nothing is copied; the back buffer keeps what was composed and the next pass adds what has arrived since. Counted by cause. A frame that was held already is not held again: it is **forced**, copied at once wherever the sweep is, judged, and counted (`frame_clock_hold_step`) |
| **free** (no beam, or withdrawn) | the earlier of the next period (1/60 s, from `micros_now()`) and the lease | as **running**, with no aim and no check: the copy follows the pass | entered at start without a beam, or from **aiming** on a frozen counter, or when a read withdraws the beam |

**Under `NOBACKSTOP` the clock is off.** With no lease nothing keeps the
AP's timer masked between the compositor's writes, and `scheduler_trigger`
would arm a stray one-shot in it (F2's cold read). The frame wake shares
the timer by the lease's discipline, so without the lease it has nothing
to share.

**The wake keeps the CPU** (added 2026-11-17, after the P5's freeze). A
frame wake and a nudge are scheduler passes, and a pass hands the CPU to
whoever has aged most. While the compositor halts it is its CPU's current
thread, not in the run queue, so it does not age; a thread that shares the
CPU (pinned there, or cache-home there with the CPU shared) ages on every
pass on every CPU, and won the pick at every wake. Every frame then missed
its blank, and with late frames held the screen froze (`env PINCPU=11
fpu_demo` on the P5, on demand). Two halves cure it (scheduler.c,
`sched_frame_claim_locked` and `sched_frame_cut_us`):
- **The claim.** The compositor publishes its thread and CPU once, and as
  it goes to sleep the instant its next frame's window opens: its wake
  (`kSchedFrameDueUs`, cleared when the clock stops). A pass on that CPU
  with the window open, or with a nudge owed (`kSchedFrameNudged`, raised
  by `gui_frame_nudge` before its interrupt), marks the compositor
  `expedite`, the doorbell's one-pick boost, at the doorbell's service
  point and under the same queue lock. Only that function sets it, only on
  the compositor's own CPU, and only while the compositor is runnable or is
  the thread the pass interrupted; pinned there, it cannot be running
  anywhere else when its window opens. Parked in a kernel wait, it is left
  alone and the pick proceeds as if there were no claim.
- **The cut.** Another thread dispatched on that CPU before the window
  opens has its lease cut at the wake (the dispatch tail calls
  `frame_wake_set`, so the count is never longer than the lease and the
  fire counts as a frame wake), and that fire's pass is where the claim
  takes the CPU back.

So the window from the wake to the next plan belongs to the compositor,
and the CPU is shared outside it. **What a co-tenant gets:** the compositor
still halts as its CPU's current thread between its copy and its next
wake, so a thread waiting for that CPU gets it only at a lease expiry that
falls inside the compositor's sleep, never at a wake. With the GUI
entries' 10 ms lease and a 16.7 ms frame, that is the end of each sleep
longer than the lease; with the default 50 ms lease, while the clock runs,
nothing. Giving the CPU away for the whole sleep is the compositor parking
between frames instead of halting (F4, DEBTS).

**The reservation.** While the clock is running (not stopped, not off),
with four CPUs or more and the dial on, the compositor's CPU runs only
threads pinned to it, and its idle thread. When the clock stops, the CPU
is everyone's again.
- **Who writes it:** the compositor thread alone, setting
  `kSchedReservedCPU` when it starts running and clearing it when it stops,
  is told by the dial to share, or loses the screen to a text terminal.
- **Who reads it:** every pass's eligibility test
  (`scheduler_thread_can_run_on_core`), without a lock.
- **Why a stale read is harmless in each direction.** Read as reserved
  just after the reservation lifted: an unpinned thread is passed over for
  one pass, and another CPU's pass takes it. Read as free just after it was
  set: one unpinned thread may be dispatched there and runs until that
  CPU's next pass, at most a lease, which is how the CPU was shared before.
- **Why the compositor can never itself be the thread shut out:** the
  reservation is set only by the compositor while it is that CPU's current
  thread, and it is pinned there.
- **The dial:** `/sys/gui` takes `reserve on` and `reserve off` lines, so
  one boot can compare both. It is on by default. With fewer than four
  CPUs the CPU is shared whatever the dial says, and `/sys/gui` says which
  applies.

## What it costs

- **Interactive latency rises a little.** Today a publish waits 6.4 ms on
  average and up to 16. With the clock it waits for the next blank: about
  10 ms on average, up to about 19, and evenly. Every system that does not
  tear pays this.
- **The aim** polls for about 250 µs a frame while the clock runs, plus
  the timer's lateness: a few percent of one CPU, during activity only.
- **The lead is time taken from the publisher's frame.** It follows the
  last second of passes, so typing and the pointer keep it short and a
  full-screen animation gets a long one: a publish that arrives within the
  lead of a blank waits for the next. The lead costs no CPU (the
  compositor sleeps from its pass to the aim); `publish to glass` beside
  `late` on `/sys/gui` shows the trade.
- **The pin alone gives the compositor nothing of its own.** The scheduler
  has one queue, and a pass on the compositor's CPU hands that CPU to
  whoever has aged most. **The claim** (§ 9, The wake keeps the CPU) gives
  it the CPU from its wake to its next plan, whoever else may run there;
  what else runs there gets the rest. **The reservation** decides who that
  may be: while the clock runs, with four CPUs or more and the dial on,
  only threads pinned there, so the machine has one CPU fewer for
  everything else for that time. With it off, the cache-home rule already
  keeps a thread that has run elsewhere off the compositor's CPU while the
  compositor is busy there (it is passed over until it has waited three
  ticks, and other CPUs take it first), so what the reservation adds is
  mostly the first dispatch of a thread that has never run, which is what
  made a spinner cache-home on the compositor's CPU in the P5's first
  freeze. `/sys/gui` says whether it is in force and for how much of the
  time.
- **`kGuiLock`'s holds are untouched.** A full-screen publisher still
  holds it with interrupts off for about 3 ms a frame. That is VIDEO.md's
  to weigh (a copy removed), not this design's.

## What is deliberately not here

- Changing the display mode or its refresh rate, variable refresh, a
  hardware cursor, page flipping: all need a real graphics driver.
- A general sub-tick timer or sleep for the kernel or ring 3.
- Any other graphics chip. The seam is where one is added; the 3900X's
  card would be the next row.

## Proof

- **Host:** `frame_clock.c` beside a model display that sweeps, with
  wakes that come early, late and mid-copy; mutants for each rule.
- **QEMU:** the free-running clock: `glassbench`'s frame pacer holds its
  cadence; `/sys/gui` reports the source as free and why; the clock stops
  when idle and damage restarts it; `NOFRAMECLOCK` behaves as today;
  `testrun` matches; `run-sparse` boots (a pin and a per-CPU timer are
  bring-up matters).
- **The P5** (Chris): `glassbench all` again. The bar must be whole at
  every size; `torn` must stay at or near zero; `publish to glass` must
  sit in one refresh; the frame pacer must start no frame late.

## Slices

| | Slice | Changes behaviour? | Review |
|---|---|---|---|
| F1 | the beam seam, the AMD driver, `frame_clock.c` and its host test; `/sys/gui` reports the beam and, at each of today's copies, where the sweep was | no: it measures | here, one cold read |
| F2 | the compositor's frame: pin, timer, lead, aim, strips, late and torn, the damage wake, `NOFRAMECLOCK` | yes | Codex: scheduler timer, pinning, an interrupt from under `kGuiLock` |
| F3 | the ring-3 frame; `os64_frame_wait` on it; glassbench's pacer; yonder's ticker | yes | Codex for the kernel half |
| F4 | later, if wanted: the compositor parks between frames | | |

F1's second half is worth having by itself: a histogram of where the sweep
was when each copy began is the tearing, shown in numbers, before and
after.

## Rulings (Chris, 2026-11-16: "I say we run with it")

1. **The approach:** read the beam, wake by timer, aim by polling, copy
   from the blank.
2. **A late frame is held one refresh, not shown torn**, on the condition
   that the compositor does not miss the blank often. The bar F2 is held
   to on the P5: with `glassbench` publishing a full screen at 60 fps on
   an otherwise idle desktop, fewer than one frame in a thousand is late.
   More than that is a finding to cure (the lead, or what held the
   compositor off), not a number to live with. **Held at most once**
   (Chris, 2026-11-17, after the P5's freeze): a frame already held that
   misses its next blank is forced onto the glass, torn if need be, and
   counted (§ 3).
3. **The copy runs with interrupts on, and `torn` is counted.** If the P5
   shows tears from the compositor being held off mid-copy, the copy runs
   with interrupts off (about 1.8 ms at full screen, on the compositor's
   own CPU). Measured first; the choice stays open until then.
4. **About 4 ms more latency on average for typing and the pointer** is
   accepted as the price of no tearing; `NOFRAMECLOCK` is there to feel
   the difference.
5. **The compositor is pinned to the last CPU brought up** (rewritten by
   Chris on 2026-11-16, replacing "CPU 2 where the BSP has an SMT sibling,
   CPU 1 otherwise"). With the roster's order (the BSP is CPU 0, siblings
   side by side) the last CPU is never the BSP's sibling on a machine with
   more than two, so no topology needs reading; and it keeps the compositor
   off the low numbers people pin their own work to (`PINCPU=2` for
   benchmarks). With two CPUs it is CPU 1; with one there is no pin and the
   clock is off.
