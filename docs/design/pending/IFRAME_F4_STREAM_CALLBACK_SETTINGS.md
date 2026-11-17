# F.4b: Streams callback conversion and invocation

`os64_dom_cluster_create_with_stream_callback_settings` extends the synchronous
settings constructor. It explicitly prepares Streams' self-hosted bytecode with
`JS_PrepareHostImplementation`, both after compilation and after decoding the
process-local cache. Cached bytes do not carry that authority. Ordinary DOM
constructors and Yonder keep their existing mode.

The opt-in captures callback dictionary members for ReadableStream (including
byte sources), WritableStream and TransformStream, and QueuingStrategy `size`.
A private engine-traced holder owns a `DSynchronousCallback`: the callback value,
associated entry realm and captured incumbent. It uses the wrapper's existing
retained realm handles, without adding an F.15 ticket or reclamation policy.
Partial capture and engine finalization release the owned value. Private holders
are not exposed as the dictionary's public callback properties.

The implementation may belong to D while conversion happens under A, the
callback object belongs to B, its native target executes in C, and later stream
operations come from E. Prepared implementation frames preserve A's provenance
at conversion. Invocation prepares B and captured A around the callback and its
return conversion; executable eligibility still follows the actual callable.
Retiring A, B or C refuses invocation. Retiring D alone does not invalidate a
retained stream's data or an eligible callback.

`start` returns `any` and propagates a thrown completion synchronously. `size`
converts its result to a number within the same checked scope. Promise-returning
callbacks create and resolve a fresh intrinsic promise inside that scope, so a
returned object's `then` getter observes the callback settings. A thrown
completion becomes a rejected promise after callback cleanup, in the calling
realm, following WebIDL's exception steps. Cancellation and resource failure
remain terminal. Argument copies, callback holds and temporary promises unwind
on failure.

The new return-coercion probe also exposed a pre-existing TransformStream error:
its fulfillment forwarding retained a value from `Promise<undefined>` and
could read that value's `then` a second time. The reaction now discards that
value, as its IDL type requires. Neighboring readable cancellation already used
that discard; writable completion handlers also discard their inputs.

ReadableStream.from's iterator methods remain ECMAScript calls. Encoding stream
internals use the private transform algorithms, rather than being captured as
author dictionary callbacks. Internal reaction, microtask and abort algorithms
retain their separate scheduling paths. The following cut covers continuation
settings and end-to-end retained-stream proof; this cut does not complete F.4b.

The normative boundaries are WebIDL's
[callback invocation](https://webidl.spec.whatwg.org/#invoke-a-callback-function)
and [Promise conversion/reaction](https://webidl.spec.whatwg.org/#js-promise),
with the callback dictionaries in the [Streams Standard](https://streams.spec.whatwg.org/).

## Validation

`tools/test_dom_stream_callback_settings.inc` is selected by
`--stream-callback-settings` and included in the full DOM suite. Five-realm
cases exercise all four stream families, synchronous start, delayed actions,
cancel/abort, size coercion, Promise assimilation, thrown identity, raw-entry
refusal, callback-role retirement, retained data after implementation departure,
terminal cancellation and both teardown policies. The byte-source fixture
completes its outstanding BYOB request after closing the controller.

Allocation sweeps compile before the refusal seam, then cover partial callback
capture and stream construction, writer invocation/size conversion and abort
Promise conversion. Each cut checks scope unwind and complete root teardown.
ASan/UBSan and normal-exit LeakSanitizer remain enabled; the upstream engine uses
the harness's ASan instrumentation. Target compilation does not establish guest
or P5 acceptance.
