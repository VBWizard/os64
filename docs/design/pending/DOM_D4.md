# DOM_D4.md — the stream: yonder parses on the window's thread

*Written 2026-10-05 by Fable, re-derived from [DOM.md](DOM.md) § Where
script runs after the first draft was lost unpushed on 2026-10-04. This is
slice D4 of DOM.md § Slices: "The stream: yonder parses on its own thread.
No script." Read against the tree at `29641e20` (D6 merged as #217). Names
of functions that do not exist yet are working names; the code settles
their spelling, and this file's "as built" section goes into DOM.md with
the PR.*

## What D4 is, and is not

Today a navigation is one job on the pool: the worker runs `way_load`,
which fetches, parses and models the page, and hands the window a finished
`way_page_t`. DOM.md ruling 5 says the document never crosses a thread,
because the thread that runs a page's scripts must be the thread that owns
the parser (a script stopped at mid-parse reads and writes the tree, and
`document.write` re-enters the parser). So the parser moves to the window.

After D4 the worker fetches and the window parses. The worker opens the
fetch, judges the head exactly as `way_load` does, and posts the head, the
body in chunks, and the fetch's verdict through the navigation's mailbox,
waiting when the window has not caught up. The window makes the page's
parser from the head and feeds it a slice at a time, going back to its
event loop between slices, and when the verdict is in it finishes the
parse, builds the model and arrives exactly as a page arrives today.

**What D4 does not do**, each deliberately:

- **No script runs.** A script's end tag stops the parser and the stream
  resumes it at once, as D5b's loader does today. D7 replaces that resume
  with a run. D4 is what makes D7 possible, and nothing more.
- **The old page stays on screen until the new one is whole.** Nothing is
  drawn from a half-parsed tree. Progressive display is a different slice
  with a different proof (what a reader sees while a page arrives), and
  DOM.md § What the old page does meanwhile already says the old page
  stays live.
- **Linked sheets are still discovered when the parse ends.** Starting
  them during the parse is a real saving on every page with a `<link
  rel=stylesheet>`, and D7 needs it ("a script waits for the sheets named
  before it"). It is booked below with that trigger, not smuggled in here.
- **wend does not change.** `way_load` stays, as the same pieces driven to
  completion on one thread.

## The seam in libway: one implementation of every judgement

`way_load` becomes four pieces and itself. The pieces are public because
yonder's worker and window call them from two threads; `way_load` is the
pieces in a row, so wend and the real-fetch harness keep the function they
have and prove the pieces by proving it.

| Piece | What it does | Thread |
|---|---|---|
| `way_open(leg, url, request, &opening, why)` | opens the fetch with the leg's options (agent, accept, the parser's byte cap as `max_body`, the downgrade question, cookies, Referer, a POST's body), reads the head, and JUDGES it: a page (`WAY_BODY_HTML`), text (`WAY_BODY_TEXT`), or not a page at all. The last is false with the sentence `way_load` writes today ("that is image/png, not a page - save it with: os64get ..."), fetch closed. A head that never came is false with `why`. True leaves the fetch open in the opening, with the head COPIED beside it (`way_head_t`: status, reason, content type, charset, final address, whether the final method was POST) | the fetcher's |
| `way_read(leg, &opening, buf, cap)` | one `os64_fetch_read`, and the "reading N KB of url" sentence every 64 KB through the leg's face. `parse_body` and `read_body` each carry a copy of that sentence today; this is the one copy | the fetcher's |
| `way_text_utf8(head, first, n)` | the text/plain encoding decision — the label through libhtml's table, else JSON's own rule, else a byte order mark in the first bytes — pure, so the window can make it at the first chunk | either |
| `way_note(page, head, short_of_memory, fetch, reason)` | writes `page->url`, `page->posted` and the standing line: status, reason, and a sentence for every way the page is incomplete (ours, the wire's, the parser's `refusal` read from `page->doc`). Pure. The fetch's reason sentence is a string argument because `os64_fetch_reason` lives inside the fetch object, which is on the other thread | either |

The head is copied rather than pointed at because `os64_fetch_head_t` is
storage inside the fetch, valid until close, and the fetch is closed on
the worker while the window is still composing the note from the head.

What `way_load` does with them: `way_open`; HTML through the parser loop
with `way_read` (feed, resume past every script stop, finish); text through
`read_body` with `way_read` and `way_text_utf8`; `way_note`; close. Same
calls in the same order as today, so the real-fetch harness's
`scripted_document_load` and every other case answer as they do now. That
harness gains a case that drives the pieces by hand against the same
scripted peer and checks the page it gets is the page `way_load` gets:
same serialised tree, same note, same text, same sentences on the two
refusals.

## The stream rides the mailbox

The mailbox is already the thing that outlives whichever side lets go
last, carries its navigation's generation, and has a pipe the worker waits
on in cancellable steps. DOM.md asked for "a bounded queue in the mailbox",
and that is where it goes. `yonder_mail_t` grows three things:

- **The head**, posted once, `way_head_t` plus the body kind.
- **The ring**: `STREAM_CHUNKS` slots of `STREAM_CHUNK` bytes, fixed
  storage allocated with the mailbox (16 slots of 16 KiB, 256 KiB per
  navigation; two mailboxes exist for the moment one navigation replaces
  another). Fixed, so posting never allocates and never fails for memory:
  the only way a post ends without posting is cancellation.
- **The verdict**, posted once, last: the fetch's status and its reason
  sentence, and whether there was a page at all. An opening that failed, or
  a head that was not a page, posts a verdict with no head before it and
  the sentence to show.

**Posting blocks the worker when the ring is full**, which is backpressure
to the wire: the worker stops reading the socket, TCP's window closes, the
server waits. That is the right place for the pressure, since a window
that parses slower than a LAN delivers must slow the LAN and not grow a
buffer. The wait is the mailbox's existing shape: `os64_read_for` on the
answer pipe in 100 ms steps, asking the pool's predicate between them, so
Stop, a new navigation, the close box and the pool's own failures all end
it. The window wakes it early. When it takes from a ring the worker is
waiting on, it writes a note down the same pipe the answers travel —
numbered 0, which no question is — and the worker reads notes and stale
answers alike while it waits, acting only on room. The worker sets a
`waiting` flag under the lock before it sleeps and clears it when it
wakes, and the window writes a note only when the flag is set, so notes
are bounded to one per wait and a pipe write can never block the window.
(Without the flag a worker and window in lockstep would leave a note per
chunk unread in the pipe until it filled.) One pipe, not two: a task has
64 handles and this is the browser.

**Taking never blocks.** The window takes the head if it is new, then one
chunk at a time into a buffer of its own and feeds it; it never holds the
mailbox's lock across a parse. Progress sentences and questions keep
working exactly as they do: a downgrade hop asks before any chunk exists.

**Generations are unchanged.** The mailbox carries its navigation's
generation and the window reads only the mailbox it holds; a late chunk
from a replaced navigation is never read because that mailbox is never
read again, and its worker's post ends at the cancel.

## The window's side: the stream and its slice

A new piece of window state, `g.stream`, beside `g.nav` (the pool job: its
id, nothing else now) and `g.coming` (a page waiting for its sheets):

- the window's reference to the mailbox, the generation, and what arriving
  needs (`kind`, `crumb`, `fragment`) — these move from `g.nav`, because
  the parse outlives the job: the worker returns the moment its last
  post lands, and the window drains at its own pace;
- the head once seen, the parser, the parser mode captured when the
  navigation started, bytes fed so far, and the verdict once seen;
- for a form being sent, the window's OWN copy of the request
  (`request_copy`, which Reload's resend already uses). Today the request
  is moved into the job and moved back at the reap, which only works
  because arrival IS the reap. Now arrival is the verdict, which may land
  before or after the reap, and a parse the window cuts short (below)
  cancels the job, whose release frees the request. A copy costs a form
  body, kilobytes, and decouples the page from the pool.

**`stream_turn`**, once per turn of the loop beside `script_turn`:

1. A new head makes the parser. HTML: libhtml's options with the head's
   charset and the captured scripting mode, as `parse_body` does. Text: the
   same parser given `<!doctype html><plaintext>` first, the way
   `page_from_text` does today, with the encoding decided by
   `way_text_utf8` at the first chunk (the byte order mark needs three
   bytes of body, and the first chunk has them or the body is shorter than
   three bytes).
2. Chunks are taken and fed until the ring is empty or
   `STREAM_SLICE_BYTES` have been fed this turn (64 KiB to start: four
   chunks). A stop at a script is resumed at once. Chunk boundaries are
   invisible to the parser by contract (THE PARSE THAT STOPS), so the
   document is the one `way_load` builds from the same bytes.
3. **A parser that refuses ends the stream early.** `way_load` stops
   reading at the first refusal and closes the fetch, because a page too
   big or too deep to parse is a page you can read the beginning of and
   there is no point pulling the rest off the wire. The stream does the
   same: the window cancels the job — the worker's next cancel check ends
   its read, and the pool releases the job — and finishes with what it
   has. Its note then carries the parser's sentence and no fetch sentence,
   which is what `way_load` writes in the same case (a fetch stopped by
   its reader has no verdict of its own).
4. When the verdict is in and the ring is drained, the parse is finished
   (`os64_html_parser_finish`, straight through, as `parse_body`), the
   model is built on the window (`os64_page_build`, as `way_load` does;
   this is the one piece of work that moves from the worker that is not
   the parse), `way_note` writes the standing line, and `arrive()` takes
   the page from there: sheets, the wait for them, layout beside the old
   page, the swap. Nothing after this point changes.
5. If chunks remain, or the verdict is in but step 4 has not run, the turn
   rings `BELL_STREAM` so the loop comes straight back after painting and
   reading its events. That is how the window stays live through a body
   that arrives faster than it parses: one slice, one look at the glass.

The slice is a byte budget rather than a time budget because the clock the
window has is the tick (10 ms) and a byte count is deterministic on the
host. The number is a starting guess and the as-built section records what
a 64 KiB slice costs in the guest; it moves with that evidence.

**`stream_drop`** destroys the parser with its document
(`os64_html_parser_destroy`: nothing points into a D4 tree — no model, no
snapshot, no runtime — so abandon, which DOM.md's teardown order uses for a
page that has those, is not yet needed; D7 switches it), drops the window's
mailbox reference and frees the request copy. It runs from `stop_trip`
(Stop, a new navigation, the Settings scripts toggle), at a verdict with no
page, when the pool breaks, and at the window's close after the pool is
destroyed (the pool joins its workers first; a worker still posting holds
its own mailbox reference, so the order is sound either way).

**Who lights Stop.** `buttons_follow` reads "loading" as `g.stream.active
|| g.coming.active`; a stream still draining after its job was reaped is
still a load. `click_stop` with a stream up ends it with "stopped".

**Two pages at once, never three.** The shown page and ONE of a stream or a
page waiting for its sheets; a stream becomes the coming page at step 4,
and `start_trip` drops both before it starts. This is the rule DOM.md
§ What the old page does meanwhile states, kept by the same door.

## The reap, and the pool's budget

The job's product is empty now; the reap for the current id clears
`g.nav.id` and refreshes the buttons, and the release frees the job (its
request, its mailbox reference). A reap for any other id is released
unread, as today.

`TRIP_RESERVE` was 80 MiB: the parser's input, the tree and the model,
because the worker built them. The worker now holds a fetch and a 16 KiB
chunk; the reserve becomes what libfetch and libtls allocate for one
connection with room to spare, and its comment says so. The tree and the
model are the window's, outside the pool's budget, which is a change in
what the budget bounds: the budget still admits pictures and sheets by
their declared cost; a page's tree is bounded by libhtml's own arena cap,
as before, just not counted twice. `POOL_BUDGET`'s comment, which names a
page's cost, is rewritten.

## What this leaves ready for D7

The parser is on the window's thread, stopped at a script's end tag with
the tree readable between calls — exactly the posture DOM.md's event loop
needs. D7's first act is to replace the stream's `resume` with "run the
script, then resume", which is a task on the page's queue; the stream's
turn is already one task per loop turn. Nothing in D4 has to be undone.

## The proof

In the house's shape (DOM_BRIEFS.md § What every DOM slice does the same
way), on the host first:

- **libway's harnesses, unchanged in result**, then grown:
  `tools/test_way_fetch_host.sh` (27 checks, the real libfetch against
  scripted peers) gains the pieces-by-hand case above and runs it with
  scripting on and off; `tools/test_way_host.sh` (the pure half) gains a
  case per sentence of `way_note` and per rule of `way_text_utf8`.
- **A new `tools/test_yonder_stream_host.sh`**, under ASan, UBSan and LSan,
  in two parts. The mailbox, with real pipes and two threads: 1,000 chunks
  through a 16-slot ring arrive whole and in order; a poster blocked on a
  full ring is woken by the room note and waits are counted so a build
  that never blocks or never wakes is caught; a stale answer in the pipe
  does not pass for room; cancellation ends a blocked post; the head and
  the verdict each arrive once; the mailbox and its pipe go with the last
  holder. The window, hosting `yonder.c` the way `test_yonder_scripts_host`
  does with the test playing the worker through the mailbox: a slice
  feeds at most its budget and asks to be rung again; the head makes the
  parser with the captured mode; a text/plain body decides its encoding
  from its first bytes; a verdict before the ring is drained defers the
  finish; a parser refusal cancels the job (the stub records it) and
  arrives with the parser's sentence; a stream dropped mid-body frees
  everything; a broken pool drops the stream; a request copy survives the
  job's release; and the arrived document serialises identically to the
  same bytes through `way_load`.
- **Mutants**, one per rule the slice adds, against the finished harness:
  the ring's wrap, the waiting flag, the note's number, the slice budget,
  the verdict-before-drain, the scripting capture, the resume loop, the
  cancel on refusal, the request copy, the generation check. The counts go
  in the as-built section.
- **Consumers unchanged**: `test_yonder_host.sh` (the painter links
  `mail.c`), `test_yonder_scripts_host.sh` (it includes `yonder.c`, so its
  stubs grow with the mailbox's surface), `test_wend_host.sh`,
  `test_libpage_host.sh`, `test_libflow_host.sh`, `test_garb_host.sh`,
  `test_html_host.sh`.

Then the guest, on a scratch copy of the image, never Chris's:

- **The Y3 walk** against `tools/httptestd.py`, its request log compared
  request for request with wend's walk of the same server, as the Y3
  evidence was gathered: links, the toolbar, a 404 shown as a page,
  text/plain drawn, a picture refused with its os64get command, a
  redirector followed and skipped by Back, a delayed refresh offered, a
  chain stopped at its sixth page, a fragment with no request, Back and
  Forward.
- **The window live through a stalled body.** `httptestd.py` gains
  `/stall-body`: a whole head and half of a declared body, then silence.
  The window scrolls and paints the page it was on while the body stalls,
  Stop ends the load at once, and left alone the idle deadline ends it
  with the page as far as it came and the sentence saying the server went
  silent. `/stall` (half a head) and `/slow.txt` (a body dribbled out)
  keep their Y3 behaviour, the second now showing the progress sentence
  from a worker that only reads.
- **A scripted page**, with Run page scripts on, still shows "Two
  JavaScript donuts!" and keeps the typed Q (D5b's fixture): the stream
  resumes past its scripts and queues them at arrival as before.
- **Measured**: the time to a laid-out Wikipedia page from the local
  server before and after; the time one 64 KiB slice takes to feed in the
  guest, which is what the slice budget is held to; and whether a LAN
  fetch slows when the ring applies backpressure.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| Linked sheets fetched as the parse goes | the model that lists them is built at the end today; building one mid-parse is D7's "the model rebuilt at a script's stop" | D7, or a page whose sheets are the long pole of its load |
| Progressive display of the arriving page | its own proof: what a reader sees, and when the old page goes | a page slow enough that a blank wait is worse than a growing one |
| A time-based slice budget | the window's clock is the 10 ms tick; `micros()` exists if bytes prove the wrong unit | a page whose bytes parse at very unequal speed |
| `os64_html_parser_abandon` at `stream_drop` | nothing points into a D4 tree, so destroy is right and simpler | D7, the first slice with a runtime over a half-parsed document |

## Decisions, and what would reverse each

1. **The stream rides the mailbox.** One object already owns the lifetime,
   the generation and the pipe. *Reversed by:* a second kind of stream (a
   script's own fetch) wanting the same queue without a navigation.
2. **A fixed ring, and backpressure to the wire.** Posting cannot fail and
   the slow side is the one that waits. *Reversed by:* a measured page
   where the ring's size, not the parse, is the long pole.
3. **The verdict rides the mailbox; the reap only clears the job.**
   Arrival is decoupled from the pool so a window can cut a parse short.
   *Reversed by:* nothing foreseen; a product-carried verdict would have to
   re-couple them.
4. **A byte budget per slice.** Deterministic, host-testable, measured in
   the guest. *Reversed by:* the row above.
5. **The window copies a POST request.** Kilobytes, for a page whose
   arrival no longer coincides with its job's end. *Reversed by:* a request
   body big enough to matter, which libpage's own caps forbid today.
