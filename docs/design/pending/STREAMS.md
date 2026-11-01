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

## 4. Cuts

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
   - `TextEncoderStream` and `TextDecoderStream` (the Encoding standard's,
     over the decoder libdom has).

**Booked, not cut:** transferable streams (`postMessage` of a stream),
request bodies that stream (`duplex: "half"`), `CompressionStream` and
`DecompressionStream` (libgzip could serve them; a slice of their own).

## 5. Lifecycle

| Event | Streams code | A body's stream |
|---|---|---|
| a realm starts (a document's scripts) | its bytecode read; intrinsics captured; globals defined; the C door handed back | — |
| a page builds a stream | the standard's operations, in JS | — |
| a fetch answer arrives (S1, S2) | — | made through the C door, its bytes enqueued whole, closed |
| a head arrives, then chunks (S3) | — | made at the head; each chunk enqueued as it comes; closed at the end, errored on a failure |
| a fetch is aborted | — | errored through the door, as the Response's abort steps say |
| the page leaves | — | its streams go with the realm; a pending host read is cancelled, as today |

## 6. Proof

- **WPT `streams/`** (testharness `.any.js`, added to `tools/wpt_fetch.sh`),
  the oracle: readable, writable, transform, piping, queuing strategies, and
  in S2 the byte streams. The `patched-global` pages check the captured
  intrinsics.
- **No loss in WPT `fetch/api`**, whose body tests move from the C streams
  to these, and in the DOM host suite.
- **A host fixture of a realm's start**: its cost measured (time and memory
  per realm), since every document pays it; read lazily on first use if it
  is too dear.
- **The P5**: the social site's sign-in pages draw; GitHub's bundles get
  past `ReadableStream`.
