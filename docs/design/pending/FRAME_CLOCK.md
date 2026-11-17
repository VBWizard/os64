# The frame clock

*Design, 2026-11-16, Fable. For Chris to rule on (§ For Chris); not yet
ratified. The video arc (VIDEO.md § 7) asked what a player needs from the
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
  polling and never a tear. The aim is at most the lead's margin, a few
  hundred microseconds a frame.
- **The lead adapts**: the cost of recent passes, plus a margin, between
  half a millisecond and eight.
- **Late is held, not shown.** If the pass ends after the blank's first
  lines have gone, the copy waits for the next blank. A late frame costs
  one refresh of delay and is counted; it does not tear.
- **Torn is counted, not assumed absent.** If something held the
  compositor off mid-copy and the sweep passed it, the check sees it.
  The number says whether the copy needs protecting (§ For Chris 3).

**How the compositor wakes at an instant.** The kernel has no "call me at
time T": every wait resolves to the 10 ms tick, and the one one-shot timer
is the scheduler's preemption lease, in whole milliseconds. What is added
is small and is the compositor's alone:

- **The compositor is pinned again** (it has declined its pin since
  tickless became the default: DEBTS, "Tickless + GUI coexistence"). Not
  to the BSP, which takes every device interrupt, and not to the BSP's
  SMT sibling. Input interrupts follow the pin, as the routing code
  already arranges.
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

**`NOFRAMECLOCK`** on the cmdline keeps today's tick-driven compositor for
that boot: the flashlight, as `NETPOLL` and `USBPOLL` are. It does not
travel to the P5 and does not need to.

## What it costs

- **Interactive latency rises a little.** Today a publish waits 6.4 ms on
  average and up to 16. With the clock it waits for the next blank: about
  10 ms on average, up to about 19, and evenly. Every system that does not
  tear pays this.
- **The aim** polls for up to a few hundred microseconds a frame while the
  clock runs: a few percent of one CPU, during activity only.
- **The pin** gives the compositor a CPU of its own in effect, as its
  halting already does wherever it lands today.
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

## For Chris

1. **The approach:** read the beam, wake by timer, aim by polling, copy
   from the blank. Lean: yes.
2. **A late frame is held one refresh, not shown torn.** Lean: yes.
3. **The copy runs with interrupts on, and `torn` is counted.** If the P5
   shows tears from the compositor being held off mid-copy, the copy runs
   with interrupts off (about 1.8 ms at full screen, on the compositor's
   own CPU). Lean: measure first.
4. **About 4 ms more latency on average for typing and the pointer**, as
   the price of no tearing. Lean: accept; `NOFRAMECLOCK` is there to feel
   the difference.
5. **Which CPU the compositor is pinned to.** Lean: CPU 2 where the BSP
   has an SMT sibling, CPU 1 otherwise, as one named constant; a setting
   only if someone needs one.
