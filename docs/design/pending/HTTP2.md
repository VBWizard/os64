# HTTP2.md — one connection per origin, many requests at once

*Written by Fable on 2026-11-09 at Chris's ask, after the morning's
question "what would HTTP/2 buy us?" and the ruling that followed: do it,
as a stack of ordinary PRs on userland, with a tracker that says where the
whole thing stands. This file is the design and the briefs. The TRACKER is
`HTTP2_TRACKER.csv` beside it, and § 0 says how it is kept. The feature is
libfetch's, below the fetch object, invisible to its callers — yonder,
os64get and wend get it without a line changed. It stays INERT until its
last slice offers `h2` in ALPN (§ 2.1), which is what lets every earlier
slice merge as it clears review: nothing before that is reachable from a
page. HTTP/3 is not in this arc, and § 1 says why.*

Read first, in this order: WIRE.md § 2 and its "as built" (the connection
pool this arc grows out of; the numbers in `pool.h`), LIBFETCH.md (the
fetch object's contract), TLS13.md (the engine the resumption slice
extends; its § The profile is the ClientHello that slice changes), and
`abi/include/os64/syscall_numbers.h`'s "read's patience" block, which is
the whole of what ring 3 can wait on today and the reason § 2.4 exists.

## 0. The tracker — how `HTTP2_TRACKER.csv` is kept

Chris, 2026-11-09: "Keeping track of where we are and the whole picture
gets hard when we're in the middle of it. And we get sidetracked because
we see a kitten, and then lose track of something." So the arc has one
file that answers "where are we?", kept by the same hands that do the
work, and kept by rule rather than by memory. Anyone working an HTTP/2
slice — Opus, Fable, Quinn, Codex — follows this section; nobody working
another arc touches the file.

**The file.** `docs/design/pending/HTTP2_TRACKER.csv`, one row per task,
six columns: `id`, `task`, `depends_on`, `status`, `done_in`, `notes`.
Fields are quoted when they hold a comma. `tools/http2_tracker.py` reads
it: with no arguments it checks the file (ids unique, every status in its
vocabulary, every `depends_on` an id that exists, every `done` row with a
`done_in`) and prints the board; `--html <path>` writes the page Chris
asked for, grouped by slice with a progress bar each. The page is
GENERATED and never edited: the CSV is the truth, the page is the view.

**Three kinds of row, told apart by their id.**

- **A slice row** (`H2.1`) has a status that walks the lifecycle a slice
  goes through in this house, in order: `todo` (named, no brief yet),
  `design` (the brief is being written), `design-review` (Fable is reading
  the brief), `code` (being built, tests included, the three passes),
  `review` (Codex's round), `p5` (Chris is testing it), `done` (merged to
  userland), `dropped`. One row says where the slice is in the pipeline,
  which is the thing that gets lost.
- **A sub-task row** (`H2.1.3`) is a piece of work inside a slice. Its
  statuses are `todo`, `doing`, `done`, `dropped`. Sub-tasks never go
  through review on their own, so they do not get the long vocabulary.
  Captures (`C1`) are sub-tasks of the slice that needs them.
- **A question row** (`Q1`) is a ruling the arc needs from Chris: `open`
  or `ruled`, with the ruling written into `notes` when it comes. A
  question asked in passing gets a row the same day, so it has a place
  until it is answered.

**The rules.**

1. **A row is added in the PR that defines it.** A slice's sub-task rows
   are added by the PR that writes the slice's brief, never before; a
   question row by whoever asks the question, in whatever PR they are in.
   Nothing is ever deleted: a task we decide not to do becomes `dropped`,
   with the reason in `notes`, so what was decided stays visible.
2. **A row changes only in the PR that does the work, and a branch touches
   only its own rows.** A slice's branch flips that slice's rows and no
   other's. The `done` flip and the PR number in `done_in` land in the
   same PR as the code, as its last commit: merging IS the crossing-off,
   and there is no separate bookkeeping commit to forget.
3. **Start from the copy beneath you.** Before a branch edits the file it
   rebases onto the copy it is stacked on: userland as Chris last pushed
   it when the branch is first in its stack, the branch below otherwise
   (which already carries the flips of everything under it, so a stack
   merged in order is a no-op on the rows above). THE OWNER OF THE
   USERLAND COPY — Chris — PUSHES EVERY TIME IT CHANGES; he signed up for
   that on 2026-11-09, and it is the one step no agent can do for him.
4. **Slices are separated by a blank line in the file** and a row is one
   line, so two branches on two slices never edit the same hunk and never
   conflict at merge.
5. **The builder owns the slice row's status.** It moves to `code` when
   the builder starts, `review` when the Codex round opens, `p5` when the
   round is clean and the branch is handed to Chris, `done` in the PR's
   last commit. Chris may move a row himself, from userland, and push.
6. **`tools/http2_tracker.py` passes before every push that touches the
   file.** A check that fails names the row.

## 1. Why, and why now

**What it cost when this arc began** (H2.1 and H2.2 have since paid the
ticket and the cap; their as-built sections say how; the rest of this
paragraph is the state they started from). libfetch speaks HTTP/1.1 with
keep-alive and a pool of kept connections (WIRE.md § 2): six idle per key, sixteen in all, sixty
seconds idle. Concurrency is not capped by the pool — a host is dialled by
as many fetches as are asking at once, which is the caller's thread count
(yonder: its twelve workers and a synchronous XHR), and a page that asks
for forty resources dials more than that over its life, because only six
idle connections per key are kept between requests (DEBTS § libfetch,
"Fetches to one host are not capped at once"; WIRE.md § 2's table counts
21 connections for a page of 26 resources). Each one pays a TCP handshake and a
WHOLE TLS 1.3 handshake, because session tickets were consumed and
discarded when this arc began (H2.1 keeps them: TLS13.md § Resumption). On
a kept connection a request waits for the one before it to finish; the
pool's measurement showed connections falling to a quarter and arrival
time barely moving, because a page's burst of same-host requests dials
anyway (WIRE.md § 2, "The measurement").

**What HTTP/2 buys, honestly.**

- **One connection per origin, many streams on it.** Every request after
  the first on an origin starts one round trip later instead of three, and
  a page's burst costs one handshake, not a dozen. Real, and modest on a
  wired LAN with a millisecond to the gateway; largest on sites that ship
  hundreds of small unbundled files.
- **The fingerprint, which is bigger than "a check or two".** Chrome never
  speaks HTTP/1.1 to a server that offers h2, so a client sending Chrome's
  User-Agent over HTTP/1.1 is a mismatch a bot-management service scores
  (WIRE.md § 1 named this and declined it as "the honest outcome"; this
  arc pays it). The catch is that h2 is fingerprinted too — SETTINGS order
  and values, the first WINDOW_UPDATE's increment, PRIORITY frames,
  pseudo-header order — so speaking it means speaking it the way the
  preset's browser does, or one tell is traded for another (§ 2.9).
- **Browser-like politeness.** Forty parallel connections from one address
  is a burst no browser makes; h2 fixes that structurally, and a
  six-per-host cap (§ 4, H2.2) fixes it for h1 sites on the way.
- **Not advantages any more:** server push is dead in every browser, and
  h2's priority tree is ignored by most servers in favour of the
  `Priority` header the presets already send (WIRE.md § 1 as built).

**Why not HTTP/3 in this arc.** QUIC is a whole transport in ring 3:
congestion control, loss recovery, pacing, per-packet AEAD with header
protection the engine does not expose, QPACK, and a fall back to TCP when
UDP is blocked. Its wins over h2 are on lossy links — head-of-line blocking
across streams, a faster cold start — which the P5's wired RTL8125 does not
have. It becomes interesting after the Wi-Fi plan, and it needs h1 or h2
first anyway, since h3 is discovered through Alt-Svc. One DEBTS row when
this arc lands; no design now.

**The ranking the slices follow (§ 3)**: session resumption first, because
it is the cheapest cut of the same handshake cost and stands alone (so it
looked: H2.1's "as built" has the P5's measurement, which found the
handshake's cost to be its round trip, and resumption to save none of it); the
pool's new ownership and the cap second, because h2 rides on it and h1
sites gain from it today; then the pure pieces; then the one hard thing,
the multiplexer; then the switch.

## 2. The contracts — decided once, before any slice is coded

### 2.1 Where the protocol is chosen: ALPN, and nothing else

A connection speaks h2 when, and only when, the TLS 1.3 handshake's ALPN
answer is `h2`. libfetch offers `h2, http/1.1` in that order on a direct
https dial (today `dial` in fetch.c offers `http/1.1` alone); the server's
pick is the connection's protocol for its whole life, recorded on the
connection object, never re-negotiated. Everything else is h1, unchanged:

- A plain `http://` hop (no `h2c`, no `Upgrade: h2c`, no prior knowledge:
  no browser speaks them, and a preset that did would be a tell).
- A hop through a proxy: libfetch sends https through a plain proxy in
  absolute form (WIRE.md § 2 as built), so a proxied connection is plain
  and h1.
- The TLS 1.2 fallback: the Bear client (TLS13.md § Version selection)
  offers `http/1.1` alone. RFC 9113 § 9.2 puts conditions on h2 over 1.2
  that are not worth meeting for a path that exists to reach legacy
  servers.
- A preset that says so. A costume whose browser never spoke h2 (Netscape
  4, Lynx) offers `http/1.1` alone, because a preset is a whole identity
  (WIRE.md § 1) and a Lynx that negotiates h2 is not Lynx. The browser
  presets and yonder's own offer both. (Ruled here: web-identity policy,
  the kind this seat decides and tells; it is in the tracker as a note on
  H2.5, not as a question.)

The pool key does not change: a kept connection's protocol is a fact
about the connection, not about who may share it, and a key's connections
may be a mix during the moment ALPN answers differ (two dials in flight to
one host, one answering h2 and the other h1 — rare, legal, handled by
§ 2.2's "which connection a fetch takes").

### 2.2 Ownership: a connection is a shared object with a count

Today a connection belongs to exactly one fetch between its take and its
put, and the pool is a table of idle `fetch_transport_t`s (pool.h). Under
h2 many fetches use one connection at once, so the unit changes:

- **`fetch_conn_t`** (name is the builder's) is a heap object: the
  transport, the key, the protocol, a reference count, the time it was
  last left idle, and — for h2 — the connection state of § 2.5. The pool
  becomes a table of pointers to these, idle AND live; `fetch_pool_take`
  increments the count, `fetch_pool_put` decrements it, and a connection
  with a count of zero is idle: kept with the idle timer, evicted by the
  caps, exactly as today. For h1 the count is 0 or 1 and nothing a fetch
  can observe changes. That is why the ownership change ships BEFORE any
  h2 code (H2.2): an h1-only PR that is behaviour-neutral, reviewed on its
  own.
- **Which connection a fetch takes, for its key, in this order:** a live
  h2 connection with room (its streams below the peer's
  `SETTINGS_MAX_CONCURRENT_STREAMS`); else an idle connection, most
  recently kept first, probed as today (an idle h2 connection needs no
  probe — its reader thread has watched it the whole time, § 2.3); else,
  if the key is REMEMBERED as h2 (the last ALPN answer for it in this
  process) and a dial to it is in flight, wait for that dial's verdict
  rather than dialling another; else, if the key's connections (live,
  idle and dialling together) are below `FETCH_POOL_PER_KEY`, dial; else
  wait. **A dial needs a slot in the table, and an idle connection never
  makes a fetch wait for one:** when the table is full, the least recently
  kept idle connection of any key is closed for the slot, and when the key
  is at its own cap holding an idle connection this fetch may not take (a
  request sent once passes over one kept longer than
  `FETCH_POOL_ONCE_MS`), that one is closed for it. A fetch waits only
  when every connection that stands in its way is live or dialling. The
  remembered-h2 clause is what keeps a page's burst to a known
  h2 host from dialling six connections that will all answer h2; the
  first visit to a host still bursts up to the cap, as Chrome's does
  without a stored property. (Alt-Svc and HTTPS DNS records, which would
  tell us in advance, are not spoken; a row.)
- **The cap is a queue in front of the dial.** A fetch that must wait
  parks on its bell (§ 2.4) in the pool's queue. **A ring is a hint and
  never a grant.** When a connection of a key is put back, dies, finishes
  its handshake or gains room, EVERY waiter of that key is rung, and when
  a table slot comes free every waiter is; each one woken re-asks the
  whole question above under the pool's lock, since the world moved, and
  parks again if it lost. Ringing one waiter would hand the news to a
  fetch that may be on its way out (cancelled, or out of patience) and
  strand the rest until their own patience ran out; ringing all costs a
  wake apiece for at most a caller's thread count of waiters. No order is
  promised among them. The wait is under the fetch's idle deadline and
  its cancellation predicate like every wait in libfetch. (As built, the
  wait has a bound of its own and not the idle deadline: "H2.2, as
  built" says why.)
- **Excess h2 connections to one key** (the burst case above) are not
  spread across: a new stream goes to the h2 connection that has been
  live longest and has room, so the duplicates carry what was already
  sent on them and nothing more, then idle out. Spreading streams over
  them would keep several connections and their reader threads working
  where one will do, and several busy h2 connections from one address to
  one origin is a shape no browser makes. Chrome closes duplicates; ours
  go quiet and let the timer close them.
- **A connection's death is fanned out.** When an h2 connection's reader
  sees the transport fail, or a GOAWAY, every stream on it ends as § 2.5
  says, every waiter of the key is rung, and the object is freed
  when its count reaches zero — the pool drops its own reference on
  death, the reader thread holds one for as long as it runs (§ 2.3), and
  whoever leaves last frees it. No stream ever holds a
  pointer to a freed connection: the count is what makes that true, and
  it is the one invariant the Codex round reads hardest.

### 2.3 Threads and the socket — the central decision

Three facts about os64 shape this, and none of them is negotiable inside
this arc:

1. **Ring 3 waits on ONE handle at a time.** `read(handle, buf, len,
   timeout_ms)` is the only wait (syscall_numbers.h, "read's patience");
   there is no select or poll over a set of handles, and the GUI's
   wait-any is the window owner's alone.
2. **A TLS object has one serialized owner** (tls.h: "One serialized owner
   per client"; transport.h: "do not use the handle or reenter
   operations"). Two threads may not drive one TLS connection at once.
3. **A TCP connection parks one reader AND one writer** (tcp.h's `reader`
   and `writer` slots), so a thread blocked in read on a socket does not
   stop another thread writing to it.

An h2 connection must read frames for many streams while any of them may
want to send, and the thread that wants to send is a fetch's thread,
blocked nowhere the reader can see. So:

- **Each h2 connection has ONE READER THREAD**, started when ALPN answers
  h2 and detached (its handle closed: thread.h, "close(h) detaches"), so it
  costs no handle. It blocks in `os64_read_for` on the raw socket in
  SLICES (a second or less; the H2.4 brief names the number), and feeds
  what arrives to
  the TLS ENGINE under the connection lock, then parses frames out of the
  plaintext and delivers them to streams. A slice that ends with nothing
  read is the reader's turn to look around: at whether the connection is
  to end (below), at whether it has idled out (§ 2.5), and at ciphertext
  still owed to the wire (below). It holds a reference on the connection
  for as long as it runs. It never takes a stream's lock
  while holding the connection's for longer than a delivery, and it never
  blocks on a stream (§ 2.6 makes that true: a stream's buffer always has
  room for what the window allowed).
- **The TLS engine, not the transport wrapper.** fetch.c's h1 path drives
  `os64_tls_transport_*`, which owns the socket I/O and the single-owner
  rule with it. The h2 connection drives the ENGINE directly
  (`os64_tls_client_*`, tls.h: feed ciphertext, take ciphertext, read and
  write plaintext — "the second engine wears Bear's buffer interface",
  TLS13.md § Architecture) under one connection lock held for engine
  calls and the queue's bookkeeping, never across a syscall that can
  block (a bell's ring cannot, and § 2.4 is why some are made under a
  lock). The handshake itself is still the
  transport wrapper's (the dial is unchanged up to the ALPN answer); the
  wrapper's engine is then adopted by the connection. Whether the wrapper
  can hand its engine over, or the h2 connection must be created from the
  engine API end to end, is the H2.4 brief's first question of the code.
- **Ciphertext reaches the wire through one queue with one drainer, and
  no lock is held across a write.** Whoever has something to send — a
  fetch thread (HEADERS, DATA, WINDOW_UPDATE, RST_STREAM) or the reader
  (a SETTINGS ack, a PING ack, the connection's WINDOW_UPDATE for bytes it
  discarded, a GOAWAY, and whatever the engine itself owes after a feed:
  a KeyUpdate reply, an alert) — takes the connection lock, writes
  plaintext frames into the engine, moves the engine's ciphertext to the
  tail of the connection's OUT QUEUE, and lets the lock go. The queue's
  order is the engine's order, so TLS records reach the wire in sequence.
  Then, if nobody is draining (a flag, under the lock), that thread
  becomes the DRAINER: it takes bytes from the queue's head under the
  lock and writes them to the socket outside it, until it finds the queue
  empty and gives the role up in that same critical section. A thread
  that finds a drainer at work leaves its bytes to it and goes back to
  its own business. The rules that make this sound:
  - **Queued ciphertext is owed.** The engine's record sequence moved
    when it was made, so it cannot be withdrawn, and a record half
    written must be finished. A drainer drains to empty whatever becomes
    of its own fetch; its writes carry the connection's write patience
    (one number for the connection, since the bytes are everybody's), and
    a write that fails or outlasts it is the connection's death (§ 2.5),
    as it is on h1.
  - **A thread with no patience to give drains with none.** The reader,
    and a fetch that was cancelled, write what the socket takes at once
    and leave the rest queued with no drainer. The next sender takes the
    role; failing one, the reader tries again each time it wakes, which
    its slices bound. So the reader never blocks on the wire, and a
    blocked socket stalls senders and never the reading of frames. Bytes
    the socket has refused for longer than the write patience are the
    same death as a write that outlasted it, and the reader is who
    notices.
  - **The queue is bounded at the door, for what can wait.** A sender
    with DATA, or with a new stream's HEADERS, while the queue is past
    its mark (the brief names it) parks on its bell until the drainer
    brings it under. What ends a stream (a reset, a credit) is queued
    regardless and never waits: it is bounded by the streams that were
    let in. (Amended 2026-11-10 on Quinn's read of the H2.4 brief: the
    first wording bounded DATA alone and let HEADERS through, on the
    ground that live streams bound them, and a stream opened and
    cancelled frees its slot while its bytes are still owed.)
  A lock held across the write is set aside:
  `os64_lock_t` contenders spin and yield (lock.h), so every sender
  waiting on a stuck socket would burn a CPU for as long as it was stuck,
  the reader would wait there too for its acks, and a sender that gave up
  mid-record would leave half a record nobody owned.
- **The reader is the socket's only closer, and a flag is how it is
  stopped.** No verb hangs up a TCP connection without closing its
  handle, and a close from another thread, though it does wake the parked
  reader (`tcp_conn_close`), frees the handle's NUMBER for the next open:
  a drainer between two writes, or the reader between two reads, would
  then be speaking to whatever took the number. So nobody but the reader
  closes the socket. The connection carries `dying`, set under the
  connection lock by whoever decides it must end — the pool evicting or
  draining it, a drainer whose write failed, the reader itself (a
  transport failure, a GOAWAY, the idle timer). Senders look at it under
  the lock before they encrypt and send nothing on a dying connection; a
  drainer looks between writes and gives the role up. The reader looks
  each time it wakes, and once the connection is dying and nobody is
  draining it closes the socket, ends every stream (§ 2.5) and lets its
  reference go. The cost is that a connection somebody else condemned
  ends up to a slice later (or, with a drainer blocked in a write, when
  that write returns, which the write patience bounds), and one wake a
  slice per h2 connection.
  `fetch_pool_evict_one` therefore takes an idle h1 connection first,
  whose close is immediate, and `fetch_pool_drain` waits for the readers
  to leave (how is the H2.4 brief's).
- **A fetch never reads the socket.** It reads its STREAM: a buffer the
  reader fills, and a bell the reader rings (§ 2.4). From the fetch's
  side `os64_fetch_read` is unchanged in contract — bytes, 0 at the end,
  negative with a status — and `os64_fetch_open` returns when the
  stream's final HEADERS arrived, exactly as it returns today when the
  head was read.

Alternatives weighed and set aside, so the review need not re-derive them:
a connection with no thread, where whichever fetch needs bytes reads the
socket for everyone (fairness and hand-off between readers is a second
protocol, and a fetch satisfied early must keep reading for others or
strand them); a reader that polls the socket and a send queue by turns on
a tick (ten milliseconds of latency on every request, the flashlight
DOORBELL.md replaced in the kernel); a kernel poll over handles (the right
long-term primitive and its own design, not this arc's, and not needed
once the reader owns the socket and the out queue has its drainer); a
WRITER thread per connection parked on a bell, which would own every
socket write and make the drainer role unnecessary (one more thread and
one more handle per h2 connection, and the handle table of § 2.4 has no
sixteen to spare); and a kernel verb that hangs a connection up without
freeing its handle, which would stop a reader at once instead of within a
slice (new kernel surface for a second of teardown latency; Q3, ruled for
the slices, and booked in § 2.10 for the day the latency matters).

### 2.4 The bell — how a fetch waits for its stream

A fetch thread waiting for bytes on its stream has nothing to block on:
the socket is the reader's, and a lock is a spin (lock.h, "contenders spin
briefly, then yield"). The work pool solved the same problem for jobs with
pipes (work.c: a byte written per job, a worker blocked in `os64_read` on
the pipe), and that is the house's wait primitive today: A THREAD WAITS BY
READING A HANDLE WITH PATIENCE.

A pipe costs two handles and a 64 KiB ring (`PIPE_CAPACITY`) for a
one-bit message. The handle budget says that is too dear here: a task has
64 handles (`OS64_TASK_HANDLES`), yonder's baseline holds about nineteen
(its three standard handles, the work pool's two pipes, twelve worker
thread handles), the pool keeps up to sixteen, and thirteen fetches may be
in flight at once (twelve workers and a synchronous XHR on the window
thread). Thirteen pipe pairs is twenty-six handles on top of that, and the
table is full before a window or a file is opened.

**The bell is a handle whose read waits and whose write rings** — the pipe
with the ring buffer taken out: one handle, no buffer, one parked waiter
(pipe.c's single-waiter shape, `readWaiter`, the park discipline
`pipe_forget_waiter` and `signal_park_must_end` spell out), a flag that
stays set from a ring until a wait consumes it so a ring that lands before
the wait is never lost. `read(bell, &b, 1, timeout_ms)` returns 1 when
rung (the flag cleared), `OS64_ERR_TIMEOUT` when the patience ran out
unrung, and the interrupted answer when a caught signal cut the park short,
exactly as a pipe's read does; `write(bell, &b, 1)` from any thread of the
task sets the flag and wakes the waiter if one is parked; `close` frees it
and wakes a waiter with the closed answer. Bells are never inherited by a
spawned child (philosophy 6: nothing is inherited that was not asked for).
Thirteen in-flight fetches cost thirteen handles, and os64get's one fetch
costs one. It is `os64_bell()` in libos64 (`os64/bell.h`), and it is what
the pool's cap queue (§ 2.2), a stream's reads (§ 2.3) and a stream's
sends waiting on a window (§ 2.6) all park on: one bell per fetch, made
when the fetch first needs one and closed with it.

**A ring is made under the lock its owner takes to withdraw the bell.** A
ring is `write(handle)`, and a handle is a number: one that lands after
the fetch closed its bell, and after the number was given to the next
open, writes a byte into somebody else's file or socket. So whoever holds
a fetch's bell for ringing — the pool's waiter list, a stream — holds it
under a lock, rings while holding that lock (a ring never blocks, so it
may), and the fetch takes the same lock to remove its bell from that
place before it closes it. And because one bell serves a fetch's every
wait, a ring left over from the last one may open the next: every wait on
a bell is a loop that asks its question again when rung, and a ring with
nothing behind it costs one pass.

**This is kernel work** (a handle kind, a park loop, a syscall to make
one), small and in the class the review rule names, so it is its own slice
(H2.0) with a Codex round, and it is the subject of Q1: the alternative is
the pipe-backed bell in libos64 alone (no kernel change, the handle cost
above, which only yonder feels), with the same `os64/bell.h` either way so
nothing above it cares which was chosen. Fable's lean is the kernel bell.

### 2.5 Streams, fetches, and how each thing ends

- **A stream is one request and its response**, owned by one fetch. A
  redirect hop opens a new stream — on the same connection when the hop
  stays on the key, on another when it leaves — and the old stream is
  complete or reset. A fetch holds at most one stream at a time.
- **Opening a stream is sending its HEADERS** (END_HEADERS; END_STREAM
  when the request has no body), with the next odd stream id, taken
  under the connection lock so ids rise with sends as RFC 9113 § 5.1.1
  requires. The request body goes as DATA under the send windows (§ 2.6);
  the fetch's `sent_whole` means the same as today.
- **`os64_fetch_open` returns at the final HEADERS** (`:status` ≥ 200,
  END_HEADERS seen). Interim responses (1xx, 103 Early Hints among them)
  are consumed and ignored, as the h1 path consumes interim heads;
  trailers end the body and are not delivered, as h1's are not.
- **Cancellation is RST_STREAM(CANCEL) and no wait**: a fetch whose
  predicate says stop, or that is closed with its body unread, queues the
  reset (queuing never blocks; it drains with no patience, § 2.3), marks
  the stream closed, and leaves. Frames that arrive for a closed stream
  were sent before the reset was seen: they count against the
  connection's flow-control window, are discarded, and their bytes are
  credited back to the connection (§ 2.6), as are the bytes a closing
  fetch leaves unread in its stream's buffer.
- **A server's RST_STREAM ends the fetch** as `OS64_FETCH_BROKE` mid-body
  or `OS64_FETCH_REQUEST_FAILED` before the head, with the error code in
  the detail and in the reason sentence; `REFUSED_STREAM` before any
  response byte is retried once on another connection (or the same one,
  later) whatever the method, because RFC 9113 § 8.7 defines it as "the
  request was not processed".
- **GOAWAY** carries the last stream id the server processed. Streams
  above it were never processed and are RETRIED on a fresh connection,
  whatever their method, by the same reasoning (RFC 9113 § 6.8); streams
  at or below it run to their end on the connection, which accepts no new
  stream and dies when the last one leaves. The retry rule's count in the
  reason sentence (fetch.c `retried`) counts these too.
- **A silent connection is condemned by the first stream it fails.** A
  path that died without a FIN (a NAT that forgot the flow, a cable)
  looks like a live h2 connection with room, and without a rule every new
  fetch would join it and wait out its own idle deadline. So when a
  stream ends `SILENT` and the connection received no frame of any kind
  while it waited, the connection takes no new stream, exactly as after a
  GOAWAY: the streams on it meet their own deadlines, and it dies when
  the last one leaves. Silence on one stream while frames arrive for
  others is that stream's alone.
- **A connection's death** (transport failure, a write the socket would
  not take within the write patience, a connection error we
  detect, a GOAWAY with an error code) ends every open stream: BROKE
  mid-body, and before any response byte the h1 retry rule applies as
  written — retried once on a fresh connection when the request is
  idempotent and the connection was a kept one, else REQUEST_FAILED.
- **Connection errors we raise** (a frame that violates the protocol, a
  PUSH_PROMISE, an HPACK decode failure, a flow-control violation) send
  GOAWAY with the code and close; the detail names the code. A STREAM
  error we raise is RST_STREAM with the code, and that fetch alone ends.
- **Idle** is a connection with zero streams. It is kept like any idle
  connection and closed by the same idle timer, which the reader judges
  when a slice ends: zero streams, and kept longer than
  `FETCH_POOL_IDLE_MS`. The verdict is reached under the POOL's lock,
  where a take would find the connection, so a fetch can never take one
  the reader has just decided to close. A server's idle GOAWAY or FIN
  closes it sooner, and the reader sees that at once, so an idle h2
  connection taken from the pool is never one the server closed — the
  probe at take and the 408 rule are h1's.
- **PING is answered, never sent.** No keepalive pings: a connection that
  has nothing to say says nothing, and Chrome sends none either. What
  Chrome does send is a PING ahead of a request on a connection that has
  been quiet a while, to learn that the path died before the request
  finds out; the rule above is our cheaper answer to the same failure,
  and the probe is a row (§ 2.10).

### 2.6 Flow control: a window is what we are willing to buffer

- **Receive side.** Each stream has a buffer in user memory that grows on
  demand up to the stream window, and the connection window is the sum
  the connection will hold across streams. The reader never blocks on a
  stream because a peer that obeys flow control cannot send more than the
  window, and a peer that does not has made a connection error
  (FLOW_CONTROL_ERROR), not a full buffer. The consumer (the fetch's
  thread, in `os64_fetch_read`) sends WINDOW_UPDATE for its stream once it
  has consumed half the stream window, and for the connection once the
  connection has consumed half of its own, from the sending side of the
  connection (§ 2.3) — the reader thread sends no window update for bytes
  a fetch has yet to read, so a page that stops reading stops the server,
  which is the point of the mechanism. **Bytes nobody will ever read are
  credited by whoever discards them**, to the connection's window alone
  (their stream is gone): the reader for frames that arrive on a closed
  or reset stream, the closing fetch for what it leaves unread in its
  buffer. Without that, each cancelled stream would take its unread bytes
  out of the connection's window for good, and a connection that had seen
  enough cancellations would stop receiving for every stream on it.
- **The window values are the preset's** (§ 2.9), because the server sees
  them. The ceiling is the arithmetic the brief must state: thirteen
  streams at a Chrome-sized window is tens of megabytes IF every server
  floods every stream before its fetch reads a byte, which no server does
  to a client that is reading; the buffers hold only what has arrived and
  not been consumed, and the body cap (`max_body`) still ends a fetch at
  the byte it names. Q2 asks Chris whether to match the preset's window or
  cap ours below it at a memory cost he names.
- **Send side.** DATA frames are cut to the peer's `SETTINGS_MAX_FRAME_SIZE`
  and sent only within both the stream's and the connection's send
  windows; a sender out of window parks on its bell until the reader
  delivers a WINDOW_UPDATE, under the fetch's idle deadline and
  cancellation. Request bodies are small for a browser (a form, a JSON
  post) and large for a download tool's upload, and the rule is the same.

### 2.7 One request model, two renderers

`http_request` (http.c) writes the request as TEXT — a request line, `Host`
first, then the lines the caller's `order` names, then the rest in the
writer's own order — and the reply parser reads text. h2 carries the same
fields as a LIST: pseudo-headers (`:method`, `:authority`, `:scheme`,
`:path`) then fields, names lowercase, in HPACK. The contract:

- **One model.** The request is composed once as an ordered list of
  (name, value) pairs with the ordering rules http.h already states
  (`order`, the writer's own order for the rest, every value judged by the
  field-byte rule), and rendered two ways: h1 text, or h2 pseudo-headers
  and fields. `Host` becomes `:authority` (and is never also sent as
  `host`); the method, scheme and path become their pseudo-headers in the
  preset's pseudo-header ORDER (§ 2.9, captured). The connection-specific
  fields h2 forbids (RFC 9113 § 8.2.2: `Connection`, `Keep-Alive`,
  `Proxy-Connection`, `Transfer-Encoding`, `Upgrade`) are not rendered on
  h2, whatever `keep_alive` says — a preset's `Connection: keep-alive`
  describes its browser's h1 request, and its browser does not send it on
  h2 either. `TE` is not sent at all.
- **Cookies are crumbled on send and joined on receipt** (RFC 9113
  § 8.2.3): a `Cookie` line is sent as one `cookie` field per
  cookie-pair, as Chrome does, which is what lets HPACK index them
  individually; `cookie` fields in a response head do not occur, and
  several `set-cookie` fields reach `on_set_cookie` one by one as today.
- **The reply model** fills `os64_fetch_head_t` the same way: `:status`
  is `status`, `reason` is `""` (h2 has no reason phrase; callers already
  tolerate an empty one), `content-length` fills `length` when present,
  there is no chunked framing (DATA frames are the framing; `encoding` is
  the content coding as today), and `on_header` hears every field in wire
  order with the pseudo-headers left out. The head gains one fact,
  `http2` (true when the final head came over h2), because yonder's
  request record, lsof's story and a person's curiosity all want it; the
  detail gains the h2 error code when one ended the fetch.
- **The head's size limit holds** (`YONDER_FETCH_HEAD_MAX` in yonder is
  64 KiB, and libfetch has `HTTP_LINE_MAX` per line): a header block past
  what the decoder will hold is still DECODED to keep the HPACK dynamic
  table in step — a decoder that stops mid-block desynchronises every
  later head on the connection — and then the stream is reset
  (ENHANCE_YOUR_CALM, the code that says "you asked too much of me"; the
  H2.4 brief confirms the choice against RFC 9113 § 10.5.1, which is
  explicit only that the block must be processed either way) and the
  fetch ends `OS64_FETCH_UNSUPPORTED` with `head` in `unsupported`, as the
  h1 path ends on a framing header too long to read.
- **Bodiless replies** are the same three (a HEAD's answer, 204, 304) plus
  any stream whose final HEADERS carried END_STREAM. A 205's body is read
  and handed to nobody, as today.

### 2.8 The error vocabulary stays libfetch's

No new `os64_fetch_status_t` values. h2's ways of ending map onto the ones
callers already act on: a reset or a dead connection mid-body is `BROKE`;
before the head it is `REQUEST_FAILED` (or retried, § 2.5); a protocol
violation we detect is `BAD_HEAD` when it is the reply's shape and
`UNSUPPORTED` when it is a legal thing we refuse (a head too large, a
PUSH_PROMISE); the idle deadline is `SILENT`; the cancellation predicate is
`INTERRUPTED`. What is new is in `os64_fetch_detail_t` (the h2 error code
and which side sent it, the stream id) and in the reason sentence ("the
server reset the stream: REFUSED_STREAM"; "the connection went away:
GOAWAY ENHANCE_YOUR_CALM, retried once on a fresh connection"). yonder's
record prints the reason; nothing above libfetch learns new words.

### 2.9 The greeting is the preset's, like its headers

What the far end can fingerprint on h2, in the order it sees it: the
connection preface's SETTINGS frame (which identifiers, in what order, with
what values), the WINDOW_UPDATE that usually follows it (its increment),
whether PRIORITY frames are sent and what tree they build, the
pseudo-header order of the first HEADERS, and the HPACK choices in it. All
of it is the browser's, so all of it lives where the browser's headers
live: **`agent.c`'s preset table gains a GREETING** — the SETTINGS list,
the window increment, the priority behaviour, the pseudo-header order —
and libfetch's options gain a way to pass one (`os64_fetch_options_t`,
beside `order`). yonder's own preset sends its own honest greeting, which
the H2.5 brief writes down and which is also the default for a caller that
passes none (os64get, wend).

**The values come from captures, not from this file** (W1's rule, WIRE.md
§ 1 as built): Chrome and Firefox on Windows, each making a navigation and
a page's worth of sub-resources to a local h2 server that prints every
frame it receives — `nghttpd -v` with a self-signed certificate does this
(the nghttp2 server tools; Chrome is started with the flag that ignores the
certificate error for that host) — and the captures are kept beside the W1
set with the browser's version in their names. Rows C1 and C2. Firefox is
captured too because its preset exists and its greeting differs (it builds
a priority tree; Chrome does not).

**What is matched and what is not.** The SETTINGS (identifiers, order,
values), the window increment, the pseudo-header order and whether
PRIORITY frames are sent are matched byte for byte, because they cost
nothing but care. The HPACK encoder's indexing choices are not matched:
no published fingerprint scores them, and matching an encoder's internal
policy is a rabbit hole with no floor. The TLS handshake is still os64's
own (DIVERGENCES, "The browser's identity"); a gate that scores it keeps
scoring it, and this arc moves the line from "h1 under a Chrome string" to
"os64's handshake under a Chrome greeting", which is the honest next step.

### 2.10 Deliberately absent, and where each is booked

Server push (we send `ENABLE_PUSH = 0` where the preset does, and a
PUSH_PROMISE is a connection error); `h2c`, `Upgrade` and prior knowledge;
the h2 priority tree beyond what a preset's greeting sends; CONNECT and
extended CONNECT (no WebSockets over h2 — WebSockets themselves are a
separate design); Alt-Svc and HTTPS records (so h3 is never learned of,
and a host's h2 support is remembered per process only); keepalive PINGs,
and the PING Chrome sends ahead of a request on a quiet connection
(§ 2.5); a verb that hangs a connection up without closing its handle, so
a reader stops at once instead of within a slice (§ 2.3);
h2 over the TLS 1.2 fallback; h2 through a proxy. Each is one row in
DEBTS § libfetch when H2.5 lands, and none is a hook designed now.

## 3. The slices, in order, and what each depends on

| Slice | What | Reaches a page? | Depends on | Review |
|---|---|---|---|---|
| **H2.0** The bell | A handle whose read waits and whose write rings (§ 2.4): kernel handle kind, one syscall, `os64/bell.h` | No (no caller until H2.2) | Q1 | Codex (kernel) |
| **H2.1** Session resumption | TLS 1.3 tickets kept and offered; a resumed handshake skips the chain | Yes, on its own: every reconnect is cheaper | nothing | Codex (TLS) |
| **H2.2** Connections as shared objects, and the cap | `fetch_conn_t` with a count; the per-key queue in front of the dial (§ 2.2) | Yes, on its own: six per host, like Chrome | H2.0 | Codex (concurrency) |
| **H2.3** Framing and HPACK | Pure libraries with host tests and vectors | No | nothing | in-house |
| **H2.4** The connection | State machine, streams, flow control, the reader thread (§ 2.3, § 2.5, § 2.6), alone against a scripted peer | No (nothing calls it) | H2.0, H2.2, H2.3 | Codex (concurrency) |
| **H2.6** Fetches on the connection | fetch.c and the pool join it: § 2.2's h2 clauses, the retry rules, the words; a guest probe | No (ALPN still offers h1 alone) | H2.4 | Codex (concurrency) |
| **H2.5** The switch | ALPN offers h2; the preset half of the request model (§ 2.7); the greeting per preset (§ 2.9); captures; QEMU and P5 proof | YES — this is the arc | H2.6, C1, C2 | Codex, then Chris on the P5 |

H2.1 and H2.3 touch nothing the others touch and can run beside anything.
H2.0 goes before H2.2 because the cap's wait parks on a bell. Only H2.5 changes what the wire sees, so only H2.5 needs
Chris's P5 pass before merge; H2.1 and H2.2 change the wire's SHAPE (fewer
handshakes, fewer connections) without changing a byte of a request, and
get the QEMU `make run-net` page load every wire-facing slice gets.

Briefs for H2.0 through H2.4 follow in full. H2.6 and H2.5 have their
contracts above and get their briefs when their turn comes, written
against the tree as it stands then: H2.6's shape depends on what H2.4
became after its round, and H2.5's on the captures. (H2.6 is numbered
after H2.5 because it was split from H2.4 once the tracker's rows were
laid down; it is built before it.)

## 4. The briefs

Each brief is a builder's: the three passes (builder, one cold Opus
reader, Fable), then the review column above. Every brief's PR adds its
rows to the tracker as § 0 says, and its "as built" subsection here.

### H2.0 — The bell

**What.** A kernel handle kind with the semantics of § 2.4, and
`os64/bell.h` over it. Q1 ruled for the kernel bell; the pipe-backed
fallback's differences are marked where the brief was written with both
in view.

**Kernel.**
- A `bell_t` (kernel/src/bell.c, kernel/include/bell.h): a spinlock, a
  `rung` flag, one `thread_t *volatile waiter`. The park loop is pipe.c's
  shape to the letter — `pipe_read`'s discipline of clearing its own
  waiter slot at the top of every pass (`pipe_forget_waiter`), asking
  `signal_park_must_end` before parking, and `current_thread_will_catch`
  on the way out — because that loop has had its Codex rounds and this
  one must not re-learn them. The builder reads pipe.c's read path before
  writing a line, and the brief's first review question is "where does it
  differ from pipe_read, and why".
- `read(bell, buf, 1, patience)`: if `rung`, clear it, write one byte
  (value 1) and return 1; else park under the patience the read syscall
  already defines (0 polls, N ms, FOREVER), returning `OS64_ERR_TIMEOUT`
  unrung, `OS64_INTERRUPTED` when a caught signal ends the park, and the
  handle layer's closed answer when the bell is closed under the waiter.
  A second thread reading a bell that already has a waiter is refused
  (one parked waiter, as a pipe has one); the builder picks the existing
  refusal code that says "busy" and names it in the header.
- `write(bell, buf, len)` from any thread of the task: set `rung`, claim
  and wake a parked waiter (pipe.c's `pipe_claim_parked_waiter`), return
  `len`. A write never blocks and never fails for a full anything: a bell
  has no capacity.
- `close`: frees it; a parked waiter is woken with the closed answer
  before the memory goes (the builder reads how pipe.c orders this and
  does the same).
- **The pin.** A handle is resolved PINNED (handle.c's header, § The
  pin): an operation in flight holds a reference on the object in the
  object's own currency, so a sibling's close cannot free it under a
  parked waiter. A bell joins that as every kind has: a count of holders
  that `handle_pin` raises and the unpin drops, the waiter woken by
  `close` being the one who frees the bell when it leaves. `handle_share`
  (a spawn's redirections) refuses a bell.
- Not inherited across spawn (the 0/1/2-plus-what-was-asked rule); no
  `seek`, `stat` answers a bell as its own kind; `/proc/<pid>/handles`
  names it `bell`, and lsof (Chris's) shows it by that name.
- `SYSCALL_BELL` (the next free number in syscall_numbers.h, with its
  comment in the house style: what it makes and what read/write/close
  mean on it). Patience on a bell read must be accepted: the dispatcher's
  "other handles refuse finite patience" rule gains the bell beside pipes.

**libos64.** `os64/bell.h`: `os64_bell_t` (a struct holding what the
implementation needs: one handle for the kernel bell, two for the pipe
fallback), `int64_t os64_bell_make(os64_bell_t *)`, `int64_t
os64_bell_wait(os64_bell_t *, uint64_t timeout_ms)` (1 rung,
`OS64_ERR_TIMEOUT`, `OS64_INTERRUPTED`, negative closed), `int64_t
os64_bell_ring(os64_bell_t *)`, `void os64_bell_close(os64_bell_t *)`. The
header says what a bell is for (one waiter, many ringers, a ring is never
lost), what it is not (not a counter: two rings before a wait are one),
and the rule its ringers keep (§ 2.4: ring under the lock the owner takes
to withdraw the bell, because a ring is a write to a handle number).

**Proof.** `/tests/belltest`: ring before wait is not lost; wait then ring
from another thread wakes it; patience expires unrung with the timeout
answer; two rings, one wait, then a second wait times out; close under a
parked waiter wakes it with the closed answer; a SIGWINCH handler
installed, the wait interrupted and answering INTERRUPTED; a spawned child
that lists its handles sees no bell; a second thread's read on an occupied
bell is refused. The suite gains a kernel case for the park/ring race
(ring landing between the flag check and the park — the case pipe.c's
claim/forget pair exists for). Under the fallback, belltest runs unchanged
against the pipe-backed `os64_bell_t`, which is the point of the one
header.

#### H2.0, as built (2026-11-10, Fable)

The bell is `kernel/src/bell.c` behind `HANDLE_BELL`, made by
`SYSCALL_BELL` (67), wrapped by `os64/bell.h`. Where the build departs
from the brief, or says more than it did:

- **The owed wake, not pipe.c's claim and sweep.** The brief asked for
  `pipe_read`'s park loop to the letter. A pipe's wake acts only on a
  waiter that has finished parking, so pipes keep a list and a sweep in
  `processSignals` for the one that registered and had not parked yet,
  and a ring in that window costs a scheduler pass. `bell_ring` uses
  `scheduler_wake_or_owe` under the bell's lock instead (WAKE_OWED.md,
  the glass reader's shape): a waiter not yet parked is left a debt its
  own park pays. So there is no list of bells and no sweep, and the
  window is closed rather than swept. What is kept from pipe.c is the
  discipline the brief was after: the waiter voids its registration (and
  any debt) at the top of every pass under the lock it registered under,
  asks `signal_park_must_end`, and parks with a one-second backstop.
- **The answers.** A wait returns 1 with one byte when rung; a close under
  the wait returns 0, as a pipe's EOF and a closed glass view do ("no ring
  will come"); a second waiter is refused with -1, the syscall's plain
  refusal, since no handle kind has a busy code to borrow, and it is
  refused BEFORE the ring is looked at: a ring made while a waiter is
  registered is that waiter's, which may be awake and on its way back for
  it. A signal that ends the wait outranks a ring already made, and the
  ring stays for the next wait; so does the ring of a wait whose byte
  could not be written to the caller's buffer, which is rung back.
- **A ring is a write of at least one byte.** An empty write moves nothing
  on any handle and rings nothing here. A ring that finds the bell closed
  (possible only for a write that held its pin across a sibling's close)
  is refused.
- **The pin.** A bell counts its holders (`refs`): the handle, and each
  read or write in flight through `handle_pin`. `handle_share` refuses it
  by its default arm, so a spawn that names a bell as a child's 0/1/2
  fails, and a child's table never holds one.
- **The interrupt case is SIGINT, not SIGWINCH.** A program can raise
  SIGINT on a task through `/proc/<id>/ctl`; nothing lets one raise
  SIGWINCH without a pty. `belltest` spawns a child to send it, so the
  only thread of the fixture that can take the signal is the one parked
  in the bell.
- **Proof.** Kernel: `test_bell` (late phase) drives the object with no
  handle, and stands in the window itself: it registers as the waiter,
  rings while still running, then parks against a ten-second backstop
  (the park returned in 0 ticks). Ring 3: `/tests/belltest`, in testrun,
  nine steps through the syscalls with real second threads; its last is
  2,000 rings each a random fraction of a millisecond after the waiter
  took the one before (under QEMU's TCG, eight CPUs: all taken, worst
  1.4 ms, mean 200 us, against the one-second backstop a lost ring would
  wait for). Twelve hand mutants, each built and booted against both, and
  each caught by at least one: a ring, and a close, that wake only a
  parked waiter (the kernel case sleeps its whole backstop, and the race
  loses 8 rings of 2,000 at 1,008 ms each); a wait that leaves the ring
  set; no refusal of a second waiter; a second thread allowed the ring of
  a registered waiter; a close that wakes nobody; a wait that keeps its
  registration, or a debt owed to it; a wait that never asks
  `signal_park_must_end`; a wait that keeps a ring it could not deliver;
  and a pin that takes no reference.

### H2.1 — Session resumption

**What.** TLS 1.3 session tickets are kept and offered, so a connection to
a host visited moments ago authenticates by its earlier handshake and
skips the certificate chain and its signatures. TLS13.md's ruling 6 ("no
resumption") was Chris's on 2026-10-09 and is reversed by his 2026-11-09
ruling to run this arc; DEBTS § TLS 1.3 engine boundaries loses the
tickets clause, and TLS13.md § The profile gains the two extensions (45
on every hello, 41 with a ticket; its "not offered" list loses both) and
notes the reversal in one line.

**The engine (`userland/libtls/port/`).**
- **Keep what a NewSessionTicket says.** `ticket_byte` (tls13_handshake.c)
  already walks the message's fields and discards them; it now keeps
  `ticket_lifetime`, `ticket_age_add`, the nonce, the ticket (up to
  `TLS13_TICKET_MAX`, 2 KiB — a ticket longer than that is consumed and
  not kept, counted in the state so a harness can see it) and ignores
  `early_data` and any other extension. The PSK for it is
  `HKDF-Expand-Label(resumption_master_secret, "resumption", nonce,
  Hash.len)` (RFC 8446 § 4.6.1).
- **Derive `resumption_master_secret`** = Derive-Secret(master_secret,
  "res master", CH..client Finished) at the moment the client's Finished
  is sent (tls13_schedule.c; the line in TLS13.md § The key schedule that
  says it is not derived changes). `master_secret` lives until then, which
  it already does; `res master` lives until the connection closes, and is
  wiped with the rest.
- **The session cache** (`port/session_cache.c`): one per process inside
  libtls, under one lock, keyed by (hostname as the config normalises it,
  port, the trust store's pointer) — the store because a resumed
  connection is authenticated by a handshake verified under THAT store,
  and a connection to the same host under another store is not the same
  trust. An entry holds its own reference on that store for as long as it
  lives, as a kept connection does (pool.h's key): a store that could be
  freed while an entry named it would let another be made at the same
  address, and the pointer would stop being an identity.
  `os64_tls_config_t` gains `uint16_t port` for the key (0 means
  "do not resume", which is also the default for a zeroed config, so no
  caller changes behaviour until it says a port). Capacity 32 entries,
  two tickets per key (servers commonly send two), least recently added
  evicted; an entry carries the PSK, the ticket, the suite (so its hash),
  `received_at` on the platform clock, `lifetime` and `age_add`. Expired
  entries (lifetime passed, or past the seven-day maximum RFC 8446
  § 4.6.1 sets) are dropped when seen. **Each ticket is used once**: an
  offer removes it, as Chrome does and the RFC recommends against linking
  connections; the server's new tickets refill the key. Wiped on free.
  `os64_tls_sessions_drop(void)` empties it, for the harnesses between
  cases.
- **EVERY ClientHello that may resume says how: `psk_key_exchange_modes`
  (45) with `psk_dhe_ke` alone**, whether or not the cache holds a ticket
  for the key (a config with port 0 sends neither extension, and its
  hello is today's). ECDHE is always done, so a resumed connection keeps
  forward secrecy and no 0-RTT is ever possible. It goes on every hello
  because it is how a server learns the client can use a ticket at all:
  RFC 8446 § 4.2.9 tells servers not to send tickets a client's modes
  cannot use, and BoringSSL (Google's servers) and GnuTLS send none to a
  hello without it — so a client that offered 45 only alongside a ticket
  would never be given its first one. Chrome sends it on every hello too.
- **The ClientHello offers a PSK when the cache has one for the key**:
  `pre_shared_key` (41) LAST in the extension list as
  the RFC requires, one identity (the ticket, `obfuscated_ticket_age` =
  (age in ms + age_add) mod 2^32) and its binder. The binder is
  HMAC(finished_key(binder_key), Transcript-Hash(the truncated
  ClientHello)), binder_key = Derive-Secret(early_secret, "res binder",
  ""), early_secret = Extract(0, PSK). The truncated hello ends with the
  identities field: the binders list and the two-byte length in front of
  it are left out of the hash, while every length field before that point
  already counts them (RFC 8446 § 4.2.11.2). So the hello is built whole
  with the binder's bytes reserved, hashed up to the end of identities,
  and the binder filled in — the fiddly part, and the part the OpenSSL
  server will check for us. The suite list is
  unchanged; the ticket's hash must be one of the offered suites' (it
  always is: we offer every suite the engine speaks).
- **The server's answer.** `pre_shared_key` in the ServerHello with
  `selected_identity` 0 means resumed: the key schedule's `early_secret`
  is the PSK's, the handshake continues from EncryptedExtensions to
  Finished with NO Certificate and NO CertificateVerify (a server that
  sends them anyway after selecting the PSK is a protocol error), and the
  state reports `resumed = true`. No `pre_shared_key` in the ServerHello
  means declined: `early_secret` is Extract(0, 0^Hash.len) and the
  handshake is the full one, as today. A server selecting a suite whose
  hash differs from the ticket's while also selecting the PSK is a
  protocol error (RFC 8446 § 4.2.11). On HelloRetryRequest the second
  hello recomputes the age and the binder over the new transcript (§
  4.1.2) and keeps the offer.
- **Fallback.** The Bear TLS 1.2 client is untouched: no 1.2 session
  cache. A 1.2 fallback connection is never resumed and never offers.

**libfetch.** `dial` passes the peer port in the TLS config (resumption is
on for every direct https dial); `os64_fetch_detail_t` gains
`tls_resumed`, filled by `tls_snapshot`; the reason sentence does not
mention it (it is not a failure), but yonder's request record gains a
word for it beside the TLS version it already prints, and os64get's `-v`
says it.

**Proof.** The TLS 1.3 engine harness (tools/test_tls13_engine_host.py,
which drives the engine against OpenSSL; TLS13.md § Proof names its
ticket paths and `-num_tickets`): the first hello to a key with an empty
cache carries extension 45 and no 41, and a hello under port 0 carries
neither (checked on the hello's bytes, because OpenSSL may send tickets
to a hello without 45 and so cannot be what proves it is there); a first
connection
yields tickets in the cache (count, lifetime kept); a second to the same
key is resumed — the transcript shows no Certificate message, the state
says `resumed`, the application data flows; a server started with
`-num_tickets 0` leaves the cache empty and the next connection full; a
server that declines (restart it, so its ticket key changes) falls back to
a full handshake on the same connection attempt and refills the cache;
expired entries are not offered (the clock is the harness's); a ticket
over `TLS13_TICKET_MAX` is not kept; HRR (`-groups P-384` as the existing
HRR case does) with a PSK offered, resumed on the second hello; a wrong
binder (a mutant flips a byte) is refused by the server with
`illegal_parameter`, and the harness proves the case fails without the
mutant's catch; two keys that differ only by trust pointer do not share
tickets; use-once (a third connection after two resumptions with one
ticket each performs a full handshake). The fuzz loop
(tools/fuzz_tls13_host.py) gains the PSK server extension shapes. Mutants
through tools/mutants_parallel.py once at the end, twelve workers.
**Measurement in the PR body:** handshake time in the guest, cold and
resumed, against one host that sends tickets (micros around the dial in
os64get `-v`), and the connection count unchanged on WIRE.md's blog page
with its arrival time, before and after.

#### H2.1, as built (2026-11-10, Fable)

TLS13.md § Resumption is the record of what the engine does and of its
proof; this is where the build departs from the brief above, or says more.

- **The engine keeps nothing and reads no clock.** The brief put the
  keeping of a ticket's fields in the engine and the cache beside it. The
  engine (`tls13_resumption`, tls13_engine.h) is handed one session to
  offer and hands back each one that arrives; the client wrapper owns
  both, and the cache (`port/session_cache.c`) is the wrapper's. That
  keeps the engine a thing the harness drives with no platform behind it.
- **`OS64_TLS_RESUMED` is a flag, not a field.** The state's `flags`
  already said what a connection is; the struct did not have to grow.
- **The key's trust is an identity with a hold.** The cache is given a
  pointer it compares and never looks into, and a retain/release pair it
  calls once per entry; libtls passes the trust store and its own
  reference count. The cache therefore has no trust-store code in it and
  links into every harness that has the client engine.
- **The second hello reports the first hello's age.** The brief had a
  HelloRetryRequest's second hello recompute `obfuscated_ticket_age`. The
  engine has no clock, the two hellos are a round trip apart, and with no
  0-RTT a server has nothing to protect with the age; the binder, which
  is what matters, is recomputed over the new transcript.
- **A second hello drops an offer the retry made unusable.** When the
  HelloRetryRequest fixes a suite whose hash is not the session's, the
  session is not offered again, and a ServerHello may not accept what the
  hello it answers did not offer.
- **Lifetimes.** Zero is the server's "discard at once" and is not kept;
  more than seven days is held to seven. `TLS13_ERR_PSK`
  (`illegal_parameter`) is the engine's word for an acceptance it may not
  honour: an identity it did not offer, or a suite of another hash.
- **The clock is `os64_micros`**, which does not jump when the wall clock
  is set: a session's age and its lifetime are both told by it.
- **A redundant test left out of the cache.** "The clock has gone back"
  needs no clause: the age is an unsigned difference, and a clock behind
  the keeping reads as an age no lifetime reaches. A mutant that removed
  the clause was not caught because nothing had changed; the clause went.
- **The server that declines is another context, not a restarted
  process.** The harness runs OpenSSL in process, so "restart it, so its
  ticket key changes" is a second `SSL_CTX`, whose keys cannot open the
  first one's tickets.
- **libfetch.** Every direct https dial passes the peer's port. A fetch on
  a kept connection reports that connection's own handshake, so
  `tls_resumed` says how the connection was made, not whether this fetch
  made it. `/tests/resumeprobe` is the hand probe: it opens one address
  several times on connections of its own and says which resumed.
- **A debt booked on the way** (DEBTS, beside "TLS 1.3 engine
  boundaries"): the hello is still os64's own and a fingerprint tells it
  from a browser's. Chris asked on 2026-11-10 whether it could ride this
  slice; it is its own design, under the rule that a hello never
  advertises what the client cannot honour or safely ignore.
- **Proof beyond the engine harness.** The four TLS host suites and the
  thirteen fetch, libway and os64get ones pass with the cache linked in;
  the library's export audit passes with one new export and four new
  imports; the fuzz loop ran 11,138 cases in two minutes with no
  sanitizer finding. Thirty-seven hand mutants
  (`tools/test_tls13_resumption_mutants.py`), every one caught, after a
  first run that missed two: nothing had fed a ticket split across
  records, so the code that reassembles one could be broken unseen.
- **Measured in the guest** (QEMU under TCG, e1000 on slirp, one public
  https site, `resumeprobe ... 4`): the first open, a full handshake,
  407 ms; the three after it, each resumed on a connection of its own,
  124, 140 and 125 ms. The first figure includes the name lookup, and TCG
  overstates what the signature checks cost, so the P5 is the number to
  trust.
- **Measured on the P5, and what it says** (Chris, 2026-11-10, one public
  site, `resumeprobe ... 4`): the full handshake's open 149 ms; the three
  resumed ones 157, 139 and 160 ms. No difference outside the network's
  own variation. A TLS 1.3 handshake is one round trip full or resumed,
  and `psk_dhe_ke` resumption removes none of it: what it removes is the
  certificate chain's bytes and the CPU that checks them, which on this
  machine is milliseconds inside an open that is mostly round trips. The
  brief's claim, and § 1's ranking of this slice as "the cheapest cut of
  the same handshake cost", held under QEMU's emulated CPU and does not
  hold on the hardware. What the slice buys on the P5 is a client that
  resumes as every browser does, fewer bytes, and less work on a slow or
  loaded CPU; what cuts a page's waiting is fewer handshakes, which is
  H2.2's cap and H2.5's one connection per origin. Only 0-RTT would take a
  round trip out of a reconnect, and § "The hello" in TLS13.md says why
  that is not offered. **Not measured:** WIRE.md's blog page before and after, which the
  brief asked for. Resumption changes no connection count, and that
  page's arrival time under TCG is its scripts'.

### H2.2 — Connections as shared objects, and the cap

**What.** § 2.2's ownership model, built for h1 alone so it reviews on its
own: `fetch_conn_t`, a count, a pool of pointers, and the per-key cap in
front of the dial with its queue on the bell. Behaviour-neutral for a page
that opens six connections or fewer; a page that dialled a host from
every worker at once now opens six and queues the rest (its other workers
wait in the queue, holding their threads), which is Chrome's behaviour and the DEBTS row "Fetches to
one host are not capped at once" paid.

**pool.h / pool.c.**
- `kept_t` becomes `fetch_conn_t`, heap-allocated by the first take or
  the dial that makes it: `key`, `io` (the transport), `protocol`
  (`FETCH_PROTO_H1` only in this slice; the enum exists so H2.4 adds a
  value, not a field), `refs`, `kept_at`, `dialling` (a reservation: the
  slot is counted toward the key's cap while its dial is in flight, and
  released if the dial fails). The pool table is `fetch_conn_t
  *s_conns[FETCH_POOL_TOTAL]` — idle entries have `refs == 0`, live ones
  `refs == 1`, dialling ones `dialling` set — and `FETCH_POOL_TOTAL`
  stays sixteen and keeps its handle-table reasoning, now counting LIVE
  connections too, which is a change: today sixteen idle plus a dozen in
  use is twenty-eight handles, and the assert "a quarter of the table"
  was written for the idle ones. The brief's number: the table counts
  every connection libfetch holds, idle or live, and sixteen in all is
  the cap for both; a fetch to a new host with sixteen LIVE or dialling
  waits on the total exactly as a seventh to one host waits on the key's,
  and with an idle one among the sixteen it closes that one and dials
  (§ 2.2: an idle connection never makes a fetch wait). (The
  builder measures a corpus page's distinct hosts at once; if sixteen
  starves a page, the number moves and the reasoning in pool.h moves
  with it.)
- `fetch_pool_take(key, idle_most_ms, &conn)`: § 2.2's order less the h2
  clauses: an idle connection of the key (probed by the caller as today),
  else a dial slot if the key is under `FETCH_POOL_PER_KEY` and the table
  under `FETCH_POOL_TOTAL` — making the slot, when an idle connection is
  all that stands in the way, by closing it (§ 2.2: the table's least
  recently kept idle one when the table is full; the key's own idle one
  that this fetch passed over when the key is at its cap; closed after
  the lock is let go, as the pool closes everything) — else WAIT. The
  wait: the fetch's waiter node
  (its bell, its key, a link) goes on the pool's waiter list under the
  lock; the lock is released; `os64_bell_wait` under the remaining idle
  patience; the node is REMOVED under the lock before any return (the
  park-loop discipline, § 2.4's pipe.c citation applies in ring 3 too:
  a node left on the list naming a fetch that returned is a wake of a
  stranger or a read of freed memory); then the question is asked again.
  `fetch_pool_put`, a dial's end and a failed probe's close ring EVERY
  waiter whose key matches, and whatever frees a TABLE slot rings every
  waiter (§ 2.2: a ring is a hint, and one waiter rung alone may be the
  one that is leaving). The rings are made under the pool's lock (§ 2.4).
  A fetch whose patience runs out waiting answers
  `OS64_FETCH_DIAL_FAILED` with `detail.dial = OS64_NET_ERR_NO_RESOURCES`
  and `why` saying "waited <n> ms for one of <key>'s <cap> connections",
  because "no road within patience" is the truth of it; a cancelled wait
  is `INTERRUPTED`.
- `fetch_pool_put(conn)` decrements, keeps when `connection_reusable`
  said so (the caller's judgement, as today), frees the object otherwise,
  rings as above. `fetch_pool_evict_one` and `fetch_pool_drain` keep
  their contracts over the new table (a live connection is never
  evicted). `fetch_pool_idle()` counts `refs == 0`; a `fetch_pool_live()`
  joins it for the harness.
- One lock still, held to take, put, reserve or release a slot, to edit
  the waiter list, and to ring the bells on it. Never across a dial, a
  probe, a close or a bell wait.

**fetch.c.** `take_kept` (built as `acquire`), `dial` and `release_connection` speak to connections
instead of transports; the fetch gains its bell, made on the first wait
and closed with the fetch (`os64_fetch_close`); the retry rule, the 408
rule, the probe and the ONCE_MS rule are untouched and their cases prove
it.

**Proof.** tools/test_fetch_pool_host.c (the pool's threaded harness,
pthreads under the sanitizers — the "two threads never share a
connection" sweep lives there) for every case that waits, and
tools/test_fetch_host.c (one thread, the scripted peer) for the rest: a
seventh fetch to a key with six live waits and
proceeds when one is put back; it proceeds when one dies; it is
INTERRUPTED when cancelled while waiting and its node is gone (a mutant
that skips the removal is caught by the sanitizer's read of the freed
node); its patience expiring answers DIAL_FAILED with the sentence; a
new key with sixteen live waits on the total, and with fifteen live and
one idle it closes the idle one and dials without waiting; a request sent
once at a key's cap with an idle connection too old for it closes that
one and dials; three waiters on one key and the first rung cancelled as
it wakes — a put still reaches one of the other two (a mutant that rings
one waiter strands them and is caught by their patience expiring); a
ring made after a waiter left reaches no bell (the harness's bell stub
fails the case on a ring to a closed bell); a dial slot reserved by a
dial that fails is released and the waiters rung; every existing pool
case passes
unchanged (reuse, eviction, the probe, the retry rule, ONCE_MS) with the
counts read through the new accessors; the sweep that proves two threads
never share a connection stays. The harness stubs the bell behind
`os64_bell_t` (make, wait, ring, close), which is the whole of what the
pool sees of it. QEMU `make run-net` against the W2 local page: the dump shows at most
six SYNs to the host at once and the requests queued behind them, and the
arrival time within the W2 figures. P5: Chris on the blog page and the
storefront, by feel and by the record's connection count.

#### H2.2, as built (2026-11-10, Fable)

The pool is `userland/libfetch/pool.c` still, with pool.h's contract
rewritten; fetch.c asks it through `acquire`. Where the build departs from
the brief above, or says more than it did:

- **What it removes, said first** (the lesson of H2.1's measurement). Not
  latency by itself. A page that asked one host for twelve things at once
  opened up to thirteen connections and now opens six: up to seven fewer
  TCP and TLS handshakes a burst, and a client that looks like a browser
  to the server. The requests that would have dialled wait for one of the
  six instead, so a page of many small same-host requests should load
  about as fast, and one whose replies are slow can load SLOWER, at half
  the parallelism: Chrome's own trade. What makes a page faster is H2.5,
  where the twelve go at once down one connection; this slice is the
  ownership that needs, and the cap HTTP/1.1 sites get meanwhile.
- **THE WAIT IS NOT UNDER THE FETCH'S IDLE DEADLINE.** § 2.2 and the brief
  put it there. That deadline measures a server's silence, thirty seconds
  by default; a fetch in this queue is waiting on its own program's other
  fetches, and six slow downloads to one host would have failed the
  seventh at thirty seconds where before it simply dialled. The wait has
  its own bound, `FETCH_POOL_WAIT_MS`, five minutes: a browser's queue has
  none, and this one has a long one so that connections held by fetches
  nobody is reading end as a named refusal and not as a wait nobody can
  explain. Cancellation is a flag that rings nothing, so a waiter looks
  up once a second (`FETCH_POOL_LOOK_MS`) to ask, and leaves as
  INTERRUPTED within that.
- **A connection's transport is lent, not shared.** `fetch_conn_t` is the
  heap object the brief asked for (key, transport, protocol, count, when
  it was kept, whether it is being dialled). An HTTP/1.1 connection has
  one user, so the fetch still works on its own copy of the transport, as
  it always did, and the object holds the transport while the connection
  is idle. H2.4's connections keep theirs in the object.
- **`fetch_pool_take` answers one of two things to do**: KEPT (probe it;
  `fetch_pool_gone` if the probe refuses) or DIAL (a slot reserved;
  `fetch_pool_dialled` or `fetch_pool_gone`). The old pool's `put` took a
  key and found a slot; now a connection has had its slot since before
  its dial, which is what makes the cap a cap on what a server sees.
- **Room is made before the dial, not after the put.** With the table
  full, the least recently kept connection goes when the next dial needs
  its slot. The old pool closed it when the seventeenth was given back. A
  consequence in the harness: "out of handles" now needs a task with one
  handle fewer than before to reach the refused dial.
- **Every waiter is rung at every put and every departure**, of any key:
  a connection given back is one a waiter of its key can take and one a
  waiter of another key can put out for its slot.
- **A fetch that cannot have a bell cannot wait**, and says so
  (`FETCH_POOL_FAILED`, DIAL_FAILED with the sentence). The bell is made
  at a fetch's first wait and closed with the fetch, after its connection
  is released.
- **The reason sentence.** A wait that ran out is DIAL_FAILED with
  `detail.dial = OS64_NET_ERR_NO_RESOURCES`, and the sentence is the
  pool's ("waited N ms for one of the 6 connections to it, all in use",
  or "one of this program's 16 connections" when it was the whole table
  that was full), not the kernel's word for no resources, which would
  point at the wrong thing.
- **`fetch_pool_counts`** reads the kept and the in-use counts in one
  look. The harness needed it: adding two separate reads counted a
  connection twice as it changed hands.
- **`tools/host_bell.c`** is os64/bell.h for the host harnesses that link
  the pool: a mutex and a condition with the bell's three rules, a ring of
  a bell that is not one stopping the program, and hooks only a stand-in
  can give (a wait that never blocks, for a harness with one thread; an
  interrupt, as a caught signal would be; a refusal to make one).
- **Proof.** `tools/test_fetch_pool_host.sh` (ASan and UBSan, then TSan):
  each rule of the cap and the wait as a case of its own, then eight
  threads on three keys, 160,000 asks, about 26,000 connections made,
  never seven in use to a key or seventeen held in all, several hundred
  to some 1,500 waits a run, every connection closed once; ten runs in a row clean. (One TSan report
  on an earlier run, before a read the harness itself made of a field
  another thread was writing was replaced with an atomic flag; it was not
  kept and has not come back.) `tools/test_fetch_host.sh`: the six-per-key
  case rewritten for the cap, with the wait's sentence checked word for
  word, and a check at the end that no case left a fetch holding a place
  or a bell; 2,781 checks. The libway, os64get, wend and yonder fetch-job
  suites pass with the pool linked in. Mutants:
  `tools/test_fetch_keepalive_mutants.py`, 102 of 102 caught, 26 of them
  new for the cap and the wait. The first run missed two (the order
  `fetch_pool_evict_one` closes in, which the fetch harness no longer
  reaches now that room is made before the dial, and a cancelled waiter
  taking the connection that rang it); each has a pool case now.
- **In the guest** (QEMU under TCG, e1000 on slirp, one public site):
  `/tests/capprobe <address> 12` starts twelve fetches at one moment, each
  holding its connection a third of a second: none failed, and at most six
  held a connection at once. `testrun`: 63 passed, 0 failed, 6 skipped.
  **Not done:** the brief's `make run-net` dump counting SYNs on the W2
  page, and the corpus page's count of distinct hosts at once (whether
  sixteen in all starves a page). capprobe shows the cap on the wire's
  near side; the SYN count would show it on the far side.

### H2.3 — Framing and HPACK

**What.** Two pure libraries in libfetch with no kernel or syscall
dependency, host-tested, unreachable from any program until H2.4 calls
them: `userland/libfetch/h2_frame.c/.h` (the frame layer) and
`userland/libfetch/h2_hpack.c/.h` (field compression). Plus the pure
validators H2.4 will need and this slice can prove alone:
`h2_head_check` (RFC 9113 § 8.2's rules on a decoded field list) and the
error-code and setting-identifier name tables.

**h2_frame.**
- `h2_frame_header_t` (length, type, flags, stream id) from nine bytes,
  with the reserved bit cleared; `h2_frame_parse(header, payload, len,
  our_max_frame_size, &frame)` validates length against type (SETTINGS a
  multiple of six and zero when ACK, PING eight, WINDOW_UPDATE four with a
  nonzero increment, RST_STREAM four, GOAWAY at least eight, PRIORITY
  five), stream-id rules per type (zero where the type is a connection's,
  nonzero where it is a stream's), padding (PADDED flag: the pad length
  byte and that it fits, on DATA and HEADERS and PUSH_PROMISE), the
  PRIORITY fields on a HEADERS with the flag, and hands back the payload's
  meaningful bytes. Unknown frame types are a parsed frame of kind
  `H2_FRAME_UNKNOWN` (RFC 9113 § 4.1: ignored, never an error). A
  violation is `H2_ERR_FRAME_SIZE` or `H2_ERR_PROTOCOL` with which rule,
  for the connection to turn into a GOAWAY.
- Writers for every frame the client sends: SETTINGS (a list of
  identifier/value pairs IN THE ORDER GIVEN — the greeting's order is
  data, § 2.9), SETTINGS ack, WINDOW_UPDATE, PING ack, RST_STREAM, GOAWAY,
  HEADERS (with END_HEADERS and END_STREAM flags, and CONTINUATION split
  when the block is past the peer's max frame size), DATA (cut to a size
  the caller gives), PRIORITY (for a preset that sends them). Each writes
  into a caller's buffer and refuses what does not fit.
- The connection preface string, and the SETTINGS and error-code names
  (`h2_error_name`, `h2_setting_name`) for reason sentences and `/sys`
  style readers later.
- CONTINUATION reassembly is a small state object: a HEADERS or
  PUSH_PROMISE without END_HEADERS opens a block; only CONTINUATION on the
  same stream may follow until END_HEADERS; anything else is a connection
  error; the block is bounded by the caller's head limit (§ 2.7's "decode
  anyway, then reset" is the CONNECTION's policy — this layer only
  enforces the hard cap past which the connection is closed).

**h2_hpack.**
- Decoder: `h2_hpack_decoder_t` with the static table (61 entries, RFC
  7541 Appendix A), a dynamic table bounded by the size WE advertised
  (`SETTINGS_HEADER_TABLE_SIZE`, the preset's value), integer decoding
  with a width cap (an integer past 2^31 is an error, not a wrap), string
  decoding plain or Huffman (the 257-symbol code of Appendix B as data; a
  padding longer than seven bits, or not all ones, or an EOS symbol in the
  string, is `H2_ERR_COMPRESSION`), dynamic table size updates (allowed
  only at a block's start, never above the advertised maximum), indexed
  and literal representations with and without indexing, the
  never-indexed flag carried to the caller. `h2_hpack_decode(dec, block,
  len, emit, ctx)` calls `emit(name, name_len, value, value_len, sensitive)`
  per field in order; it decodes THE WHOLE BLOCK even when the caller's
  emit says "too much" (§ 2.7), so the table stays in step.
- Encoder: `h2_hpack_encoder_t` with a dynamic table bounded by the PEER's
  advertised size (4096 until its SETTINGS say otherwise; a shrink is
  announced with a table size update at the next block's start as the RFC
  requires), indexing policy: a field found in either table by name and
  value is sent indexed; one found by name alone is sent literal with
  incremental indexing; `authorization` and a caller-marked sensitive
  field are sent never-indexed; strings are Huffman-coded when that is
  shorter. Cookie crumbling is the connection's (§ 2.7), above this
  layer.
- `h2_head_check(fields, n, is_response)`: names lowercase and legal
  field bytes, pseudo-headers before regular ones and only the four
  request ones or `:status`, none duplicated, no connection-specific
  field, no `te` other than `trailers`, a `content-length` that parses.
  Returns the rule broken, for a stream error.

**Proof.** tools/test_h2_host.sh (one script building both files with
their test under ASan/UBSan, the test_fetch_host.sh shape): RFC 7541
Appendix C's worked examples, every one, byte for byte in both
directions (C.1 integers, C.2 literals, C.3 and C.4 request sequences
plain and Huffman with the dynamic table's state checked after each, C.5
and C.6 responses with evictions); every frame type's length and
stream-id rules, each violation named; padding rules; CONTINUATION
reassembly across one, two and three frames and the interleaving errors;
unknown types ignored; a block fed one byte at a time through the
reassembler; the head-check rules one by one; the encoder's table
shrinking on a smaller peer setting; a fuzz loop over random blocks and
frames under the sanitizers (tools/fuzz_tls13_host.py's shape) run for a
fixed count in the script. A mutant per rule, run once at the end. No
QEMU: nothing in this slice runs in the guest yet.

#### H2.3, as built (2026-11-10, Fable)

`userland/libfetch/h2_frame.c/.h` and `h2_hpack.c/.h`, with RFC 7541's two
tables as data in `h2_hpack_tables.h`. They are compiled into libfetch.so
with the library's hidden visibility and nothing calls them: no program's
behaviour changes with this slice. Where the build departs from the brief,
or says more than it did:

- **A fault says whose it is.** `h2_fault_t` is the error code, the rule,
  and whether RFC 9113 makes it the stream's or the connection's. Two
  frame rules are the stream's (a PRIORITY of the wrong length, a
  WINDOW_UPDATE of nothing on a stream); the connection would otherwise
  have had to know that by frame type.
- **`h2_frame_header_check`** judges a header alone, so a connection can
  refuse a frame longer than its max frame size before reading sixteen
  megabytes of it. `h2_frame_parse` takes the payload's length from the
  header and not as a separate argument.
- **`flow_len`** on a parsed DATA frame is the whole payload, padding
  included, which is what the windows are charged (§ 6.9.1); `data_len`
  is the body's bytes alone.
- **`h2_setting_check`** judges a setting's value (ENABLE_PUSH, the
  initial window's 31 bits with its own error code, the max frame size's
  two bounds). The brief did not list it; H2.4 applies the peer's
  settings and would have written it there.
- **Names are never NULL**: an error code or a setting this file has no
  name for answers "UNKNOWN_ERROR" or "UNKNOWN_SETTING", since a peer may
  send either and a reason sentence must print something.
- **The reassembler copies every fragment** into the caller's buffer,
  the one-frame block included, so there is one path. A block past the
  buffer is the connection's ENHANCE_YOUR_CALM.
- **HPACK allocates nothing.** Each side is given its storage once
  (`H2_HPACK_STORAGE(size)`), and the decoder a scratch for one literal
  field. A literal's name and value are copied to the scratch before the
  field is added, which is what makes RFC 7541 § 4.4's case sound: a
  field named by the index of the entry its own insertion evicts.
- **Three answers from a decode.** `H2_HPACK_OK`; `H2_HPACK_REFUSED`, the
  block decoded whole and the table in step but its fields not all
  delivered (the caller's emit said enough, or a field was larger than
  the scratch), which is the stream's problem; `H2_HPACK_BAD`, the
  connection's COMPRESSION_ERROR.
- **The decoder's table size has a before and an after.** Until the peer
  has acknowledged our SETTINGS it may encode against the default 4096
  whatever we advertised, so the decoder allows the larger of the two;
  `h2_hpack_decoder_settings_acked` then holds it to what we advertised,
  and when that is under the table's size at that moment, the peer's next
  block must begin with the size update that shrinks it (RFC 7541 § 4.2)
  or the connection ends.
- **The encoder has a ceiling of its own** (its storage) and follows the
  peer's size up to it, announcing each change at the next block's start,
  the smallest size it passed through first. An encoder made with a
  ceiling under 4096 announces that in its first block.
- **A field too large for the table is sent without indexing**, not
  added: adding it would only empty the table.
- **A sensitive field is never sent as an index**, even when a table
  holds it whole: an index carries no never-indexed instruction, and a
  decoder hands it over as an ordinary field. (Codex, on the PR.)
- **"Huffman when shorter" is not "Huffman always"**, and one of the
  RFC's examples shows the difference: C.6.2's `:status: 307` is three
  bytes either way, the RFC's encoder codes it and ours does not. The
  harness checks our bytes for that one string and the RFC's bytes for
  everything else, in both directions. The encoder has a `huffman` switch
  (on by default) because C.3 and C.5 are the plain-string examples.
- **An integer is at most 2^31 - 1 and at most five continuation bytes.**
- **Padding is judged after a frame's fixed fields.** A HEADERS or
  PUSH_PROMISE too short to hold its priority fields or its promised
  stream is FRAME_SIZE_ERROR; one that holds them, with padding longer
  than what is left for the block, is PROTOCOL_ERROR (§§ 6.2, 6.6).
- **An owed size update is owed by the very next block**, an empty one
  included.
- **`h2_head_check` takes a kind** (request, response, or trailers, which
  may carry no pseudo-header at all) and answers which field broke the
  rule. Beyond the brief's list it requires a response's `:status` to be
  three digits, a request's method, scheme and path to be present
  (`:authority` may be absent, § 8.3.1), and a value to carry no NUL, CR
  or LF and no space or tab at either end (§ 8.2.1). CONNECT's different
  pseudo-header set is not modelled (§ 2.10).
- **Proof.** `tools/test_h2_host.sh` under ASan and UBSan, 630,000-odd
  checks in about eleven seconds: the Huffman code proved canonical and
  complete from its lengths; RFC 7541 C.1 to C.6, each block in both
  directions with the table read after it; every frame type's rules by
  name; the writers read back through the parser, HEADERS split at ten
  block sizes around the frame boundary; reassembly including a block
  one byte to a frame; each of the decoder's refusals; the head rules one
  at a time; and three random loops (blocks at a decoder, frames at the
  parser, field lists through an encoder into a decoder with the table
  size moving). Mutants: `tools/test_h2_mutants.py`, 215 of 215 caught,
  one deliberate break to a rule. The first whole run caught 192: ten
  mutants did not compile and were rewritten, five were gaps in the
  harness and each has a case now (a setting identifier past one byte,
  the settings count asked of another frame, a content-length of `-`, a
  plain string copied past the scratch, a name and a value that fit the
  scratch apart and not together), and one was a line in the encoder
  that did nothing, which is gone.
- **The harness ends itself.** It stops at its fiftieth failed check and
  sets a two-minute alarm. One mutant (the table evicting nothing) made a
  loop here print a failure line for each of a wrapped entry count, at
  125 MB a second, into a log the mutant harness then read back whole:
  it filled the disk and then memory, twice, before it was found. The
  runner's own ceilings are `tools/mutants_parallel.py`'s (#534).
- **The sanitizer does not watch libos64's long copies** in a host
  harness (DEBTS § Build system), so the harness keeps guard bytes after
  the decoder's scratch and checks them.
- **A second opinion on the decoder**, run once on the host and not kept
  in the tree: the hpack-test-case stories that ship in the WPT checkout
  (the output of eleven other implementations' encoders), 36,990 blocks
  and 430,305 fields, every one decoded to the fields the corpus lists.

### H2.4 — The connection

**What.** `userland/libfetch/h2_conn.c/.h`: one HTTP/2 connection as an
object, with its reader thread, its streams, its windows and its one way
of dying. It is the first caller of h2_frame and h2_hpack. Nothing in
fetch.c calls it in this slice: it is proved alone, against a scripted
peer, with real threads under the sanitizers. Joining it to fetches (the
pool's h2 clauses, the retry rules, the error words) is H2.6, split from
this slice by Q4's ruling; `dial` offers `http/1.1` alone until H2.5 either way.

Contracts: § 2.3 (the reader and its slices; the engine under the lock;
the out queue and its drainer; `dying`, and the reader as the socket's
only closer), § 2.4 (a ring is made under the lock its owner withdraws
the bell under), § 2.5 (every way a stream and a connection end), § 2.6
(windows and buffers), § 2.7 (the head limit).

**The first question of the code, answered: the wrapper hands its engine
over.** `os64_tls_transport` (libtls/transport.c) is a struct of the
engine (`os64_tls_client *`), the socket, and two ciphertext buffers. The
dial stays the wrapper's to the end of the handshake, and libtls gains
one verb:

    os64_tls_status_t os64_tls_transport_release(os64_tls_transport *transport,
        os64_tls_client **client, int32_t *handle, void *unread, size_t room, size_t *unread_len);

Legal once the handshake is done, with nothing closing and no ciphertext
of its own still to send (it flushes first, under the caller's patience,
and refuses if that does not empty it). It hands back the engine, the
socket, and whatever ciphertext it had read and not yet fed (the server's
SETTINGS may ride in the same segment as its Finished), wipes and frees
the wrapper, and closes nothing. On refusal the wrapper is intact and the
caller closes it as it would have. Building the connection on the engine
API from the first byte was set aside: it is the handshake loop, its
deadline and its resumption plumbing written a second time.

**Sizes and times, each with its reason.**

| Name | Value | Why |
|---|---|---|
| `H2_READER_SLICE_MS` | 500 | How long the reader sleeps in one read. It is the most a connection condemned from outside lingers, and it costs two wakes a second for each h2 connection, 32 at the pool's sixteen. A second doubles the linger; a quarter doubles the wakes for a delay nobody sees. |
| `H2_WRITE_PATIENCE_MS` | 30000 | How long the socket may refuse the queue's head before the connection dies: `FETCH_IDLE_MS_DEFAULT`, since the bytes are every stream's and no one fetch's deadline is theirs. |
| `H2_OUT_MARK` | 256 KiB | Past this much queued ciphertext, a sender with DATA or with a new stream to open parks. Sixteen full frames: enough that a fast socket never parks a sender, little enough that a stalled one is felt by the sender and not by memory. |
| `H2_OUT_CONTROL_MOST` | 1 MiB | Past this much queued, the frames a PEER makes us owe (PING and SETTINGS acks, resets, credits) end the connection with ENHANCE_YOUR_CALM. A peer that floods pings at a socket it will not read is otherwise a way to grow the queue without bound. |
| `H2_STREAMS_MOST` | 64 | The stream table's size, a task's handle count: a fetch holds a bell, so no task has more fetches than handles. The peer's MAX_CONCURRENT_STREAMS lowers it. |
| `H2_BLOCK_MOST` | 256 KiB | The reassembler's room: the hard cap on one compressed field block, past which the connection ends (ENHANCE_YOUR_CALM). Four times the head limit below, so a head merely too large is the stream's problem and only an abusive one is the connection's. |
| `H2_HEAD_MOST` | 64 KiB | The decoded head a stream keeps (`YONDER_FETCH_HEAD_MAX`'s figure, now libfetch's for h2). Past it the block is still decoded, the stream is reset with ENHANCE_YOUR_CALM, and its fetch ends UNSUPPORTED (§ 2.7). RFC 9113 § 10.5.1 asks only that the block be processed, which it is. |

A connection costs about 400 KiB before any body arrives: the decoder's
table and scratch at a 65,536-byte advertised size (the presets'), the
encoder's at 4,096 (its ceiling here, whatever a peer allows; § 2.9 does
not match the encoder's choices), the reassembler's room, and the reader's
1 MiB of stack address space, of which it touches a few pages. Body
buffers grow on demand to what is unread, and the CONNECTION window bounds
their sum: at Chrome's 15 MiB, sixteen connections whose every server
floods and whose every fetch has stopped reading hold 240 MiB, and a
connection whose fetches are reading holds a frame or two. (Q2, ruled:
the windows are the preset's.)

**Two objects, one lock, one count.**

`h2_conn_t` holds: the engine and the socket; the out queue (a list of
16 KiB blocks, its byte counts for data and for control, the `draining`
flag, and when its head was first refused); the stream table; the next
stream id; what the peer set (max streams, initial window, max frame
size, header table size) and whether it has acknowledged ours; the two
connection windows; the HPACK encoder; `dying` with its reason, `goaway`
with the last stream id, `quiet` (§ 2.5's silent connection: no new
stream); a count of frames received; and `holds`.

`h2_stream_t` holds: its id; its fetch's bell; the final head as decoded
bytes, or that it was refused; the body buffer; the two stream windows
and the bytes consumed since the last WINDOW_UPDATE; whether each side
has ended; and how it ended, when it did not end well (the peer's reset
and its code, the connection's death and its reason, a GOAWAY that never
processed it, a head we refused).

- **The connection lock** guards all of the above. It is held for engine
  calls and bookkeeping and never across a syscall that can block. A bell
  is rung under it, which is what lets a fetch withdraw its bell under it
  (§ 2.4).
- **What the reader owns alone, with no lock:** its read buffer, the
  frame it is assembling, the reassembler, and the HPACK DECODER. Only
  the reader decodes, so a block is decoded outside the lock into the
  reader's own list, and the lock is taken to hand the result to a
  stream. The lock is therefore held for a delivery and not for a decode.
- **Lock order: the pool's, then a connection's.** Never the pool's while
  holding a connection's. The reader takes the pool's lock for the idle
  verdict and to tell the pool of a death or of room; it holds no
  connection lock when it does.
- **`holds` is the object's life**: one for the pool's table while the
  connection is listed, one for the reader while it runs, one for each
  stream. It is counted atomically and whoever takes it to zero frees the
  engine, the queue and the object. A stream is freed by its fetch and by
  nobody else: the reader marks a stream ended and rings; it never frees
  one. So a fetch's stream pointer is good until that fetch closes it,
  and the connection under it is good for as long.

**The reader, pass by pass.**

1. Read the socket for at most a slice, with no lock held.
2. With bytes: take the lock, feed the engine, move plaintext to the
   reader's buffer and whatever ciphertext the engine now owes (a
   KeyUpdate reply, an alert) to the out queue, let the lock go.
3. Cut whole frames from the buffer. `h2_frame_header_check` judges a
   header before its payload is waited for. Each frame goes through
   `h2_frame_parse` and the reassembler; a complete block through the
   decoder; then the lock is taken and the frame is delivered (below).
4. Drain the out queue with no patience (§ 2.3), if nobody is draining.
5. With or without bytes, look around: is the connection dying; has the
   queue's head been refused for longer than the write patience; has the
   connection idled out (the pool's verdict, under the pool's lock).
6. Dying, and nobody draining: close the socket, end every stream that
   has not ended, tell the pool, drop the reader's hold, and exit.

A read of zero bytes is the peer's FIN and an error is a transport
failure: both are step 6 with their reason.

**What each frame does on delivery.** A stream id we never opened, or an
even one, is the connection's PROTOCOL_ERROR wherever a stream is needed.
"Closed" below is a stream we opened that has since been closed or reset.

| Frame | On an open stream | On a closed stream |
|---|---|---|
| DATA | Charged to both receive windows by its whole payload; past either window is FLOW_CONTROL_ERROR (the stream's, or the connection's). The body bytes are appended and the bell rung; padding is credited at once, having no reader. END_STREAM ends the peer's side. The body's bytes are counted against the head's `content-length`, when it had one (below). | Charged to the connection's window and credited straight back (§ 2.6). |
| HEADERS | Judged by `h2_head_check`. A 1xx head is dropped and the wait goes on. The final head is kept and the bell rung. A second head is trailers: it must end the stream, and its fields are dropped. A head that breaks a rule resets the stream (PROTOCOL_ERROR) and ends its fetch BAD_HEAD; one past `H2_HEAD_MOST` resets it (ENHANCE_YOUR_CALM), UNSUPPORTED. | Decoded, since the table must see it, and dropped. |
| RST_STREAM | Before the peer's END_STREAM: the stream ends with the peer's code, and the bell is rung. AFTER it: the response is whole and stays whole, whatever the code; the reset only stops our sending (below). | Ignored. |
| WINDOW_UPDATE | Added to the stream's send window (or the connection's, on stream 0) and every sender waiting on it is rung. Past 2^31 - 1 is FLOW_CONTROL_ERROR, the stream's or the connection's. | Ignored. |
| SETTINGS | Each value judged by `h2_setting_check`, then applied: the header table size to the encoder; the initial window as a difference to every stream's send window, which may go negative and may not pass 2^31 - 1; max frame size and max streams as they are. Then our acknowledgement is queued. The peer's acknowledgement of ours tells the decoder (`h2_hpack_decoder_settings_acked`). | |
| PING | An acknowledgement is queued; an acknowledgement received is dropped. | |
| GOAWAY | No new stream. Streams above its last id end as never processed (§ 2.5: retried whatever their method). With an error code it is the connection's death. | |
| PUSH_PROMISE | The connection's PROTOCOL_ERROR (§ 2.10). | |
| PRIORITY, unknown | Dropped. | |

**A response's length is checked, and its end is not always a failure.**

- **`content-length` against DATA** (RFC 9113 § 8.1.1). A stream whose
  final head carried a `content-length` keeps that number and the count
  of body bytes delivered, padding left out. A DATA frame that takes the
  count past it, or an END_STREAM that arrives short of it (on DATA, or
  on the final HEADERS itself), is a malformed response: the stream is
  reset with PROTOCOL_ERROR and ends as a truncated body does on h1,
  BROKE, and never as a successful end. Exempt, because their
  content-length describes content they do not carry: a stream opened
  `bodiless`, and a 304. `h2_head_check` judges only that the number
  parses; the count is the stream's.
- **A reset after a whole response stops the upload and nothing else**
  (RFC 9113 § 8.1). A server may answer before it has read the request,
  and then send RST_STREAM(NO_ERROR) to stop the rest of it. When a reset
  arrives on a stream whose peer side has ended, the head and the body
  already delivered are kept and are read to their successful end;
  `h2_stream_send` answers "stop", so the fetch sends no more
  (`sent_whole` stays false) and goes to read its response; and no reset
  of our own is sent for it at close. That holds in whatever order the
  fetch gets to them: the END_STREAM and the reset may both have arrived
  before it reads a byte. The code is kept for the detail.

The server's first frame must be SETTINGS (RFC 9113 § 3.4); anything else
is PROTOCOL_ERROR. A connection error we raise queues GOAWAY with the
code, sets `dying`, and is the death of step 6.

**The out queue has a ceiling, and here is the sum.** Three kinds of
bytes enter it. What can wait (DATA, a new stream's HEADERS) parks at
`H2_OUT_MARK`, so it adds at most the mark and one sender's frame beyond
it: a block of at most `H2_HEAD_MOST`, or one DATA frame. What a PEER
makes us owe is counted and ends the connection at `H2_OUT_CONTROL_MOST`.
What a stream's own life owes without waiting is bounded by the streams
that were let in: one RST_STREAM at its close, and credits that follow
what was consumed, of which a stream has one outstanding for itself and
the connection one for all (a credit not yet in the queue is a number
that grows, not a second frame). So a stalled socket holds at most the
mark, a head, the control ceiling, and a reset for each stream admitted
since it stalled, and no more streams are admitted past the mark. A
thread that opens and cancels streams against a stalled socket fills the
queue to the mark and then parks at the door.

**What a fetch's thread calls** (H2.6's fetch.c; the harness now). Every
wait takes the pool's wait shape from H2.2 (a bell, a patience, a
cancellation predicate) and looks up as that does, so a cancellation is
noticed within `FETCH_POOL_LOOK_MS`.

- `h2_conn_adopt(engine, socket, unread, greeting, hooks)`: queues the
  preface, the greeting's SETTINGS and its WINDOW_UPDATE, starts the
  reader, and returns the connection holding the pool's hold. The
  greeting is a parameter (H2.5 supplies each preset's); `hooks` are the
  pool's three calls: the idle verdict, "this connection has room", "this
  connection is dead".
- `h2_conn_room(conn)`: not dying, no GOAWAY, not quiet, and streams under
  the lesser of the peer's limit and `H2_STREAMS_MOST`.
- `h2_stream_open(conn, fields, count, ends, bodiless, wait, &stream)`:
  parks while the queue is past `H2_OUT_MARK`; then, under the lock,
  takes the next id, encodes, writes HEADERS, queues; then drains.
  Refused when the connection has no room, with which reason, and when
  the fields encode past `H2_HEAD_MOST`. `bodiless` says the reply
  carries no content whatever its content-length says (a HEAD).
- `h2_stream_send(stream, bytes, len, ends, wait)`: DATA, cut to the
  peer's frame size and to both send windows; parks on the bell for
  window, and for the queue when it is past `H2_OUT_MARK`. It answers
  "stop" when the peer has ended the stream under it (below).
- `h2_stream_head(stream, wait, emit, ctx)`: waits for the final head and
  hands its fields over in wire order.
- `h2_stream_read(stream, buffer, room, wait)`: bytes; 0 at the peer's
  END_STREAM; negative when the stream ended otherwise or the wait did.
  After consuming, it sends the stream's WINDOW_UPDATE at half the stream
  window and the connection's at half of its own.
- `h2_stream_close(stream)`: takes the bell out under the lock; if either
  side had not ended and the peer has not reset it, queues
  RST_STREAM(CANCEL) and drains with no patience; credits what is unread
  to the connection; frees the stream and drops its hold. Never waits.
- `h2_stream_ended(stream)`: how it ended, the error code and whose it
  was, for the detail and the reason sentence (§ 2.8).
- `h2_conn_condemn(conn)`, `h2_conn_release(conn)`: the pool's, for
  eviction and for dropping its hold.

A wait that runs out of patience ends SILENT; if no frame of any kind
reached the connection while it waited, the connection is marked `quiet`
(§ 2.5).

**How `fetch_pool_drain` waits for the readers** (§ 2.3 left it here):
it condemns each h2 connection and parks on a bell that the "dead" hook
rings, under the pool's lock, as each reader leaves; it has waited for
them all within a slice, or within the write patience when a drainer is
blocked. Only harnesses drain. The hook is this slice's; the pool's side
of it is H2.6's.

**Not in this slice, and where each is.** SETTINGS_TIMEOUT (we never end
a connection because a peer was slow to acknowledge; a DEBTS row with
H2.5's). A GOAWAY of our own on an idle close (the socket is closed; a
row). Everything of § 2.10.

**Proof.** `tools/test_h2_conn_host.sh`, the pool harness's shape: ASan
and UBSan, then TSan, real threads. The far end is `tools/h2_peer.c`, a
scripted server over an in-memory socket whose reads can be made to stall:

- **The engine stand-in numbers its records.** Each plaintext write
  becomes a record with a sequence number, and the peer refuses a record
  out of order, repeated or cut. That is what makes the out queue's one
  rule (records reach the wire whole and in the engine's order)
  something a test can fail.
- **Cases**, each observing the peer's side and the fetch's: the
  greeting byte for byte; two streams interleaved; a stream reset
  mid-body; GOAWAY above and below a stream; a window exhausted and
  refilled, the stream's and the connection's; an initial-window change
  that takes a stream's send window negative; a connection dying under
  three streams; cancellation of one of three; a head over the limit; a
  block over the hard cap; a PUSH_PROMISE; a frame-size violation; a
  first frame that is not SETTINGS; interim heads and trailers; DATA and
  HEADERS for a closed stream, credited and decoded; padding credited;
  enough cancelled streams to spend the connection window were their
  bytes not credited; a body short of its content-length, one past it,
  and a final HEADERS that ends a stream promised ten bytes, each a
  reset and none a clean end, with a HEAD's and a 304's content-length
  left alone; a whole response followed by RST_STREAM(NO_ERROR) while
  the request body is still going, the response read to its end and the
  upload stopped, with both frames arriving before the fetch reads
  either; a drainer stalled while another thread opens and cancels
  streams by the thousand, the queue never past its ceiling and the
  thread parked at the door; a socket that stops taking bytes (two senders
  queued behind it, the reader still delivering, then death at the write
  patience); a cancelled fetch's reset reaching the wire after it left;
  a flood of pings at a stalled socket ending the connection; a
  connection condemned from outside ending within a slice with its
  socket closed once and by the reader; ciphertext handed over at
  adoption; ciphertext the engine owes after a feed; a stream going
  silent on a silent connection marking it quiet, and on a busy one not;
  eight threads opening, sending, reading and cancelling against a peer
  that answers at random, with every record in order at the end and
  every hold returned.
- **The harness ends itself**, as test_h2_host.c does: at its fiftieth
  failed check and after its alarm.
- libtls's transport harness gains the release verb's cases: refused
  mid-handshake and while closing, unread ciphertext handed back whole,
  the wrapper intact after a refusal.
- Mutants once at the end, through `tools/mutants_parallel.py`.
- No QEMU: nothing here is reachable in the guest. The first guest proof
  is H2.6's probe.

#### H2.4, as built (2026-11-10, Fable)

`userland/libfetch/h2_conn.c/.h`, compiled into libfetch.so with the
library's hidden visibility, and one new verb in libtls,
`os64_tls_transport_release`. Nothing calls the connection: no program's
behaviour changes with this slice, and H2.6 is what joins it to a fetch.
Where the build departs from the brief, or says more than it did:

- **The release verb does not flush.** `os64_tls_transport_release` hands
  over the engine, the socket and up to `OS64_TLS_TRANSPORT_UNREAD_MAX`
  bytes of ciphertext that arrived with the handshake, and frees the
  wrapper without closing anything. While the engine still owes
  ciphertext it answers NEED_PROGRESS and the caller steps the transport
  as it would for any other verb; the brief had the verb send what was
  owed. A verb that writes to a socket needs a deadline and a
  cancellation, and the transport's step already has both.
- **The control ceiling is on the whole queue.** A frame the peer makes
  us owe (an acknowledgement, a reset of its stream) ends the connection
  with ENHANCE_YOUR_CALM when the queue already holds more than
  `H2_OUT_MARK + H2_OUT_CONTROL_MOST`. The brief counted control bytes
  apart from DATA; one count is one rule, and DATA and new streams stop
  at the mark, so everything above it is control by construction.
- **The reader has a last word.** Before it closes a dying connection's
  socket it offers the queue once more with no patience
  (`drain_as(c, false, true)`). Without it a GOAWAY of ours, queued by
  the same call that condemned the connection, never reached the wire:
  every drainer stops at `dying`. It waits for a drainer that is inside
  a write to come back first, so the socket is never closed under one.
- **A condemned connection sends nothing more.** `emit` refuses once the
  connection is dying, which makes a fetch that leaves its stream
  afterwards owe the server no reset; the socket's close is the news.
- **`h2_conn_queued`** says how many bytes wait in the out queue. The
  brief did not list it; the harness needs it to see the mark hold, and
  H2.6's probe will print it.
- **`H2_TOO_LARGE` is judged on the encoder's bound**, not on the encoded
  size: `h2_hpack_encode_bound` over the request's fields against
  `H2_HEAD_MOST`, before any table changes. A request refused this way
  has touched nothing. The bound is the most the fields can encode to
  with any table, so a request the table would have shortened under the
  limit is refused all the same: the limit is on what is asked, and does
  not depend on what the connection carried before.
- **Our own frames are cut at 16,384 bytes** whatever the peer allows:
  the smallest frame is always legal, and a larger one buys nothing on a
  connection that interleaves.
- **A greeting that asks for a max frame size or a table over 1 MiB is
  not adopted.** Both are memory this connection allocates at adoption.
- **The first frame must be the server's SETTINGS** (RFC 9113 § 3.4), and
  an acknowledgement does not count. Anything else is PROTOCOL_ERROR.
- **DATA for a stream that has ended** (our reset, or its own
  END_STREAM) is dropped and its bytes credited to the connection at
  once; DATA for a stream we never opened is the connection's error.
- **A reset after a whole response holds for any code.** The brief wrote
  the rule for NO_ERROR (RFC 9113 § 8.1); a server that says CANCEL or
  REFUSED_STREAM after its END_STREAM has still sent a whole response.
  The response stands, `reset_after_end` and `reset_code` say why the
  upload stopped, and a send answers `H2_STOP`.
- **A head that ends its stream short of its content-length is
  delivered**, and the stream then fails at the first read
  (`H2_END_BAD_LENGTH`). The head did arrive; it is the body that is not
  what it promised, and a fetch's sentence can say so.
- **The server's close does not cost the frames that came with it.**
  When the engine reports the peer's close_notify, the reader reads and
  cuts the plaintext that preceded it and only then ends the connection:
  a server that sends its last DATA and closes, in one segment, has sent
  a whole response.
- **A send stops (`H2_STOP`) when the response is whole and the
  connection is ending**, reset or no reset. A server that answers an
  upload early and then closes sends no RST_STREAM; the answer stands and
  the rest of the request has nowhere to go.
- **`H2_END_BAD_FRAME`** is an ending of its own for a stream we reset:
  a frame for it broke a rule RFC 9113 makes the stream's (a
  WINDOW_UPDATE of nothing), with the code in `code`. `H2_END_FLOW` is
  the windows alone.
- **A stream's fault inside an open field block is the connection's.**
  RFC 9113 § 6.10 lets nothing but a CONTINUATION follow an open block,
  and that outranks what else is wrong with the frame.
- **A server's `ENABLE_PUSH = 1` is PROTOCOL_ERROR** (§ 6.5.2), judged
  here because `h2_setting_check` cannot know which end it is reading
  for.
- **The pool is asked about idleness only for a connection with no
  stream**, and its yes is checked again under the lock: a stream opened
  between the question and the answer keeps the connection.
- **A wait whose patience is under `H2_LOOK_MS` never looks up.** It is
  woken by what it waits for or it ends at its patience, which is what
  lets the harness prove each wake (below).

Proof. `tools/test_h2_conn_host.sh` builds the connection against
`tools/h2_peer.c` twice, under ASan/UBSan and under TSan: about 6,100
checks in nine groups of cases. The peer's stand-in for the TLS engine
numbers its records and refuses a second thread inside it, so the out
queue's order and the one-lock rule are things a case can fail; its
clock is the host's plus what a case skips, so thirty seconds of write
patience take no time. `tools/test_h2_conn_mutants.py` is 121 deliberate
breaks, one rule each. The first run through `tools/mutants_parallel.py`,
of the 102 written before the cold read, caught 82; the other twenty
were eight that did not compile as written and twelve the harness
missed, and each of the twelve got a case:

- three wakes (DATA, a head, and a window given by SETTINGS each ring the
  fetch that waits) had been hidden by the look every second, and are
  proved with a patience too short to look;
- a wait rung after its patience was spent;
- a body longer than both windows, which fails unless what was credited
  became window again;
- a refusal remembered after the socket took the bytes;
- the reader closing under a drainer; the pool asked about a connection
  with a stream; a reset sent after a condemnation; a reset for a stream
  both sides had finished;
- a head for a stream never opened, and a head over the limit by the sum
  of many small fields.

One cold read by Opus followed. It found five defects, each fixed with
a case and a mutant of its own: the four entries above that begin "The
server's close", "A send stops", "A stream's fault" and "A server's
`ENABLE_PUSH`", and a quiet connection that did not end when its last
stream left, which § 2.5 already asked for. Then the mutants once more:
109 of 109 caught.

Codex's first round (#538) found five more, each fixed with a case and
at least one mutant, all caught:

- **Window given back counts against the queue's ceiling.** A
  WINDOW_UPDATE is a frame the peer makes us owe like any other: padded
  frames at a stream with a small window, sent to a socket that takes
  nothing, grew the queue past `H2_OUT_MARK + H2_OUT_CONTROL_MOST`.
- **Plaintext the engine already holds at adoption is read at once.**
  The reader's first act is to take in what came with the handshake,
  ciphertext or not: a server's SETTINGS decrypted with its Finished
  would otherwise wait for the next record.
- **The decoder's scratch is a head, not a table.** One literal field
  passes through it, and a field may be as large as `H2_HEAD_MOST`
  whatever SETTINGS_HEADER_TABLE_SIZE is. With the scratch at the
  table's 4,096 bytes a 5,000-byte cookie ended its stream.
- **A change to the initial window is applied to streams that still
  send.** One that has sent its last, or was reset, keeps its slot until
  its fetch leaves and has no window to overflow.
- **Two content-lengths that differ** are a malformed head (RFC 9110
  § 8.6), as they are for HTTP/1.1 in `http.c`; two that agree are one.

Its second round found four:

- **What the engine owes of its own accord has the ceiling too.** A
  KeyUpdate's answer is ciphertext no frame of ours asked for; a peer
  that asks for them at a socket that takes nothing is ended at the same
  `H2_OUT_MARK + H2_OUT_CONTROL_MOST`.
- **Content where none may be resets the stream** (`H2_END_BAD_LENGTH`):
  a HEAD's response, a 204 and a 304 (RFC 9110 § 6.4.1). `bodiless` had
  only excused the content-length; `no_content`, decided at the final
  head, also refuses DATA that carries a byte. An empty DATA frame with
  END_STREAM is how such a stream is meant to end.
- **A receive buffer read empty is freed once it has grown past
  64 KiB.** The window its bytes held goes back to the peer when they
  are read, so a buffer kept at its high-water mark is memory held twice
  over: a server filling each of 64 streams in turn could hold the
  stream window 64 times against one connection window.
  `h2_conn_buffered` says what the buffers are allocated to hold.
- **A signal does not start the write patience again.** A patient write
  that is interrupted having moved nothing stamps the refusal as a
  timeout does, so the thirty seconds are counted from the first attempt
  that moved nothing.
No QEMU and no P5 pass: nothing here is reachable in the guest.

### H2.6 — Fetches on the connection (brief when H2.4 has landed)

Split from H2.4 by Q4's ruling. fetch.c's `acquire` gains § 2.2's h2 clauses (a
live connection with room first; the remembered-h2 table; a dial in
flight waited for); the pool's `fetch_conn_t` gains a connection's users
and the hooks above; the request model renders to a field list (§ 2.7,
the half that does not depend on a preset); the retry rules of § 2.5 and
the words of § 2.8; the scripted peer joins tools/test_fetch_host.c beside
the h1 one, with the cases that need a fetch (a take racing the idle
close; the next fetch dialling past a quiet connection; REFUSED_STREAM
and an unprocessed GOAWAY retried). And `/tests/h2probe`, built from
libfetch's sources so it can ask for h2 while `dial` does not: one real
h2 server fetched in QEMU, which is the first time the reader's slices,
the bell and a detached thread meet the real kernel. The brief is
Fable's, written against the tree after H2.4's round.

### H2.5 — The switch (brief when its turn comes)

Contracts: § 2.1 (the offer, and who offers), § 2.7 (one model, two
renderers; the head's new fact), § 2.8 (the words), § 2.9 (the greeting
per preset, from captures C1 and C2), § 2.10 (the rows). The brief adds:
the greeting's representation in `agent.c` and `os64_fetch_options_t`;
yonder's own greeting, written down with its reasons; what the request
record shows; the QEMU proof against a local `nghttpd` (`make run-net`,
the W2 page served over h2, the dump showing one connection carrying it,
nghttpd's `-v` log showing our greeting byte for byte against the
preset's capture); DEBTS, DIVERGENCES and BROWSER_DEBTS rows retired and
added; and Chris's P5 pass against the sites that scored us (by outcome,
never by name, WIRE.md's rule). This is the slice whose merge waits on
that pass.

## 5. Proof, across the arc

- **Host first, always.** H2.3 and most of H2.4 are pure or scripted and
  never need the guest; H2.1 runs against OpenSSL on the host; H2.2's
  concurrency runs under the sanitizers with real threads. A QEMU boot is
  for what a unit test cannot see: the wire, and the kernel's bell.
- **A scripted h2 peer, not a real server, for the connection's cases**,
  because the cases that matter are the ones a well-behaved server will
  not produce on request (WIRE.md's precedent: "the awkward cases added").
  The real server (`nghttpd`) is for the switch, where interoperability
  is the question.
- **Mutants once, at the end of each slice**, through
  tools/mutants_parallel.py, twelve workers, re-run only for lines a
  review round changed.
- **The measurement that justifies the arc** is WIRE.md § 2's table with
  a third column — connections per page and `arrived after` — on the
  local page, the blog page and the course page, before H2.1, after H2.1,
  after H2.2 and after H2.5, each a fresh VM under the Chrome preset. The
  expectation: connections to one or two per host and handshakes with
  them; arrival time following on the pages whose wait was connections,
  and not on the one whose wait is QuickJS under TCG. The P5's storefront
  is the figure QEMU cannot stand in for, and Chris reads it.

## 6. The books

When H2.5 lands: DEBTS § libfetch loses "HTTP/2 and pipelining are not
spoken" and "Fetches to one host are not capped at once" (paid by H2.2),
DEBTS § TLS 1.3 engine boundaries loses the tickets clause (H2.1), and
gains the § 2.10 rows and one for HTTP/3; DIVERGENCES' "The browser's
identity" row changes its last cell from "HTTP/1.1, os64's own handshake"
to "HTTP/2 under the preset's greeting, os64's own handshake";
BROWSER_DEBTS loses nothing it has not already lost. WIRE.md § 2 gains a
one-line pointer here. Each slice's "as built" lands under its brief
above, by its builder, in its PR.

## 7. Questions (each is a `Q` row in the tracker; all ruled by Chris for the leans, Q1 to Q3 on 2026-11-09 and Q4 on 2026-11-10)

- **Q1 — the bell's implementation.** A kernel handle kind (§ 2.4; one
  handle per bell, a small kernel slice with a Codex round; Fable's lean),
  or a pipe-backed `os64_bell_t` in libos64 alone (no kernel change; two
  handles and 64 KiB per bell, which yonder's handle table cannot afford
  at thirteen in flight unless the pool's total drops). Same header either
  way.
- **Q2 — the receive window.** Match the preset's window values exactly
  (the fingerprint's whole point; the buffer ceiling is thirteen streams
  times the window, reached only by a server that floods a client that is
  not reading), or advertise a smaller window of our own at a known cost
  in fidelity. Fable's lean: match the preset; the ceiling is theoretical
  and `max_body` is the real bound.
- **Q3 — how a reader is stopped.** The reader parks in slices and checks
  a flag, and is the socket's only closer (§ 2.3; no kernel change, a
  condemned connection ends up to a slice late; Fable's lean), or a
  kernel verb that hangs a connection up without closing its handle
  (prompt; new kernel surface with a Codex round of its own).
- **Q4 — H2.4 as one slice or two.** The connection built and proved
  alone, then joined to fetches in a second slice (H2.6; two PRs, two
  Codex rounds, each reading one kind of problem: the connection's own
  concurrency, then its meeting with the pool and the retry rules;
  Fable's lean), or one slice as § 3 first had it (one PR and one round,
  on what would be the largest diff of the arc, with the pool's changes
  and the connection's reviewed together). RULED by Chris on 2026-11-10:
  two slices. Quinn's read of the brief recommended the same.
