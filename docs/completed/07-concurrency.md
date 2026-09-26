# 07 — Concurrency: a fetch never stops the face, and a worker can wake it

*The seventh gap. It was in the first list of six-plus-one on 2026-09-24
and never became a packet, which is why nobody could find the document:
there was none. This is it, written 2026-09-25 after reading what the
substrate actually offers, because the answer to "is this a standalone
task" turned out to depend on three facts nobody had written down
together.*

## Where it stands

Half A is implemented on `codex/gui-doorbell`; its API and validation
record are in [GUI_DOORBELL.md](../../GUI_DOORBELL.md). Half B is implemented
on `codex/work-pool`; [WORK.md](../../WORK.md) describes its API. The substrate notes below describe the
starting point for this packet.

- **libfetch blocks.** `os64_fetch_open` dials, handshakes and reads the
  head before it returns; `os64_fetch_read` blocks for body bytes. That is
  the right shape for os64get and for `wend`, whose whole screen is the
  page. A GUI event loop cannot sit inside it: a face that is fetching
  forty images cannot be forty seconds deaf to its close box.
- **Threads exist and are the intended tool** (`os64/thread.h`): a thread
  shares the address space, the heap and the handles; its handle is
  waited on with `read` and detached with `close`; the program's exit
  ends its threads. sshd and telnetd already run worker threads per
  stream; desktop and controlcenter run a reaper thread beside a libui
  loop.
- **The heap is thread-safe.** `heap.c` takes a test-and-set lock with a
  short spin and a yield on every entry point (`gLock`, line ~167 — the
  file's own comment calls it `gHeapLock`, a name the code never had; the
  comment is part of the code and this packet fixes the name). So two
  threads may malloc. Nothing else in libos64 is a general lock:
  `thread.h` says os64's mutex "will be designed when a program needs
  one", and ui_session.c and the heap each carry a private spin-and-yield
  because no program had. **yonder is that program.**
- **The GUI event wait cannot be woken from inside the program.**
  `os64_gui_event_wait` sleeps until the COMPOSITOR pushes an event into
  the window's queue; there is no syscall by which a thread of the owning
  task can push one. libui's `os64_ui_run` uses that blocking wait, so a
  libui application's UI thread is unreachable by its own workers: the
  controlcenter's reaper updates a message that shows on the next
  keystroke. gterm avoids the wait entirely — it polls events on a frame
  clock and reads the pty each frame — which is why it can show output
  nobody typed for. That polling loop is the shape that works TODAY, at
  the cost of a wake per frame.
- **The libraries a worker would call carry no mutable globals**: libfetch
  and libtls have no non-const statics (checked 2026-09-25); libimage,
  libgzip, libpng, libjpeg are pure over buffers. The trust store is
  loaded per fetch unless the caller hands one in (`opt.trust`,
  caller-owned, never written by the fetch).
- **Cancellation already has its shape**: libfetch asks `cancelled(ctx)`
  before every wait and after every interrupted one; `wend` sets a flag
  from a signal handler. A worker's fetch is cancelled by a flag the UI
  thread sets, and the worker notices at its next wait.

So the packet is two halves, TWO SEPARATE CHANGES in order (Quinn's
recommendation, taken): the doorbell first, with its own probe, because it
is a shared OS capability with customers beyond the browser — a
background file search, thumbnail generation, a download manager, any
long calculation under a window — and the pool second, with its test
application, before yonder exists.

## Scope and ownership

**Half A (kernel + ABI + libui): the window doorbell.** Own
`syscall_numbers.h`, `os64/gui.h`, `gui/input.h`, `gui/event_queue.[ch]`,
the syscall table, and libui's delivery of the new event to the
application. Fable-tier for review: it is a ring boundary and a queue the
compositor writes under `kGuiLock`.

**Half B (libos64 + timed pipe read plumbing): the work pool, and it knows NOTHING about fetching.**
Own `os64/work.h` + `work.c` and `os64/lock.h` in libos64, a test
application under `/tests`, and the thread-safety audit below. General
job scheduling is the library's; every fetch-specific policy — the trust
store, the cookie doors, decoding on the worker, how many bytes an image
job reserves — is yonder's, written against the pool's interface (the
boundary Quinn named, and the one that makes the pool testable before
the browser exists). Do not change libfetch's blocking shape: a
non-blocking fetch library is a rewrite of the machinery BROWSER.md marks
Fable-tier, and threads make it unnecessary.

## Deliverable, half A: `OS64_GUI_EVENT_DOORBELL` and `SYSCALL_GUI_EVENT_RING`

**The queue already knows how to do this, for two other events.** The
window event queue (`gui/event_queue.c`) is a 64-slot ring for input,
and beside the ring it keeps two COALESCING SLOTS: an appearance
generation and a pointer snapshot, each a single pending flag plus the
latest value, independent of ring capacity. Appearance precedes queued
input; pointer snapshots follow it so old motion cannot undo a leave. That is exactly the shape a completion signal needs, because
the ring's full-queue policy is drop-newest (and focus evicts the
oldest), and a completion that can be dropped is a completion the UI may
never hear about — Quinn's finding, and the reason this is NOT "a
posted event in the ring" as the first draft said.

So: one new event type, numbered after the wheel's and static-asserted
on both sides, `OS64_GUI_EVENT_DOORBELL` / `INPUT_EVENT_DOORBELL`, whose
union member is `{ uint32_t mask; }`. One new coalescing slot in the
queue: `doorbell_mask` and `doorbell_pending`. One new syscall,
`SYSCALL_GUI_EVENT_RING(handle, mask)`: the caller must be a thread of
the task that OWNS the window (any other caller is refused — ringing
somebody else's bell is the thing this must never become); the kernel
ORs `mask` into the slot, sets pending, and wakes a parked waiter through
the same edge-triggered aim `wm_deliver_event` uses. Pop delivers one
DOORBELL event carrying the accumulated mask and clears the slot. When
input and doorbells both stay pending, they alternate after appearance
delivery so a busy worker cannot starve queued keys or close requests. So a
pending bell survives input-ring overflow and eviction: a thousand
rings between two pops are one event with their bits combined,
because the bell says "go and look" and the LOOKING is the reader's job —
DOORBELL.md's principle for the network bottom half, applied to a window
(the NIC rings, knet drains everything; a worker rings, the UI reaps
everything). The mask is the program's vocabulary — one bit per pool or
per kind of work, so several pools share one window — and the kernel
never interprets it. libui delivers the event to the application's
`on_doorbell(ui, event)` callback untouched, including without a widget
root; no widget consumes it. A zero mask is refused as BAD_ARGS; the
event's tick records the latest coalesced ring. The frame clock's
`out == NULL` wait wakes on it like any other event, which is the point.

Lineage, since it is the same idea every window system reached and then
had to fix: X11's `XSendEvent` and Win32's `PostMessage` deliver a
MESSAGE, and both learned that a message queue can fill; the durable
form is the one every serious toolkit settled on afterwards — a flag or
a mask the worker sets and the UI thread drains (Cocoa's
`performSelectorOnMainThread` coalesces the same way).

## Deliverable, half B: `os64/work.h`, a bounded pool over a pipe

**A work queue over a PIPE, and N worker threads reading it.** A pipe is
already a wakeable queue with a blocking read, a read with a timeout, and
a well-defined close — the one handle type doing the job it was designed
for in 1973 — so the pool needs no new kernel verb: the UI thread writes
ONE BYTE per job into the pipe, a worker's blocking read takes a byte and
claims the oldest queued job from the table under the lock, and closing
the pipe's write end is how the pool is told to finish (each worker reads
0 and returns). **Submit does not wait for a worker or pipe capacity**:
the metadata lock may briefly contend, but a pipe write waits for capacity
only when the pipe is full, so the job table holds at most `PIPE_CAPACITY`
(64 KiB today, one byte a job) entries and a submit past a FULL TABLE is
refused with a zero id rather than written — the caller keeps its
own list and resubmits on the next reap, which yonder does with its
image list. The pool's SHARED STATE — the job table — is guarded by the
spin-and-yield lock the heap and ui_session already carry privately,
lifted into one shared header (`os64/lock.h`: acquire, release, try —
with no sleeper list; the pool's critical sections scan at most 256
fixed slots). The API is what a general
consumer needs and no more:

The creating UI thread owns the public pool operations: submit, cancel,
reap and destroy are serialized on that thread. Workers run callbacks
and use the internal queue machinery; callbacks must not re-enter this
pool's public operations. Pipe I/O, `run` and `release` execute outside
the table lock. Creation requires `1 <= workers <= 32`, below `PIPE_CAPACITY` and
fails cleanly if its threads, tables or pipes cannot be allocated.

```c
typedef struct os64_work_pool os64_work_pool_t;
typedef uint64_t os64_work_id_t;        // slot index + GENERATION, packed: a stale id is refused, never reused

typedef struct {
    // Runs on a worker. `cancelled(ctx)` reads the job's flag with an ATOMIC
    // load — pass it straight through as libfetch's opt.cancelled. Returns
    // the job's verdict and leaves its product in *out (caller-defined).
    int64_t (*run)(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out);
    // Releases BOTH halves of a job — its input and its product (NULL when
    // run never ran or made none). EXACTLY ONE of two things happens to
    // every accepted job: the caller reaps it and owns both halves from
    // then on, or the pool calls this. Never NULL.
    void    (*release)(void *job, void *product);
    void    *job;        // the INPUT: caller-owned until submit ACCEPTS it, then the
                         // job's, released by the rule above; on a refused submit the
                         // pool touched nothing and the caller still owns it
    size_t   reserve;    // bytes this job may hold at its peak — download cap PLUS
                         // decode bound, declared by the submitter; CHARGED from
                         // admission until the job LEAVES THE POOL (reaped or released)
} os64_work_t;
// The descriptor is COPIED at submit; the caller's struct may die on return.

os64_work_pool_t *os64_work_pool_create(int32_t workers, size_t reserve_cap,
                                        int64_t window, uint32_t doorbell_bit);
os64_work_id_t os64_work_submit(os64_work_pool_t *, const os64_work_t *);  // 0 = refused; caller retains input
void   os64_work_cancel(os64_work_pool_t *, os64_work_id_t);   // validates id, sets atomic flag, wakes waiters; releases DONE jobs
// Drain: the next COMPLETED, UNCANCELLED job — its verdict, its input and its
// product — ownership of BOTH halves passing to the caller and its charge
// dropped. Called on every DOORBELL event, and legal at any time: the bell is
// a hint, the table is the record.
bool   os64_work_reap(os64_work_pool_t *, os64_work_id_t *id, int64_t *verdict, void **job, void **product);
void   os64_work_pool_destroy(os64_work_pool_t *);   // cancels all, closes the pipes, joins, releases every job not yet reaped
```

**Rules the pool enforces, each because the alternative was seen or
named in review:**

- **The bell is a hint; the table is the record.** A completed job sits
  in the table as DONE, product attached, until reaped. The worker rings
  the doorbell after marking DONE; the UI reaps EVERYTHING done on every
  bell and may reap whenever it likes. Publishing DONE ends the worker's
  access to the job and its product; the subsequent ring uses the pool's
  stable window information, since reap or cancel may already have
  released the job. Nothing about correctness depends
  on a bell arriving, only latency — and with half A's coalescing slot a
  bell cannot be lost anyway. Belt and braces, because Quinn asked what
  happens when the message is lost and the honest answer must be
  "nothing".
- **Bounded workers, bounded memory, and the bound is DECLARED — and
  CHARGED UNTIL THE BYTES LEAVE THE POOL.** Four workers are the recommended starting point (a
  page's images arrive in parallel enough; forty threads for forty images
  is forty kernel stacks for nothing). Every job declares `reserve`, its
  peak bytes; a job too big for the cap alone is refused at submit; admission waits until the charged total plus the job's fits
  under `reserve_cap`. The charge is taken at ADMISSION and dropped when
  the job LEAVES — at reap, when ownership of the product passes to the
  caller, or at the pool's own `release` — and NOT when `run` returns: a
  finished image sitting uncollected in the table is still memory, and a
  cap that forgot it would be exceeded by a burst of completions the UI
  had not got round to (Quinn, second read). The consequence is
  deliberate back-pressure: a UI that stops reaping stops admission,
  which is the right way round. What the caller holds after a reap is
  the caller's budget, outside the pool's cap — yonder's decoded-image
  cache has a cap of its own, in yonder. For an image job yonder
  declares the download cap PLUS the decode bound (libimage's dimension
  and pixel caps make that a number: a decoded image is larger than its
  file, and the reservation says so). The pool counts what it was told
  and nothing else; a job that lies about its reserve is the job's bug,
  and the audit says so. Pool metadata, worker stacks and inputs allocated
  before submission are outside this charge; callers bound queued input
  storage separately. Include library contexts and scratch in the declared
  callback budget.
- **Space notifications carry bounded credits.** A worker that cannot
  admit its job checks the predicate and records itself as waiting under
  the table lock, then unlocks and reads ONE byte from a second, shared
  pipe with `os64_read_for(..., 100)`. On return it clears its waiting
  record and re-checks admission, cancellation and stopping under the
  lock. Checking and registering in the same critical section prevents
  a release from slipping between those operations unnoticed.

  A lock-protected `space_credits` counts notifications reserved for a
  write, bytes buffered in the space pipe, and bytes read by a worker
  whose credit has not yet been retired. When a charge is dropped, a job
  is cancelled or an admission predecessor leaves the waiting order,
  the notifier reserves `max(0, waiting_workers - space_credits)` credits
  under the lock, adds them to the count, then writes that many bytes
  outside the lock. Another notifier sees those reserved credits even
  if the first writer has not reached its syscall. Thus
  `0 <= space_credits <= workers <= PIPE_CAPACITY`; pending writes have
  room already accounted for. Notifications with workers paused cannot
  accumulate another batch merely because the same workers remain
  recorded as waiting.

  A successful one-byte read retires ONE credit under the table lock.
  A timeout or interrupted read consumes no byte and retires NO credit;
  clearing a waiting record does not erase an outstanding notification.
  A failed write retires only its own unwritten credits, under the lock;
  bytes successfully written remain charged. The finite read timeout
  makes a failed notification a delayed predicate check, not a lost
  chance to proceed. Permanent pipe failure is an internal pool error,
  not a reason to spin on repeated failed reads.

  These are shared wake hints, not tokens assigned to particular
  workers. One worker may read a byte while another is the next eligible
  job; the timeout is a required part of progress for that other worker.
  **Admission is FIFO** across accepted, unadmitted jobs, including
  QUEUED jobs and claimed jobs whose worker has not run again. Claiming
  a job records that state under the table lock. A younger job cannot
  admit until its predecessors admit or are cancelled. Removing a
  predecessor from that order also notifies space waiters, even if it
  releases no bytes. This intentionally accepts head-of-line blocking
  to prevent a stream of small jobs from starving a large one.
- **Job notification credits retain their slots through handoff.**
  Submit initializes and appends a QUEUED slot under the table lock,
  reserving its notification before unlocking and writing ONE byte.
  Define `P` as reserved notifications not yet written, `B` as bytes
  buffered in the job pipe, and `R` as bytes read by workers that have
  not yet claimed a slot. The invariant is
  `P + B + R == queued_slots <= table_capacity <= PIPE_CAPACITY`.
  A write moves a credit from `P` to `B`; a read moves one from `B` to
  `R`; a claim removes one from `R` and one queued slot together under
  the table lock. These are logical handoff states, not counters that
  userland can sample atomically with kernel pipe internals.

  The pending submit's credit implies `B < PIPE_CAPACITY`, so its byte
  has room. A cancel marks a queued slot but leaves its notification and
  slot in place. The worker that consumes a byte claims the oldest slot;
  if cancelled, it releases the input outside the lock before freeing
  the slot. Slots holding running or DONE jobs remain occupied too.
  The table can therefore refuse a submit before the pipe is full;
  free table space is necessary for accepting another notification.

  The owner-thread rule serializes publication. If a one-byte write
  fails without publishing a byte, submit rolls back its appended slot
  under the lock and returns zero, leaving input ownership with the
  caller. Older notifications cannot claim this tail slot: there are
  enough older queued slots for their buffered and in-flight reads.
  A successful write accepts ownership even if a worker consumes its
  byte before submit returns. Tests must pause at these handoffs; plain
  pipe-byte count equals queued-slot count only when `P` and `R` are zero.
- **Cancellation is an atomic flag and a wait, never a kill.** There is
  no way to stop a thread from outside and there must not be: a worker
  holding the heap lock that was killed would hang every malloc. The flag
  is stored and read with `__atomic` operations at acquire/release
  order — `volatile` is not synchronisation and the first draft used it.
  **CANCEL MEANS "I DO NOT WANT IT BACK."** A cancelled job is never
  handed to the caller by reap: whether it had not started (the byte's
  worker releases it, above), was waiting for space (the waiter is woken
  and releases it), was running (it finishes at its next poll and the
  worker releases it), or was already DONE (cancel detaches both halves
  under the lock and releases them outside it), the POOL calls
  `release(job, product)` and the caller's id is dead. The caller that
  wants the product must not
  cancel. That one sentence is the whole ownership rule for the
  cancelled half.
- **Ids carry a generation.** The implementation packs a pool-wide 56-bit
  submission serial and an 8-bit slot index. It refuses at serial exhaustion;
  each reuse therefore has a different generation; an id whose generation does not match its slot is refused by
  cancel. Reap returns the completed job's id so the caller can associate
  it with its request; it does not take an id to look up. A face that
  navigated away cancels its page's jobs by the ids it holds and forgets
  them; everything of theirs is released
  by the pool under the rule above, never leaked because the UI stopped
  caring. An UNCANCELLED job is always handed back by reap or released
  by destroy — there is no third fate, and the test application counts
  `release` calls plus reaps against submits to prove it.
- **Destroy terminates because every stage does.** `destroy` sets every
  flag under the table lock, marks the pool stopping, closes the JOB
  pipe's write end, and JOINS every worker. Stopping prevents new space
  notification reservations; a writer that already reserved credits
  finishes its accounted write. Both ends of the space pipe remain open
  until the workers have joined, so a delayed notifier cannot write to
  a closed or reused handle. Space waiters notice stopping on their
  bounded reads; workers drain queued job bytes and release cancelled
  jobs before seeing job-pipe EOF. Destroy then closes the remaining
  pipe handles and releases unreaped jobs. There is no
  timeout on the join because there is no stage without a bound of its
  own: a fetch ends at its next wait (libfetch's `cancelled`, asked
  before every wait, and `idle_ms` on a silent peer), a decode ends
  within libimage's work cap, a TLS closure is libfetch's bounded one,
  and a job's `run` is REQUIRED to poll `cancelled` between any two
  stages of its own. "Bounded by `idle_ms` at worst" was the first
  draft's claim and it was not the whole truth: it named one stage.
- **Exit means exit** (thread.h): a program that returns from main with
  workers running ends them; destroy is how a program that wants a clean
  close asks for one, and yonder calls it on its close box.

**What yonder builds ON the pool, in yonder**: the trust store loaded
once at start and handed to every fetch through `opt.trust` (libfetch
never writes a caller's store — fetch.c's `trust_owned` rule); the
page's document fetch as a job like any other, so the address bar stays
alive during it; every image libpage lists as a job whose `run` fetches
through libfetch with the page's cookie doors (packet 05) and decodes
through libimage ON THE WORKER (decoding is pure and slow, and the UI
thread should receive pixels, not bytes), declaring download cap plus
decode bound as its reserve; the bell reaped on every DOORBELL event;
the decoded image handed to the layout engine's size oracle on the next
relayout. A navigation away cancels the page's jobs and forgets their
ids.

## The thread-safety audit (evidence, half B)

Every library a worker calls, listed in the packet's report with the
grep that proved it: libfetch, libtls (BearSSL's contexts are per
connection; its trust anchors are read-only), libos64's `dial` and the
DNS resolver (`resolve.c` — its cache, if it has one, is the one place a
static is likely; a lock or a per-thread answer, decided in the report),
`url.c`, `str.c`, libimage and the three codecs, libgzip. Shared mutable state found on that path needs synchronization or a
per-job owner before the pool ships. An immutable pointer table can reside
in a writable section; the audit must inspect writes and lifetimes, not
just qualifiers. The heap's comment is corrected to the lock's real name
in the same change.

## Required evidence

- Half A: a `/tests` probe that creates a window, starts a thread, rings
  from it while the main thread sits in `os64_gui_event_wait`, and
  asserts the wait returned ONE DOORBELL event; a thousand rings with
  three different bits before one pop delivering one event with those
  three bits; a ring from a second process at that handle refused; a
  ring into a queue whose 64-slot ring is FULL of input still delivered
  (the slot is beside the ring, not in it). Screendump not needed; the
  exit code is the verdict, in the harness's badge-code shape.
- Half B, fetch-free first: a `/tests/worktest` application with jobs
  that sleep, compute and allocate — twenty jobs through four workers,
  reaped in order of completion with the right products; a submit into a
  full table refused without waiting for a reader (hold workers at a
  test barrier and require submit to return before releasing them);
  a cancel of a job before it starts never starting it, and of one
  mid-run ending it at its next poll with
  both halves released by the pool; cancel after DONE releasing both
  halves and the reservation; a stale id unable to cancel a reused slot,
  and reap returning the new job's id; reservations holding admission
  at the cap, STILL held by a finished-but-unreaped job (a burst of
  completions with the UI not
  reaping stalls admission and exceeds nothing), and released at reap; a
  worker asleep for space woken by a reap on the main thread, by a
  cancel, and by destroy, each with the read backstop exercised; FIFO
  admission holding a small job behind a big one, including a pause
  between claiming the older job and its admission attempt;
  `release` calls plus reaps equal to accepted submits at the end;
  destroy with ten in flight joining all workers and releasing every
  uncollected job (the heap's
  counters before and after, byte-equal).
- Space-pipe handoffs: pause readers, issue more than `PIPE_CAPACITY`
  notifications from repeated cancellation and charge-release paths,
  and verify the notifiers finish while `space_credits <= workers`.
  Pause a notifier between reserving credits and writing; pause a
  reader between consuming a byte and retiring its credit; send more
  notifications at both points. Exercise timeout before a reserved
  write lands, timeout with no byte consumed, interrupted I/O, and
  injected write failure with unwritten credits retired. A stale wake
  byte must remain accounted for until a successful read consumes it.
  Let one worker consume shared hints while an eligible peer progresses
  through its timed re-check. Destroy with a notifier in flight must
  retain the space handles until that worker finishes and joins.
- Job-pipe handoffs: interleave a thousand submits and cancels, with
  controlled pauses before publication and after read but before claim.
  Model `P`, `B` and `R` at the handoff boundaries and verify the credit
  invariant, occupied-slot lifetime and absence of capacity waits;
  compare raw pipe bytes to queued slots only at quiescent points.
  Inject a failed submit write and verify rollback returns ownership
  without a callback or orphan byte. Exercise cancellation with a byte
  buffered and with its reader paused before claim; neither may permit
  premature slot reuse.
- Half B, with fetching, in yonder's shape but before yonder: the same
  application against `tools/httptestd.py` over slirp — twenty URLs
  through four workers, all twenty bodies CRC-checked against the
  server's (the valet dialect's CRC is right there); a cancel mid-body
  through `/slow.txt` leaving the other nineteen whole; `/sys/net/tcp` read
  after, twenty connections in the morgue, no leak in `/sys/openfiles`.
  Under a libui window with the frame clock, the window repainting each
  arrival — screendump, because that is the whole point.
- The audit report, checked in beside the packet's completion record.
- A trivial program's spawn latency before and after `os64/lock.h`
  landed in libos64: unchanged, since nothing runs at spawn.

## Booked out of this packet, by name

| What | Why | Trigger |
|---|---|---|
| A general mutex with a sleeper list, condition variables | the spin-and-yield lock covers bounded scans of 256 slots and the pipe covers every WAIT; a sleeping mutex is a scheduler feature with a design of its own | a critical section long enough that a yield-spin measurably burns a core |
| A non-blocking libfetch | threads make it unnecessary and it is a rewrite of Fable-tier machinery | never for yonder; a single-threaded consumer that cannot have threads |
| Per-host connection limits (browsers' six) | `Connection: close` and four workers bound it already; the README's keep-alive note owns the day this matters | `/sys/net/tcp` says a host is refusing parallel connections |
| Priority between jobs (the document before its images, images in view before those below) | one queue, FIFO, and the document is submitted first; a priority queue is a change to the pool's pipe, which carries no order but arrival | a page where the fold's images arrive last, measured |
| Ringing from a signal handler | the syscall is legal from a handler and the kernel slot is under `kGuiLock`, so a ring alone would work; the POOL's table lock is not re-entrant and a handler must not touch it | a consumer that must |

## Review record

**2026-09-25, Quinn's read of the first draft: taken whole, and it
changed the shape.** Two separate changes in order, the doorbell first,
because it is a shared capability with customers beyond the browser; the
pool fetch-agnostic in libos64 with yonder's policies kept in yonder. Her
four gaps: a pipe write can block the UI (one byte a job, the table
bounded by the pipe's capacity, a full table refused and never written);
a completion message can be lost (the ring's drop-newest and focus's
eviction were real, so the bell is a COALESCING SLOT beside the ring like
appearance and pointer already are, and the pool's table is the record
regardless); a cancelled job's product must still be released and ids
must carry a generation, and `volatile` is not synchronisation (the
`release` callback owned by the pool, packed generation ids, `__atomic`
flags); the bounds were under-specified (declared `reserve` per job with
the decode bound in it, admission against a pool cap, and destroy's
termination argued stage by stage instead of "idle_ms at worst"). The
lineage paragraph was rewritten too: PostMessage was the wrong ancestor
to admire, since a message queue is what fills.

**2026-09-25, Quinn's second read: four lifetime and scheduling
contracts, all taken.** A finished product still costs memory, so the
reservation is charged from admission until the job LEAVES the pool,
not until `run` returns, and a UI that stops reaping stops admission on
purpose. The job's INPUT had no owner once a cancel kept `run` from
starting, so `release` takes both halves and the rule is one sentence:
every accepted job is either reaped by the caller or released by the
pool, and cancel means "I do not want it back". The never-blocks proof
needed an invariant — bytes in the pipe equal slots in QUEUED — which a
cancel that freed a slot early would have broken one byte at a time, so
only the worker that consumes a byte may move a slot out of QUEUED. And
a worker waiting for memory had nothing to wake it, so a second pipe is
its condition variable, one byte per recorded waiter, a backstop timeout
under it, FIFO admission over it. She counted the bytes after everyone
thought the job was finished, which is the kind of counting this house
keeps.

**2026-09-25, Quinn's third read and revision: account for notifications
already in flight.** The second draft's "one byte per recorded waiter"
bound was wrong: repeated notifications could count the same sleeper
again before it ran and fill the space pipe. The shared credit count
now includes reserved writes, buffered bytes and reads awaiting credit
retirement. Timeouts do not retire unread credits, and shutdown keeps
the space handles alive through notifier completion. The job-pipe proof
also counts unpublished notifications and read-but-unclaimed bytes;
instantaneous equality between buffered bytes and queued slots was
wrong during those handoffs. The owner-thread contract makes submit
publication and rollback serial, and the required evidence now pauses
execution at the transitions those proofs depend on. The doorbell and
generic pool remain separate implementation changes; these are design
contracts and required tests, not a report of implemented validation.


## Implementation refinements

The pool fixes its table at 256 slots and permits 1–32 workers. Its bounded
scans replace the initial estimate of a few dozen instructions per lock
hold. Public calls remain serialized on the creating thread; pipe I/O and
callbacks remain outside the table lock. The serial is pool-wide and stops
before wrap. `os64_work_pool_error` reports sticky infrastructure failure
separately from a job's verdict, so private-pipe failures have an explicit
owner-visible outcome and cleanup path.

Guest integration found that the kernel's pipe reader already accepted a
deadline, but the read syscall rejected finite patience for pipe handles
and passed a zero deadline. Half B therefore also wires existing timed
pipe reads through syscall read, translates PIPE_ERR_TIMEOUT, and tests
poll, finite timeout, buffered data and EOF at the ABI boundary. No new
syscall is added. The earlier assumption that this slice was entirely
userland was incorrect.

The thread-safety audit is in [work-pool-evidence/audit.md](../work-pool-evidence/audit.md).
The public guide and ownership examples are in [WORK.md](../../WORK.md).

Validation results and reproducible commands: [work-pool evidence](../work-pool-evidence/README.md).
