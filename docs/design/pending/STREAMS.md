# STREAMS.md — the Streams standard (`ReadableStream` and the rest)

*Opus, 2026-11-01, at Chris's choice after slice 8 (MODERN_WEB.md, the
second of the two slices the P5's pages promoted). A design pass before
code, as MODERN_WEB.md asks for this one: it is a whole standard, and the
first lean below adds a mechanism libdom does not have.*

## 1. Why, measured

- A social site's sign-in pages die on `'ReadableStream' is not defined`
  before they draw (MODERN_WEB.md); GitHub's bundles name it right after
  `MutationObserver`.
- Pages use streams three ways: they build one themselves
  (`new ReadableStream({start, pull})`, often only to hand to
  `new Response(stream)`); they read a fetch body piece by piece
  (`response.body.getReader()`, `for await (const chunk of body)`), usually
  through `pipeThrough(new TextDecoderStream())`; and frameworks feature-test
  the globals (`TransformStream`, `WritableStream`) and take a different
  path, or fail, when one is missing.
- Range is not needed: the standard rests on promises, typed arrays and the
  event loop only.

## 2. What yonder has today

libdom's `fetch.c` builds a Request's or Response's `body` as a one-chunk
stream with a default reader, in C (`DStream`, `DReader`): no `ReadableStream` global, no
`tee()`, no BYOB reader, no stream as a request body. A fetch answer
reaches libdom whole, in one `os64_dom_fetched` call (`os64_dom_response_t`
in `dom.h`). DEBTS.md books the gap and two smaller rows it causes (a
released reader's `closed` not marked handled; a clone not reached by its
original's abort, which the standard's `tee` would give).

## 3. Where the standard lives: the choice

**A. Self-hosted JavaScript (my lean).** The standard is written as
abstract operations over promises and JS objects; its reference
implementation is JavaScript, and the operations map onto JS one for one.
yonder would carry `streams.js`:
- every class and abstract operation of the standard, the internal slots as
  JS private fields (`#state`), which also gives the brand checks the IDL
  asks for;
- compiled to bytecode at build time and read into each realm at its start
  (libjs already reads and writes bytecode: `runtime.c`, patch 0015), so a
  page pays a read, not a compile;
- the INTRINSICS it needs (`Promise`, its `then`, `resolve`, `reject`,
  `Reflect.apply`, typed-array constructors, `queueMicrotask`) captured
  before any page script runs and called only through those captures, so a
  page that patches `Promise.prototype.then` or `Array.prototype.push`
  cannot reach inside a stream (WPT's `patched-global` tests check exactly
  this);
- a small C door for libdom: an internal object the JS hands back at realm
  start, through which `fetch.c` makes a body's stream, enqueues bytes,
  closes it and errors it.
- Cost: one new mechanism in libdom (engine-hosted code), and that code's
  frames appear in a page's stack traces, named `os64-streams`.
- If reading it at every realm's start costs too much (§ 7), it is read on
  first use instead, through accessors on `window`, which still answer
  `'ReadableStream' in window` truthfully. The INTRINSICS are captured at
  the realm's start either way, by C, and handed to the code when it is
  read: captured on first use, they would be whatever the page had patched
  them to by then. The JS asserts at load that every capture it uses is
  present, and the host fixture fails on a missing one, so the C list and
  the JS list cannot drift apart unseen.
- Size: a few thousand lines of JS for S1 and S2 together, to be measured
  as it is written; the standard's own reference implementation is the
  yardstick.

**B. C in libdom, as every other binding is.** Consistent with the rest of
libdom, and nothing new. But every abstract operation becomes C over the
QuickJS API — promise reactions as C closures, each chunk a `JSValue` held
and freed by hand — about three times the code. Every line would have to
be checked against the standard's prose rather than against its own
reference implementation, and the C would be the harder to keep right.

**My lean is A**, for correctness: the standard's own shape and its own
reference implementation make the code checkable line by line, and WPT
`streams/` is the oracle either way. It is a ruling for Chris, since it is
a first.

## 4. Rules

1. **A body's bytes have one owner: its stream.** The C door hands the
   bytes over when they arrive and `fetch.c` keeps none. `text()`,
   `json()`, `arrayBuffer()`, `blob()`, `formData()` and `bytes()` read
   the stream to its end through a reader, as the standard's "fully read"
   does, so `bodyUsed`, locked and disturbed are one state, in JS, and
   `new Response(stream)` takes the same path as a fetched body. (As
   built, the owner is of the state, and a Request keeps its bytes as
   Fetch's body source: § 8.)
2. **The switch.** Streams is a row in libdom's Modern Web table
   (`modern.c`), so its globals exist only with `os64_dom_options_t.modern`
   set, like `fetch` and `XMLHttpRequest`, which are rows there already.
3. **Memory.** A page's streams live in its runtime's heap, and that
   runtime's allocation limit (`os64_js_default_limits`, applied with
   `JS_SetMemoryLimit`) is their cap: an `enqueue` past it throws like any
   allocation. The limit is per page (MODERN_WEB.md's reopened decision).
4. **Diagnostics say where.** A failure whose innermost frame is in
   `os64-streams` is recorded as `FAILED (in os64-streams) ...`. A FAILED
   line is read as "the page used something yonder lacks"; a bug in
   `streams.js` must not be filed that way. The mark says where the throw
   happened, not whose fault it was: a page that hands a reader the wrong
   object is also thrown at from inside.

## 5. Cuts

1. **S1, the standard minus byte streams.**
   - `ReadableStream`: the default controller, default reader, `tee`,
     `pipeTo`, `pipeThrough`, async iteration, `ReadableStream.from`.
   - `WritableStream` and its default controller and writer.
   - `TransformStream` and its default controller.
   - `CountQueuingStrategy` and `ByteLengthQueuingStrategy`.
   - `response.body` and `request.body` become real streams (the whole body
     enqueued as one chunk). `new Response(stream)` and
     `new Request(url, {body: stream})` read a page's stream to its end.
     `fetch.c`'s C streams go, and with them the two smaller DEBTS rows.
2. **S2, byte streams.** `ReadableByteStreamController`, the BYOB reader and
   request, `type: "bytes"`; a body's stream becomes a byte stream, as the
   standard makes it.
3. **S3, bodies that stream.**
   - A new host contract: the head first (`os64_dom_fetch_head`), then
     chunks as they arrive, then the end or an error. yonder's fetch job
     sends what it has as it reads, so `response.body` grows while the page
     reads it.
   - It needs nothing new from libfetch: the job already runs on its own
     thread, where `os64_fetch_open` blocks until the head and
     `os64_fetch_read` returns the body a piece at a time (DEBTS
     "Progressive `open`" is for a loop with no thread, which this is not).
   - Chunks are delivered on the page's own thread, between tasks, in
     order, the way `os64_dom_fetched` is delivered today.
   - **Backpressure.** The job holds off its next read until the page's
     thread says the stream's queue has room (the standard's
     `desiredSize`). Without that, a page that opens a large body and never
     reads it grows the queue without bound.
   - **Caps.** A streamed body is not held by the job, so
     `YONDER_FETCH_BODY_MAX` does not cap it; the realm's allocation limit
     (rule 3) does, for whatever the page keeps. The job's cap stays for
     the bodies yonder keeps itself.
   - **XHR rides the same contract**: `progress` fires per chunk and
     `responseText` grows, as XHR says. Today it fires once, at the end of
     the body (`xhr.c`'s `end_of_body`), which is why a speed test that
     measures from `progress` reads near zero.
   - `TextEncoderStream` and `TextDecoderStream` (the Encoding standard's,
     over the decoder libdom has).

**Booked, not cut:** transferable streams (`postMessage` of a stream),
request bodies that stream (`duplex: "half"`), `CompressionStream` and
`DecompressionStream` (libgzip could serve them; a slice of their own).

## 6. Lifecycle

| Event | Streams code | A body's stream |
|---|---|---|
| a realm starts (a document's scripts) | intrinsics captured; its bytecode read (or, if lazy, at first use); globals defined; the C door handed back | — |
| a page builds a stream | the standard's operations, in JS | — |
| a fetch answer arrives (S1, S2) | — | made through the C door, its bytes enqueued whole, closed |
| a head arrives, then chunks (S3) | — | made at the head; each chunk enqueued as it comes; closed at the end, errored on a failure |
| a page calls `text()` and its siblings | a reader reads the stream to its end | disturbed and locked, in JS |
| a fetch is aborted | — | errored through the door, as the Response's abort steps say |
| the realm is short of memory | the allocation throws, as any does | errored, if it was the host's enqueue |
| the page leaves | — | its streams go with the realm; a pending host read is cancelled, as today |

## 7. Proof

- **WPT `streams/`** (testharness `.any.js`, added to `tools/wpt_fetch.sh`'s
  `WPT_DIRS`), the oracle: readable, writable, transform, piping, queuing
  strategies, and in S2 the byte streams. The byte-stream pages stay in
  the list, expected to fail until S2, never excluded. The `patched-global`
  pages check the captured intrinsics.
- **No loss in WPT `fetch/api`**, whose body tests move from the C streams
  to these, and in the DOM host suite.
- **A host fixture of a realm's start**: its cost measured in time and
  memory. With `keep_pages = on` a kept page keeps its runtime, so every
  kept page holds the cost and not only the one in front; measured with
  three kept pages. If it is too dear, the code is read on first use
  (§ 3).
- **The P5**: the social site's sign-in pages draw; GitHub's bundles get
  past `ReadableStream`.

## 8. S1, as built

`userland/libdom/streams.js` is the standard (one function expression,
handed the captures); `streams.c` is its C half (the captures, the
interfaces on `window`, the door's verbs); `fetch.c` lost its C streams.
Where the build departs from §§ 3–7, and why:

- **Compiled once per process, not at build time.** The first realm
  compiles streams.js and keeps the bytecode in a static buffer; every
  later realm reads it back (5.3 ms to compile, 0.16 ms to read, on the
  desktop's host build). A build-time compile would have needed a host
  build of the engine inside the userland build for 5 ms once a run.
- **Eager, not lazy.** A realm holds about 250 KB of it, source text
  stripped (`JS_STRIP_SOURCE`, set around this compile alone, so page
  functions keep their `toString`): 0.4% of a page's 64 MiB. Lazy without
  a visible accessor on `window` needs an engine patch to reach QuickJS's
  lazily-initialized properties, which is more than the cost asks for.
  Two DOM host fixtures whose engine caps had less than 250 KB to spare
  were given room.
- **Rule 1 is one owner of the STATE.** Fetch's own body keeps a byte
  source beside its stream, and a Request needs it: the host takes a
  request's bytes whole, so a Request whose `body` was asked for still
  sends them. Once a body's stream exists it alone answers usable,
  disturbed and locked; a Response's bytes move into it, a Request keeps
  its own as the source. Until then nothing has observed a stream, and
  fetch.c's bytes and flags are the whole state (which keeps `text()` on
  an untouched body as fast as it was).
- **A fetched body's stream stays readable until it is read**: its bytes
  are enqueued at the first pull and the stream closes behind them, so an
  abort that lands after the answer still errors a body nobody read.
- **A clone an abort can still reach is teed**, and the abort errors the
  tee's source, so the clone sees it (DEBTS' clone row, paid).
- **EventTarget's methods are captured from `AbortSignal.prototype`**:
  that is where libdom puts them when the streams row runs, and a signal
  is the only target the pipes listen on.
- **A refusal while streams.js loads fails the runtime.** libjs latches an
  allocator refusal met under script as the runtime's failure; no C-only
  installer had ever run script, so the DOM host's allocation sweep now
  allows it.
- **WPT's lessons, each a rule now written at its site in streams.js:**
  WebIDL's "a promise resolved with" is a NEW promise resolved with the
  value, not `Promise.resolve` (the two ticks a promise's adoption takes
  decide what TransformStream's cancel sees); an internal record's every
  field is a declared class field, since an assignment to a property an
  object lacks reaches a page's setter on `Object.prototype`; a pipe's
  write waits a microtask behind the read that brought its chunk.
- **A tee's second branch is cloned by the window's `structuredClone`**,
  captured like the rest, so the streams row comes after the clone row.
- **Rule 4 is built**: yonder's FAILED line begins `(in os64-streams)` when
  the innermost frame is streams.js's (`OS64_DOM_STREAMS_SCRIPT`, dom.h).
- **An engine patch rode along (libjs 0017).** streams.js's atoms moved
  where yonder's scripted-page sweep cut an event handler's compile, and
  the cut landed on DEBTS' booked `resolve_labels` fault (the parser row's
  item 3): a refused bytecode buffer that kept accepting smaller writes,
  and a relocation patched over an atom. Neither is the Streams
  standard's; both crashed the sweep, so the slice could not pass without
  them. Fable reviewed 0017 (LIBJS_0017_REVIEW.md, untracked), and it
  pays the whole of that item, the unsafe `add_reloc` path included.
