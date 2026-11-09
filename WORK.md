# Background jobs and GUI completions

`<os64/work.h>` provides a bounded pool of worker threads. The owner thread
submits work and collects completed results; workers ring the window's
[doorbell](GUI_DOORBELL.md) so its event loop wakes. The pool has no HTTP,
image, cache, or browser policy. A callback may fetch, decode, compute, or
perform another bounded operation.

Create a pool with `os64_work_pool_create(workers, reserve_cap, window, bit)`.
Choose 1–32 workers (four is a reasonable starting point), a byte budget, a
window owned by this task, and one application-reserved doorbell bit. Failure
returns NULL and unwinds partially created threads and handles. There are
256 job slots. Submit does not allocate or wait for a worker: it returns zero
if the table is full, the reservation exceeds the budget, the descriptor is
invalid, ids are exhausted, or publication fails. Short metadata locking and
a one-byte pipe syscall are still required; this is not a hard real-time API.

## Ownership and cancellation

Public pool calls belong on its creating thread, outside signal handlers.
The descriptor is copied at submit. A nonzero id transfers ownership of
`job`; a zero id leaves it with the caller. Keep the pool association with
the id: ids distinguish slot generations within a pool, not between pools.
Ids use a monotonically increasing 56-bit serial and an 8-bit slot index;
serial exhaustion refuses further submissions rather than reusing an id.

The worker calls `run(job, cancelled, ctx, &product)`, with product initially
NULL. Poll `cancelled(ctx)` between bounded stages and supply it to APIs such
as libfetch that accept cancellation predicates. The predicate and context
expire when `run` returns. The callback's signed return value is its verdict,
carried unchanged to the UI. An error verdict can still carry a product.

For an accepted job, ownership leaves the pool in one of two ways:

- `os64_work_reap` returns the id, verdict, input, and product in completion
  order. The caller now owns both objects and performs their cleanup.
- Cancellation or destruction invokes `release(job, product)` in the pool.
  Product may be NULL, including when `run` was skipped. Cancel means the
  caller has surrendered the result; it will not subsequently be reaped.

`release` may run on the owner or a worker. Keep it bounded, thread-safe,
and independent of GUI operations. Neither callback may re-enter the pool.
Cancel is cooperative for running work; stale ids are ignored. Queued
cancellation retains its slot until a worker consumes its notification.
Cancelling an already completed job runs its release callback on the owner.

`os64_work_pool_destroy` marks jobs cancelled, closes the job writer, joins
workers, releases remaining objects, closes the other handles, and frees the
pool. Do this before destroying its window or shared callback context.
Cleanup waits for cooperative callbacks; a callback that ignores its
cancellation predicate and blocks forever cannot be rescued by the pool.

## Memory admission

Declare `reserve` as a bound on allocations made by the callback and its
retained output, including library contexts and scratch buffers. Admission
charges that reservation before run starts and retains it through DONE.
Reaping drops the charge; cancellation drops it after release finishes.
Thus a UI that stops collecting completed products eventually stops new
admissions. Products retained after reap need a separate application budget.

The pool accounts for declarations, not allocations intercepted from malloc.
Pool metadata, thread stacks, and inputs allocated before submission are
outside this budget. Bound the size of queued inputs separately; a URL and
small context are preferable to preloading an entire download before submit.
Underdeclaring scratch or library memory defeats the intended bound.

Admission is FIFO across queued and claimed-but-unadmitted jobs. A small job
behind a large one waits even when it could fit. This prevents starvation of
large reservations. Table bookkeeping uses bounded scans of 256 slots under
`os64_lock_t`; callbacks, heap allocation, and pipe I/O run outside that lock.

## Connecting it to libui

The application owns the pool pointer and reserves a bit for its completions.
For example, with an application-local `pool` and `WORK_READY` bit:

```c
static void on_doorbell(os64_ui_t *ui, const os64_gui_event_t *event)
{
    if (!(event->doorbell.mask & WORK_READY)) return;
    if (os64_work_pool_error(pool)) {
        ui->quit = true;  // report the infrastructure error, then destroy
        return;
    }
    os64_work_id_t id;
    int64_t verdict;
    void *job, *product;
    while (os64_work_reap(pool, &id, &verdict, &job, &product)) {
        /* Update UI state here; take or release both returned objects. */
    }
}

/* After os64_ui_init, before submitting work: */
ui.on_doorbell = on_doorbell;
pool = os64_work_pool_create(4, memory_budget, window, WORK_READY);
/* Check pool != NULL; submit descriptors; enter the normal UI loop. */
/* On exit: destroy(pool), then destroy the window and callback contexts. */
```

Drain results on each bell. Several completions can coalesce, and cancellation
can ring without producing a result. The table is authoritative; reap is also
legal outside the callback. All four output pointers are required; a false
reap leaves their values untouched.

`os64_work_pool_error` reports a sticky infrastructure failure, separately
from job verdicts. A permanent private-pipe error or failed doorbell cancels
outstanding work; the owner must destroy the pool. An interrupted submit
publication refuses that job without poisoning the pool. Space notifications
interrupted before publication fall back to the 100ms predicate recheck.

## Why the pipes cannot fill from repeated notifications

An occupied queued slot backs each job notification, including a write not
yet published and a byte consumed before its worker claims the slot. With
256 slots and the kernel's 65536-byte pipes, submit's byte has reserved room.
The single owner serializes publication and rollback. Cancelling a queued
job does not free its slot early.

Space notifications count reserved writes, buffered bytes, and consumed
bytes awaiting retirement. A notifier reserves only the difference between
waiting workers and outstanding credits. A timeout consumes no credit; a
successful read retires one. An unsuccessful writer retires its unwritten
credits. This keeps credits bounded by the worker count, even if a writer or
reader pauses between steps. Shared hints may wake the wrong worker, so a
100ms read deadline provides progress for the next eligible job. Space
handles remain open until workers and their outstanding writes finish.

## Validation and shared-library constraints

Run `bash tools/test_work_host.sh` for sanitizer-backed lifecycle and
notification tests. `/tests/testrun worktest` exercises real pipes, threads,
heap accounting, and cancellation; it skips when the GUI is unavailable.
For the graphical network fixture, start `tools/httptestd.py --port 8080`
on the host and run:

```text
/tests/worktest http://10.0.2.2:8080
/tests/worktest http://10.0.2.2:8080 --cancel
```

These run twenty requests through four workers, validate response CRCs, and
paint arrivals while a frame-clock indicator moves. The second cancels one
request after body bytes arrive from `/slow.txt`. The server address shown
is QEMU slirp's host address; use the server's LAN address on hardware.

See [the library audit](docs/work-pool-evidence/audit.md) for the inspected
paths and lifetime requirements. Keep a fetch, decoder, or GIF sequence
private to one job at a time. A sealed trust store can be shared while its
owner retains it through pool destruction. Keep the environment stable while
workers use configuration or proxy settings. The pool does not make mutable
application callback contexts thread-safe.
