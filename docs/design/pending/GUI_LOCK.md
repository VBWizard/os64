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
