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
