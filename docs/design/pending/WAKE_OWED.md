# A wake owed (the GUI event wait's lost wakeup)

Opus, 2026-10-16. Chris's ruling the same day: the fix is a kernel slice of
its own, the design goes to Fable before any code, then the code and Codex
rounds, and it lands before yonder's perf-1 PR. This note is that design,
with Fable's review folded in: `task_wait` is part of
the slice, a debt is voided at the top of every pass, and the scheduler
counts what the mechanism does.

## What was seen

On the P5, with danlegt.com's top in view, every animated GIF on the page
paused at the same moment about once a second, while neither yonder thread
was near 100%. yonder's paint counts (YONDER_PERF.md) took four readings
to name it:

- no paint, layout or tick over ~31 ms; the main loop busy ~60%;
- the main loop's **longest single wait 249-264 ms** in every reading, at
  the top, middle and bottom of the page;
- frames shown up to **~250 ms late**;
- yonder's ticker thread **rang at most 13 ms late** against its deadline,
  and its own timed sleeps overran by **at most 19 ms**.

The ticker rang on time, and the main thread slept a quarter second
anyway. A quarter second is `GUI_EVENT_WAIT_BACKSTOP_TICKS`
(`TICKS_PER_SECOND / 4`, gui_client.c).

## The race

`gui_event_wait` (gui_client.c) is console_read's gait. Under `kGuiLock`
it pops; if the queue is empty it sets `win->waiter = self`, releases the
lock, and calls `signal_raise(SIGSLEEP, now + backstop, self)`. That call
does not park: it sets the SIGSLEEP bit and the deadline and triggers a
scheduler pass, and the pass (`runAnotherTask`) moves the outgoing thread
to ISLEEP because the bit is set.

A deliverer (`wm_deliver_event`, under `kGuiLock`) pushes the event and
calls `scheduler_wake_isleep_thread(w->waiter)`. That wake acts only on a
thread that is already ISLEEP. Between the waiter releasing `kGuiLock` and
the pass that parks it, the waiter is RUNNING (or RUNNABLE, if a tick
preempted it there): the wake is a deliberate no-op, the event waits in
the queue, and nothing looks at it again until the backstop expires.

The wake leaves a not-yet-parked thread alone on purpose, and that must
stay true: cancelling a mid-park thread's SIGSLEEP is how
`task_enqueue_dead_child` once put a parent to sleep forever (task.c, the
2026-08-09 note). Every other user of this wake therefore has a second
half that catches what the wake skips:

| Waiter | Second half | A lost wake costs |
|---|---|---|
| pipes, console, ptys, UDP, TCP, ICMP, thread joins | a level-triggered sweep in `processSignals` (claims only a parked waiter whose condition holds) | one pass (~10 ms) |
| knet (doorbell) | service point B: the bell is checked again after the outgoing thread is requeued, in the same pass (doorbell.c) | nothing: the window is closed |
| **GUI event wait, `gui_event_wait_any`** | the backstop alone | **up to 250 ms** |
| **glass view read (vncd)** | the backstop alone (`GLASS_BACKSTOP_TICKS`) | **up to 250 ms** |
| **a parent's `task_wait`** | the backstop alone (`TASK_WAIT_BACKSTOP_TICKS`) | **up to 1 s** |

yonder makes the race likely rather than rare: its ticker usually rings
microseconds after the main thread hands it a deadline, which is exactly
when the main thread is on its way into the wait. Until perf-1 removed a
loop that rang ~300 times a second, the next ring covered a lost one
within milliseconds.

## The rule this slice adds

**A wake aimed at a registered waiter is never lost: if the waiter has not
parked yet, the wake is OWED, and the pass that parks it pays it.**

This is Saltzer's wakeup-waiting switch (his 1966 thesis on Multics traffic
control): a wakeup aimed at a process that has not blocked yet sets the
switch, and blocking checks the switch and declines to block. In this
kernel it is the doorbell's service point B moved off the bell and onto the
thread, so it serves any number of waiters rather than a registry of eight
daemons. It closes the window instead of sweeping after it.

### The mechanism

One field on `thread_t`, beside `expedite` (the same kind of thing: a note
the scheduler reads under its lock):

```c
// A wake that found this thread registered but not yet parked
// (scheduler_wake_or_owe). The requeue that would park it in ISLEEP makes it
// RUNNABLE instead. Written and read only under the scheduler queue lock.
bool wakeOwed;
```

A wake verb beside the existing one (scheduler.c), naming the states it
acts on:

```c
// ISLEEP: woken now (the ISLEEP test, the SIGSLEEP cancel and the relink,
// as scheduler_wake_isleep_thread_locked). RUNNING or RUNNABLE: registered
// and not yet parked, so the wake is owed. Anything else (STOPPED, USLEEP,
// ZOMBIE, NONE) is a registration that outlived its thread's wait: nothing
// is done, and it is counted.
void scheduler_wake_or_owe_locked(thread_t *w);
void scheduler_wake_or_owe(thread_t *w);
```

The public variant takes the queue lock and **never triggers a pass**:
every caller holds an object's lock with interrupts off, and
`scheduler_trigger` enables interrupts and halt-waits for a pass. The woken
or paid thread runs at the next pass, which is the latency every wake in
this kernel already accepts. `scheduler_wake_task_waiter` keeps its
priority boost (under the lock) and its trigger (outside every lock).

The payment is in the requeue of the outgoing thread
(`scheduler_run_new_thread`), the one transition into ISLEEP, under the
queue lock it already holds:

```c
else if (sigset_has(threadToStop->signals.sigind, SIGSLEEP)) {
    if (threadToStop->wakeOwed) {
        // A wake came while this thread was on its way here: pay it now.
        threadToStop->wakeOwed = false;
        sigset_del(&threadToStop->signals.sigind, SIGSLEEP);
        threadToStop->signals.sigdata[SIGSLEEP] = 0;
        threadToStopNewQueue = THREAD_STATE_RUNNABLE;
    } else
        threadToStopNewQueue = THREAD_STATE_ISLEEP;
}
```

A thread that has exited goes ZOMBIE before this branch is reached, and a
debt dies with it.

A debt is voided at the top of every pass of the wait, under the object's
lock, where the loop voids its own registration:

```c
// No wake can be owed for this wait from here until it registers again,
// and one owed before this point announced what the pass below will find.
void scheduler_forget_owed_wake(thread_t *self);     // takes the queue lock
```

### Why the window is closed

Both halves run under the scheduler queue lock, so they are ordered:

1. The deliverer's wake runs **before** the requeue. The waiter is RUNNING
   or RUNNABLE, so the wake sets `wakeOwed`. The requeue then finds
   SIGSLEEP set and `wakeOwed` set, and makes it RUNNABLE.
2. The deliverer's wake runs **after** the requeue. The waiter is ISLEEP,
   and the wake relinks it, exactly as today.

There is no third ordering. `signal_raise(SIGSLEEP)` sets the bit without
the queue lock, but the requeue reads it on the same core in program order
after the set. A waiter preempted before the set is RUNNABLE without
SIGSLEEP: the wake owes, the requeue of a thread without SIGSLEEP does not
look at the flag (it is going RUNNABLE anyway), the waiter runs again,
raises SIGSLEEP, and the next requeue pays it. That is case 1, one pass
later.

The backstop stays. It is the house rule that a missed edge costs a tick,
never a hang, and it still covers a wake that is never aimed at all, and a
wake the verb refuses because the slot names a thread in no wait.

### Why a debt never outlives its wait

**The deliverer reads the slot and owes under the same lock the waiter
registers and voids under.** A deliverer that saw the registration
therefore finished owing before the waiter's next pass-top began, and
after the waiter's last pass-top no deliverer can see it registered.

The pass-top is the right place to void, not the wait's exits. A
legitimate debt is incurred between the registration and the requeue that
pays it, and no pass-top lies between those two points. A debt found at a
pass-top was incurred while the thread was already awake (a backstop or a
signal woke it, then a deliverer pushed): what it announced is waiting,
and the check a few lines down takes it. One site per loop also covers
every exit by construction, including those where the thread never
registered, and a debt from before the wait began.

| Wait | Deliverer, and its lock | Pass-top void |
|---|---|---|
| `gui_event_wait` | `wm_deliver_event`, under `kGuiLock` | under `kGuiLock`, before the lookup: the slot is voided if the window still names this thread, and the debt always |
| `gui_event_wait_any` | the same | under `kGuiLock`, where it already voids every slot naming it |
| `glass_view_read` | `glass_damage_locked` and `glass_view_close`, under `kGuiLock` | under `kGuiLock`, where it already voids its slot |
| `task_wait` | `task_enqueue_dead_child_locked`, which now owes **inside** `kDeadChildLock` | under `kDeadChildLock`, at the top of the pass, where the slot is voided if it names this thread; it registers in the same hold as the scan that found nothing to collect |

`task_wait`'s deliverer used to capture the waiter under `kDeadChildLock`
and wake it after the release, so a wake delayed past the parent's next
park would have been owed to the wrong wait. And `task_wait` used to
register in a second hold of the lock, after the scan's: a child that
exited between the two found nobody waiting, so no wake was aimed at all
and the parent slept out its second (Codex, #282 round 1). It registers in
the scan's own hold now, the GUI wait's shape: whatever exits after the
scan finds the registration. Owing inside the lock is
legal: the order `kDeadChildLock` → queue lock already exists (the
undertaker's sweep holds `kDeadChildLock` across `scheduler_remove_task`),
and nothing takes `kDeadChildLock` under the queue lock. What must stay
outside both is `scheduler_trigger`, whose halt-wait needs a pass that
needs the queue lock.

Lock order is otherwise unchanged: `kGuiLock` → queue lock (the
deliverer's existing wake), `kDeadChildLock` → queue lock (existing). The
requeue takes no new lock.

### The state's lifecycle

| State | Set | Cleared | Read | Lock |
|---|---|---|---|---|
| `wakeOwed` | a wake for a registered waiter that is RUNNING or RUNNABLE | the requeue that pays it; the pass-top void | the requeue to ISLEEP | the queue lock always; the void also under the object's lock |
| counts: owed, paid, voided, refused | each new debt (a wake owed to a thread that already owes one adds none: one requeue pays both), each payment, each void that found a debt, each wake the verb refused | never (since boot) | `/sys/gui` | updated under the queue lock |

`owed = paid + voided + outstanding`, where outstanding is the debts held
by threads between an owe and their next requeue or pass-top, plus the
rare debt that died with its thread (an exiting thread goes ZOMBIE without
reaching the payment). A reader can check the identity; a gap that grows
while the machine is idle is a leak.

## What does not change

- `scheduler_wake_isleep_thread`, and every caller that has a sweep, keeps
  today's behaviour. Their sweep already bounds a lost wake to a pass.
  Moving them to the owed wake would retire the sweeps, which is a
  separate decision with a separate review.
- The doorbell, which already closes its window.
- No backstop gets shorter. The paid thread gets no boost: it competes on
  aging like any relinked sleeper.

## Tests

- **The primitive, in the kernel suite** (beside the doorbell fixture):
  1. a kernel thread owes itself a wake while RUNNING (exactly the
     mid-park state), then raises SIGSLEEP with a 10 s backstop, and must
     return within a few ticks, with `paid` up by one;
  2. a thread parks first and is woken by another, the old path, which
     must still work;
  3. a thread owes itself a wake, voids it, then `nap`s 100 ms and must
     sleep the whole 100 ms, with `voided` up by one. `nap` (time.c) is
     the instrument on purpose: it is a single `signal_raise` with no
     loop, so a leaked debt cuts it short visibly, where `syscall_sleep`
     re-checks its deadline and would hide the same leak behind one extra
     pass.
- **A ring-3 probe** (`/tests/guiwake`, GUI boot): one thread waits on its
  window and another rings it at random sub-millisecond offsets,
  thousands of times. The ringer stores `micros()` where both threads can
  read it just before each ring, and the waiter records the worst
  ring-to-wake latency. Before the fix it must show ~250 ms, so the probe
  is proven to see the race. After the fix, the worst case is bounded by
  scheduling, not by the backstop.
- **yonder on danlegt** (perf branch, on the P5): the main loop's longest
  wait and the frames' lateness fall from ~250 ms to the frame cadence.
  QEMU cannot show it: emulated, yonder's loop is ~99% busy and never
  sleeps long enough to lose a quarter second.

### What they showed (QEMU, 2026-10-16)

- `/tests/guiwake`, 2000 rings, on the kernel before this slice: **FAIL,
  worst 258.6 ms, 19 rings over 50 ms**, mean 2556 µs; a second run, worst
  258.0 ms, 18 over. On this slice: **PASS, worst 23.6 ms** and **1.4 ms**,
  none over 50 ms, mean ~235 µs.
- `test_wake_owed`: paid at the park in 0 ticks, the parked wake in 3, the
  voided debt slept the whole 10.
- The fixture catches each half broken: with the payment removed, case 1
  waits out its 10 s backstop (`owed +1, paid +0`); with a void that does
  not clear, case 3's nap ends after 0 ticks.
- `testrun` before and after: the same results apart from `guiwake`.

## What it reports

`/sys/gui` gains the scheduler's four counts, one line each:
`wakes_owed`, `wakes_paid`, `wakes_voided`, `wakes_refused`. They are the scheduler's
(task_wait's payments are not the GUI's), shown in `/sys/gui` until the
scheduler has a file of its own. `paid` is the size of what this fixed; a
nonzero `refused` is a registration that outlived its thread's wait.
