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

**What today costs.** libfetch speaks HTTP/1.1 with keep-alive and a pool
of kept connections (WIRE.md § 2): six idle per key, sixteen in all, sixty
seconds idle. Concurrency is not capped by the pool — a host is dialled by
as many fetches as are asking at once, which is the caller's thread count
(yonder: its twelve workers and a synchronous XHR), and a page that asks
for forty resources dials more than that over its life, because only six
idle connections per key are kept between requests (DEBTS § libfetch,
"Fetches to one host are not capped at once"; WIRE.md § 2's table counts
21 connections for a page of 26 resources). Each one pays a TCP handshake and a
WHOLE TLS 1.3 handshake, because session tickets are consumed and
discarded (TLS13.md § The profile; DEBTS § TLS 1.3 engine boundaries). On
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
it is the cheapest cut of the same handshake cost and stands alone; the
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
  its cancellation predicate like every wait in libfetch.
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
  - **The queue is bounded for DATA alone.** A sender with DATA to send
    while the queue is past its mark (the brief names it) parks on its
    bell until the drainer brings it under. HEADERS and the small frames
    are queued regardless: they are bounded by the streams that exist.
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
| **H2.4** The connection | State machine, streams, flow control, the reader thread (§ 2.3, § 2.5, § 2.6), against a scripted peer | No (ALPN still offers h1 alone) | H2.0, H2.2, H2.3 | Codex (concurrency) |
| **H2.5** The switch | ALPN offers h2; the request and reply models (§ 2.7); the greeting per preset (§ 2.9); captures; QEMU and P5 proof | YES — this is the arc | H2.4, C1, C2 | Codex, then Chris on the P5 |

H2.1 and H2.3 touch nothing the others touch and can run beside anything.
H2.0 goes before H2.2 because the cap's wait parks on a bell. Only H2.5 changes what the wire sees, so only H2.5 needs
Chris's P5 pass before merge; H2.1 and H2.2 change the wire's SHAPE (fewer
handshakes, fewer connections) without changing a byte of a request, and
get the QEMU `make run-net` page load every wire-facing slice gets.

Briefs for H2.0 through H2.3 follow in full. H2.4 and H2.5 have their
contracts above and get their briefs when their turn comes, written
against the tree as it stands then — H2.4's shape depends on what H2.2
became after its round, and H2.5's on the captures. Writing them now
would be writing a comment that goes stale before it is read.

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
  trust. **Not measured:** WIRE.md's blog page before and after, which the
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

**fetch.c.** `take_kept`/`dial`/`release_connection` speak to connections
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

### H2.4 — The connection (brief when its turn comes)

Contracts: § 2.2 (which connection a fetch takes, now with the h2
clauses; the remembered-h2 table; death fanned out), § 2.3 (the reader
thread and its slices; the engine under the lock; the out queue and its
drainer; `dying`, and the reader as the socket's only closer), § 2.5 (every way a
stream and a connection end), § 2.6 (windows and buffers). The brief adds:
the connection object's fields and locks named; how the reader hands a
frame to a stream (the stream's buffer and its bell); the settings
exchange (ours sent in the preface, the peer's applied: max concurrent
streams, initial window, max frame size, header table size — and
`ENABLE_PUSH` ignored since we never receive pushes we did not forbid);
the scripted h2 peer in tools/test_fetch_host.c (frames as bytes, released
request by request as `peer_answers` releases h1 replies today) and its
cases — two streams interleaved, a stream reset mid-body, GOAWAY above and
below a stream, a window exhausted and refilled, a connection dying under
three streams, cancellation of one of three, a head over the limit, a
PUSH_PROMISE, a frame-size violation, the idle close, a take racing the
idle close, a connection condemned from outside ending within a slice
with its socket closed once and by the reader, a socket that stops
taking bytes (two senders queued behind it, the reader still delivering
frames, then death at the write patience), a cancelled fetch's reset
reaching the wire after it left, enough cancelled streams to spend the
connection window were their bytes not credited, a stream going silent
on a silent connection and the next fetch dialling instead of joining it
— under the
sanitizers with real threads. Still unreachable: `dial` offers `http/1.1`
alone until H2.5. The brief is written against the tree after H2.2's
round, by Fable.

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

## 7. Questions (each is a `Q` row in the tracker; all ruled by Chris on 2026-11-09, for the leans)

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
