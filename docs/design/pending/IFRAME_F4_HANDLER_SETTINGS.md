# F.4b — HTML handler settings

`os64_dom_cluster_create_with_handler_settings` extends scheduled/listener/timer
settings to HTML event-handler properties, named handler properties (including
AbortSignal, XHR and MessagePort), and lazily compiled content attributes.
Ordinary constructors and Yonder do not opt in. F.4 remains in progress.

## Capture, invocation and ownership

Property assignment converts an object to a callback with the assigning
caller's incumbent. Its associated realm is obtained independently, including
proxy creation realm and non-callable object identity. Both setter families
use the same publication path. Missing or retired settings refuse before an
existing handler is replaced. Non-object values deactivate the handler without
capturing a callback. Reassigning the same object replaces its capture; this
is distinct from duplicate addEventListener registration.

The slot uses the existing DListener capture fields and owns one callback
reference, a native root for registry slots or a traced edge for engine-held
plain targets. Replacement publishes its new value before releasing the old
one. Dispatch holds the callback and snapshots its settings before invocation,
so clearing or replacing the slot from inside the callback cannot invalidate
the invocation. Removal, attribute refresh and root drain release captures
through the existing listener cleanup paths. No per-slot storage is added.

Both host and script dispatch prepare the associated entry and captured
incumbent inside the existing guard. Executable eligibility, target activity,
report attribution, ordering and checkpoints keep their existing roles.
Ordinary handlers receive the event with currentTarget as their receiver.
Window onerror receives its five error arguments and cancels on true; ordinary
boolean false cancels other supported events. Non-callable object handlers are
retained and returned by getters but invocation is a no-op: unlike an
EventListener object, they do not look up handleEvent. That correction is
scoped to the handler-settings opt-in.

## Inline attributes

HTML assigns a compiled content attribute its document's callback context,
independent of the script that first reads or dispatches it. Compilation runs
inside a checked document entry/incumbent scope and captures there. Existing
lazy compilation, document/form/element environments, body-to-window
reflection, syntax reporting and attribute refresh remain in place.

Patch `0032-inline-handler-provenance.patch` adds the trusted raw operation
`JS_ClearFunctionScriptProvenance`. The embedding calls it on a fresh,
unexposed bytecode closure before publishing the inline handler. It verifies
the complete saved function source against the generated inner function; an
escaped body cannot select another closure for provenance mutation. It clears
the existing ScriptOrModule presence marker without changing executable realm,
allocation, ownership or pending exceptions. It refuses other value/classes,
including proxies and bound functions, absent source and mismatched source.
The existing concatenating compiler can still execute malformed input while
compiling; this guard protects provenance mutation rather than replacing that
parser. It does not alter an active invocation
or provide complete Script/Module records. Clearing the marker prevents a
handler called directly from masking an older script-bearing caller merely
because some script happened to trigger its compilation.

The engine identity is
`39063a8f7f5747a9737b4f15b67dddb87f6344021bfba9fe3ae79a12ef020fbf`;
prepared headers and engine/runtime/DOM objects
must be rebuilt together. Wrapper ABI 5 and public wrapper layouts do not
change. No recovery policy, scope quiescence or native loan behavior changes.
The F16 recovery constructor remains separate.

## Source basis and proof scope

[HTML handler processing and compilation](https://html.spec.whatwg.org/multipage/webappapis.html#event-handlers)
provide the property/attribute distinction, associated-entry activity gate,
special onerror handling and scriptless inline closure. WebIDL's
[callback conversion](https://webidl.spec.whatwg.org/#es-callback-function) and
[LegacyTreatNonObjectAsNull](https://webidl.spec.whatwg.org/#LegacyTreatNonObjectAsNull)
provide incumbent capture and the non-callable no-op behavior.

`--handler-settings` uses independently named A/B/C/D realms for assigning
incumbent, associated proxy, native executable and target. It covers both
teardown policies, both engine profiles, host/script dispatch, property and
named setters, onerror, reassignment, clearing during delivery, role retirement,
missing scope, allocation refusal, revoked proxy errors, terminal interruption,
inline document capture and direct invocation, and balanced cleanup.

Recorded final-source evidence uses ASan/UBSan and LeakSanitizer:

- Full shipping-engine DOM: 338,147 checks, zero failures, including 12,949
  binding allocation sweeps. Its production objects are byte-identical to the
  final focused cohort; the focused fixture additionally strengthens the
  escaped-body test with an existing foreign closure.
- Focused handler settings: 5,526 checks per engine profile, both teardown
  policies, with balanced native accounting and complete root cleanup.
- Nine deliberate defects are rejected by named assertions: wrong associated
  entry, wrong incumbent, EventListener semantics on a non-callable handler,
  unprepared onerror, wrong inline incumbent, script-bearing inline closures,
  missing invocation snapshot, uncaptured named setters and an unchecked
  function source. The last variant returns an existing foreign closure and
  proves its original provenance must survive refusal.
- Complete runtime: 22,014 shipping-engine and 69,404 instrumented-engine
  checks, zero failures and zero live allocations. Parser refusal and fatal
  invariant probes pass.
- Affected instrumented regressions pass: events 1,020, reporting 4,523,
  window 820, callback reporting 1,118, report retirement 616, listeners 1,736,
  scheduled callbacks 6,066, timers 718, recovery 6,672, realms 678,
  retirement queues 9,770, departure 353, custom elements 298, observers 32,
  Streams realms 104 and child installation 1,306 checks.
- Target libdom/Yonder/runtime builds, manifest/preparer, contract-header and
  teardown audits pass.

Target builds and
host tests do not establish WPT, QEMU or P5 acceptance. Promise preparation,
observers/custom elements, origin checks and WindowProxy publication remain
separate work; this chapter is not a complete HTML/WebIDL conformance claim.
