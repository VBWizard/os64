# F.4b — retained Streams Promise conversion

Implementation PR: **#642**, code checkpoint `74243aa3`.

A retained stream can invoke a live callback after its implementation document
has departed. Its Promise-returning callback must still perform WebIDL's return
conversion, including the second conversion after a thrown callback. The
callback-settings opt-in uses an explicit native capability for that conversion.
Private native adoption and forwarding can complete after ordinary departure;
author-supplied `then` jobs retain their own realm's retirement policy.

This is a trusted embedding lifetime policy, not a general change to JavaScript
Promises or a claim that WebIDL prescribes a detached-document result. The
ordinary constructors, Yonder configuration, raw Promise capability API and
realm-owned queue discard contract remain unchanged.

## Admission and execution

Patch 0041 exposes `JS_NewHostPromiseCapability(ctx, resolving_funcs)` to native
hosts. It creates a fresh intrinsic Promise with a paired set of private
resolving functions. Both functions retain their actual realm and Promise.
Their shared record carries explicit host-continuation admission for direct
resolver functions; author-created bound/proxy wrappers retain ordinary job
ownership, including proxy traps. Reference counting, GC marking and once-only resolution use the existing paths. Copying a
Promise object does not copy this admission into an author-created resolver.
A direct private resolver retained by an author thenable can still be used as a
handler under its native contract; an author wrapper cannot inherit that grant.

Resolution performs the ordinary single `Get(then)`. A callable is eligible for
private native adoption only when it is the direct built-in
`Promise.prototype.then` implementation. A borrowed intrinsic from another realm
qualifies but keeps its actual execution realm. Bound functions, proxies and
author replacements do not qualify, even if they eventually call that intrinsic.
They retain ordinary incumbent/source capture, ownership and retirement discard.

The admitted native adoption job has no author owner, entry, backup incumbent or
source record. It runs inside the explicit neutral settings scope. It calls the
observed intrinsic normally: receiver validation, constructor/species lookup,
custom construction, derived capability calls and their exceptions still occur.
Mutation after enqueue affects species lookup; replacing `then` after its first
lookup does not cause a second lookup. Native invocation provides its actual
current realm. Script-bearing species/capability calls supply their actual
script incumbent; native-only execution does not invent one. This difference
from an ordinary author `then` job is part of the explicit host policy.

Fresh resolving functions created for an admitted adoption inherit its private
forwarding admission. Reactions invoking those functions are neutral native
work, so the forwarding step cannot silently disappear with the implementation
document. The capability's admission follows later native adoption if a resolver
receives another Promise. It does not turn an author replacement `then`, a
species constructor, or work scheduled by author code into private work.

No executable-failure seal, task budget, cancellation, allocation failure or
installation-group boundary is bypassed. Synchronous custom capability calls
keep the engine's ordinary call contract, including its distinction between
retirement and executable failure. A missing checked settings role still
refuses; this API grants no callback, origin, provider or reclamation authority.

The resolving-function shared record remains the same size by storing the two
boolean states as bits. Private adoption adds one argument slot to its queue
entry; ordinary adoption keeps three. Storage uses existing charged allocators.
There are no new context/value edges, runtime configuration fields or serialized
trust bits. Engine consumers must rebuild against the new engine identity;
embedding ABI 5 and bytecode version 6 are unchanged.

## Streams boundary

The private `stream_promise` helper uses this capability only through the
Streams callback-settings opt-in. Successful return conversion stays inside the
associated-entry/captured-incumbent callback scope. Thrown completion cleanup
still precedes rejected-Promise creation and the second fresh conversion in the
implementation realm. Observable getters, error identity and asynchronous
adoption timing are preserved. Terminal failures remain terminal.

This closes the default native-adoption error-completion gap recorded in
[the continuation proof](IFRAME_F4_STREAM_CONTINUATIONS.md). A departed author
replacement, bound function or proxy can still cause ordinary adoption to be
discarded without settlement. That is an explicit lifetime boundary rather than
a promise of universal retained completion.

F.4 remains in progress. Browser finalization task scheduling, neutral-job
exception-reporting policy, remaining callback/security integration, contexts,
origins and WindowProxy/Location publication remain separate gates. F.15's
ownership metadata does not authorize this operation or reclaim its roots.

## Validation

The shared host/guest fixture `host_promise_capabilities.h` and host selector
`--host-promise-capabilities` exercise independent caller, implementation,
replacement-code and value realms under both teardown policies. They cover
ordinary/private twins, fulfillment and rejection identity, pending settlement,
GC, late constructor/species changes, custom capabilities, bound/proxy/foreign
intrinsics, getter and constructor throws, failed code, cancellation, grouped
discard, queued destruction, job budgets and ordinary three-job timing.

Host allocation sweeps cover the whole conversion, from capability creation
through forwarding, and a separate adoption/forwarding execution seam. DOM `--stream-continuations` covers retained
Streams operations and thrown callbacks across readable/default/byte, writable
and transform families. The neighboring callback-settings allocation proof
remains part of validation. The focused runtime proof passes 3,038 shipping and 9,992 instrumented checks,
including whole-conversion and execution sweeps of 3/1 and 72/29 allocation
cuts per teardown policy. The shared resolver record remains 8 bytes and the
queue header remains 104 bytes; the private argument is charged separately.

The full runtime suite passes 34,769 shipping and 116,175 instrumented checks
with zero failures and live allocations; parser refusal and expected teardown
invariant paths pass. The full shipping DOM suite passes 443,791 checks and
14,473 binding-allocation sweeps. Focused Streams continuation/error proof passes
5,010 checks per engine; the instrumented callback composition passes 117,856
checks, including 1,142 allocation cuts. These runs report zero failures.
Target Yonder, js and the runtime guest consumer build; header/manifest checks,
six preparer tests and the 229 engine / 84 runtime export/link audits pass.

The five negative controls remove native admission, remove forwarding admission,
permit author thenables, bypass species, or admit wrapped private resolvers.
`tools/test_js_host_promise_mutations.py` replays them using matching retained
host objects; each must fail a named behavioral assertion on normal exit.
Signals, sanitizer aborts and build failures do not count as rejection evidence.

Engine identity: `e0ab3d653a7a184e84f61cce94aa238768d14eef27b2db949dca6ab3f02e9c6f`.
ASan/UBSan and normal-exit LeakSanitizer remain enabled. Target compilation is
not guest or P5 execution.

The algorithm boundaries are WebIDL's
[callback invocation](https://webidl.spec.whatwg.org/#invoke-a-callback-function)
and [Promise conversion](https://webidl.spec.whatwg.org/#js-promise), ECMAScript's
[thenable job](https://tc39.es/ecma262/multipage/control-abstraction-objects.html#sec-newpromiseresolvethenablejob)
and HTML's
[job integration](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob).
The native capability policy above is an embedding extension to the ordinary
retirement contract; these links do not establish browser acceptance.
