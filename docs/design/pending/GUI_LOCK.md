# kGuiLock, counted (GUI_LOCK.md)

**Why.** Chris's first reading with `top`'s USR/SYS split (2026-10-17, the P5,
yonder full screen at danlegt's top) put yonder's main thread at 70% of a
core, 30% of it in the kernel. Yonder's ticker, a thread that only waits on a
pipe and rings the window when a frame is due, was at 35%, all of it in the
kernel. The ring, `gui_event_ring`, takes `kGuiLock` with interrupts off and
spins until it gets it. That lock is also held while a client's publish
copies its frame in (`gui_window_publish`) and while the compositor
composites. So the reading suggested that the cost was mostly waiting for one
lock, not working. Nothing said who waited or who held it. This counts both.

**What.** Every taker of `kGuiLock` names its site (`gui_lock(site)`, in
`gui/gui_internal.h`). The hold is charged to the site that took the lock,
recorded under the lock when it is taken, so a release names nothing. The
sites:

| site | what takes the lock |
|---|---|
| publish | a client's frame copied in |
| composite | the compositor's frame: input, the WM, compositing |
| damage | damage added from outside the lock |
| ring | a window's doorbell rung |
| wait | a client waiting for its events |
| poll | a client taking its events |
| window | a window made, unmade, asked about or changed |
| glass | `/dev/glass`: a viewer's view, read or input |
| other | the appearance and decoration stores, the census |

For each site, `/sys/gui` gives:
- acquisitions, and how many of them had to wait;
- the time spent waiting and the time spent holding, totals since boot;
- the worst wait and the worst hold since the file was last read.

The totals let a `cat` before and after a workload measure it, as
`/sys/memory`'s lock lines do. The waits are timed by
`spinlock_acquire_irqsave_measured`, which the allocator's locks already use.

| State | Begun | Changed | Read | Let go |
|---|---|---|---|---|
| A site's counts | zero at boot | at each acquire and release of that site, under the lock | by `/sys/gui` (`gui_lock_counts`) | never; the worst wait and hold restart at each read |
| Who holds the lock, and since when | at each acquire, under the lock | — | at the matching release | overwritten by the next acquire |

**The first reading (QEMU, emulated: the proportions are the point).**
Yonder at danlegt's top, 30 s between two reads:
- The compositor held the lock for 3.8 s, 13% of the time, about 400 times
  a second.
- Publish held it for 0.7 s, 2.5 ms each.
- The ticker's rings waited 1.8 s and held it for 8 ms.
- Yonder's event polls waited 2.0 s, and its waits 0.6 s.

So 4.4 s of CPU in 30 s went to spinning, with interrupts off, by calls that
hold the lock for microseconds.

**What the P5's reading decides.** If it agrees:
1. **The doorbell takes no lock.** A window's ring sets its mask atomically
   and wakes its waiter, as a NIC rings knet (DOORBELL.md).
2. **A client's event poll and wait stop queuing behind compositing.** Either
   a lock of each window's own for its queue, or the queue made lock-free.
3. **The compositor holds the lock for less.** It could take the frame's
   damage and the windows it needs under the lock, then composite outside
   it. That needs window lifetimes the lock no longer provides, which is a
   design of its own.

Each of these is kernel GUI concurrency work, so each gets a design note and
Fable's review before code.

## The compositor's passes, counted

**Why.** On the P5 the same kernel showed yonder, at the same page position,
in two states from one boot to the next (Quinn's handoff,
`YONDER_LOCK_STATE_NOTE_FOR_OPUS.md`, beside THREAD_KERNEL_TIME.md). In the
high state the compositor held the lock 60.6% of the time and a publish
waited 5.12 ms for it. In the low state the figures were 22.9% and 0.38 ms.
A publish held the lock about as long in both. The lock lines cannot say
whether the compositor made more passes or longer ones, or what a long one
was drawing.

**A hypothesis the counts can test.** The damage list holds 16 rectangles.
When it is full and a new one merges with none cheaply, the whole list
becomes one union rectangle (`gui_damage_add_locked`). A union of
rectangles from opposite sides of the screen covers most of it, so the next
pass composites far more pixels and holds the lock longer. While it does,
clients queue more damage, and the list fills again. That loop would be
bistable: a system could sit in either state until something knocks it out.

**What.** Two lines in `/sys/gui`, totals since boot, like the lock lines:
- `compositor:` its passes, how many composited anything, the rectangles and
  pixels composited, and the collapses of the damage list into one;
- `compositor holds:` its holds of the lock by length, in buckets doubling
  from 250 µs, so a run of long passes shows even when the average does not.

A hold here is the composite site's, so it includes input routing and the
window manager's work as well as compositing. A pass that composites
nothing is mostly the idle wake at the tick; damage from a window wholly
off the screen composites nothing too.

How to read it: two `cat`s a minute apart, in each state. If collapses
climb in the high state and pixels per pass rise with them, the loop is
real, and the cure is the damage list. If collapses stay near zero and
the passes are the same size, the high state is something else, such as
which core the compositor runs on, and the holds' buckets say whether it is
a few long passes or many ordinary ones.

| State | Begun | Changed | Read | Let go |
|---|---|---|---|---|
| The compositor's counts | zero at boot | per pass, per collapse, and per composite-site release, all under the lock | by `/sys/gui` (`gui_lock_counts`, with the lock lines) | never |

**The first reading (QEMU, emulated).** Two windows, idle, 27,044 passes
(the composite site's acquisitions, as they should be), 31 that drew, no
collapses, and 30 holds of 8 ms or more: under TCG a pass that draws is
slow, so only the proportions and the agreement with the lock lines mean
anything here.

## Publish to glass, counted

**Why.** A video player publishes a whole window thirty or sixty times a
second, and what it needs to know is not how long the lock was held but
how long its pixels waited to be seen. A publish adds damage and returns;
the compositor composites and flushes it on a later pass, so the wait is
the compositor's latency as a video sees it. Nothing measured it
(VIDEO.md § 7; `/tests/glassbench` is the fixture that reads it).

**What.** One line in `/sys/gui`, `publish to glass:`, in `/sys/net/knet`'s
words: the waits, their total and average in microseconds, the longest
since boot and since the file was last read, the publishes `voided`, and
the waits by length in buckets doubling from 1 ms.

- A publish with damage visible on the screen stamps itself
  (`micros_now()`) beside the damage list, under the lock it already holds.
  The pass that takes the damage takes the stamps; once its flush has put
  pixels on the framebuffer, each publish's wait is the flush's end minus
  its stamp.
- A publish that cannot be seen is `voided`, never a long wait: at the
  publish when its window is covered, minimized or off the screen, and at
  the pass that took it when that pass flushed nothing (a text terminal
  holding the screen).
- The waits are measured after the flush, outside the lock, by the
  compositor thread alone, and folded into the counts at the next pass's
  hold: no lock and no wait of their own. A read sees a pass's waits from
  the next pass on.
- More publishes between two passes than the stamp list holds
  (`GLASS_PENDING_MAX`) share
  its last stamp, the oldest of them, so a shared wait is never short.

| State | Begun | Changed | Read | Let go |
|---|---|---|---|---|
| A publish's stamp | at the publish, under the lock | — | by the pass that takes it, after its flush | when that pass settles it |
| The waits a pass measured | after its flush, by the compositor thread | — | folded at the next pass, under the lock | zeroed by the fold |
| The publish-to-glass counts | zero at boot | at the fold, and a voided publish at its publish, under the lock | by `/sys/gui` (`gui_lock_counts`) | never; the since-read longest restarts at each read |

## The frame clock, measured

**Why.** The compositor copies finished pixels to the framebuffer at
whatever moment its pass runs, and the display reads that memory top to
bottom once a refresh. When the copy overtakes the sweep the picture
splits: that is tearing, and on the P5 every moving edge does it with the
frame clock off (FRAME_CLOCK.md). The measurement is the same with the
clock on or off, so the two can be compared on one machine: off, it is the
tearing; on, a torn copy is one the compositor was held off in the middle
of.

**What.** A `frame clock:` block in `/sys/gui`:
- the source: the beam's driver and where it found the display core, or
  `none` and why (no display core this kernel can read, the counter did not
  sweep like a display, a beam withdrawn after reads that failed);
- the refresh rate, the frame's lines, the shown rows, the blank's lines and
  its length;
- copies, those that began in the blank, and those the verdict calls torn,
  since boot and since the file was last read;
- where the sweep was when each copy began, by eighths of the shown rows top
  to bottom and the blank;
- beam reads that failed.

Without a beam the block is three lines saying so.

- With the clock off, a copy is one pass's flush: the compositor reads the
  line before the first rectangle and after the last, and the pure verdict
  (`frame_clock_judge`, `gui/frame_clock.h`) judges it, its pieces the
  rectangles in the order flushed, each copied top to bottom, sharing the
  copy's time by their pixels. With the clock on, a copy is the clock's
  copy at the blank, begun at the aim's last reading, and its pieces are
  its strips. A piece is torn when one sweep reads some of its rows new and some
  old; a copy lasting a whole frame or more is torn whatever the sweeps read.
- Two reads a pass, outside the lock, where the flush already is; no lock
  and no wait of their own. The results are the compositor thread's alone
  and are folded into the counts at the next pass's hold, as the
  publish-to-glass waits are. A read sees a pass's copy from the next pass
  on.
- Rows below the shown area are never read, so they never tear.

| State | Begun | Changed | Read | Let go |
|---|---|---|---|---|
| The beam's facts | at `beam_init`, before the compositor exists | a withdrawal writes its reason, then clears `present`, on the compositor thread | by the compositor (the line) and `/sys/gui` (the facts and reason) | never |
| A pass's copy, judged | after its flush, by the compositor thread | — | folded at the next pass, under the lock | zeroed by the fold |
| The frame clock's counts | zero at boot | at the fold, under the lock | by `/sys/gui` (`gui_lock_counts`) | never; the since-read counts restart at each read |

## The frame clock, running

**Why.** With the clock on (FRAME_CLOCK.md § 9), the compositor's pace is
its own: a CPU of its own while it runs, a wake before each blank, frames
held late once rather than shown torn. Each of those can go wrong in a way that
only numbers show, and "was a CPU withheld, and for how much of the time"
must be one `cat`.

**What.** Above the beam's lines, `/sys/gui`'s `frame clock:` block gains:
- the state now: off and why, running (aimed at the beam, or free without
  one), or stopped and why;
- the compositor's CPU, and whether it is reserved (shared below four
  CPUs, when the dial says `reserve off`, or while the clock is stopped);
- the time the clock has run and stood stopped, and how many times it
  started;
- frames, the missed blanks by cause (off its CPU, the wake, the pass,
  past the blank, overran the aim), and the frames forced onto the glass
  after being held once (FRAME_CLOCK.md § 3);
- the lead in use, and the aims that ended in a copy: their number,
  average and longest polling time;
- its wakes: how many the compositor slept toward, how many of its sleeps
  halted (before a frame and after its pass), the timer's fires on its CPU
  split three ways (its own wakes; a co-tenant's lease cut at its wake;
  the lease's expiries; the first two together match the halts), and how late
  each wake arrived, in buckets doubling from 50 µs;
- the stall tripwire's episodes and the longest (FRAME_CLOCK.md § 8).

Counts and times are since boot and since the file was last read. Writing
`reserve on` or `reserve off` to `/sys/gui` sets the reservation's dial.

- The counts are the compositor thread's, kept outside the lock and
  folded at the next pass's hold, as the copies above are. A stopped clock
  makes passes at the lease, so a read is at most a lease behind.
- The times have one writer, the compositor thread, at each change of
  state, under the lock; a read adds the stretch under way under the same
  lock, which also keeps one reader's since-read baseline at a time, so a
  read never sees the times half moved.

| State | Begun | Changed | Read | Let go |
|---|---|---|---|---|
| The clock's counts | zero at boot | at the fold, under the lock | by `/sys/gui` (`gui_lock_counts`) | never; the since-read counts restart at each read |
| The clock's state and times | at `gui_start` (off or on) and the compositor's start (stopped) | at each start and stop, by the compositor thread, under the lock | by `/sys/gui` (`gui_frame_clock_status`), under the lock | never; the since-read baseline restarts at each read |
| The dial | on at boot | by a `/sys/gui` write, from any thread | by the compositor, at each frame | never |
| The reservation | none | by the compositor thread, from the state and the dial | by every scheduler pass (FRAME_CLOCK.md § 9) | when the clock stops |
