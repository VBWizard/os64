# F.4b — EventTarget listener settings

`os64_dom_cluster_create_with_listener_settings` enables checked callback
settings for `addEventListener`, together with the timer opt-in. Ordinary
cluster construction and Yonder remain unchanged. This chapter includes the
engine identity, registration, lookup, dispatch and ownership needed by that
callback family; HTML on-event handler slots and other families remain open.

## Roles and object identity

A registers B's listener object on D's target; its `handleEvent` getter and
returned native method can belong to C. Registration captures A, preparation
enters B before lookup, execution follows C, and D supplies target activity and
the host task. The existing script/report owner remains independent. A
script-bearing getter or method supplies its own incumbent while executing;
a native-only invocation observes the captured backup.

[WebIDL callback-interface invocation](https://webidl.spec.whatwg.org/#call-a-user-objects-operation)
prepares the associated object realm and captured callback context before
looking up the operation, then calls the resulting method with the listener
object as receiver. Its [JavaScript environment](https://webidl.spec.whatwg.org/#js-environment)
leaves some associated-object realm mechanics underspecified. Our concrete
policy uses engine construction identity for noncallable objects and the
existing callable-associated policy for functions, bound functions and callable
proxies. A noncallable proxy has its own construction identity, independent
of its target and traps. Mutable prototypes do not supply realm identity.

Patch 0031 adds `JS_GetObjectRealm(ctx, value)`. Each context receives an
immutable runtime-local 64-bit ID; object allocation records that ID. A query
matches it against live contexts and returns a borrowed context, or NULL for
an unavailable identity. IDs never repeat, including after failed context
construction; exhaustion refuses a new context. The trusted API requires a
same-runtime object and performs no property access, allocation, trap or
exception mutation. Callable values delegate to `JS_GetCallbackRealm`.

The ID is not a GC edge. A raw context can be collected while an external
noncallable object survives, after which the query refuses even if another
context reuses its address. Wrapper realm contexts already remain retained
through root destruction; retired/failed handles still fail checked admission.
This policy adds no new ordinary-object context roots. It does not supply
operation-specific security authority. DOM node wrapper construction already
uses the node's creation realm, independently of later adoption.

The metadata occupies one uint64_t in each object, context and runtime and
uses their existing allocator accounting. On the x86-64 build, JSObject grows
from 64 to 72 bytes; this cost applies even without the listener opt-in.
Listener records additionally hold
an inline callback record and an associated wrapper realm pointer. Public
wrapper ABI 5/layouts are unchanged; prepared engine headers and objects must
be rebuilt together.

## Registration and delivery

In the opt-in path, type conversion precedes nullable callback conversion;
capture precedes options getters. Missing prepared settings refuse before
options access. Options can reenter or retire captured settings, so publication
rechecks lifecycle and resolves the target's activity owner again. Duplicate
registration preserves the original record, flags and incumbent. Removal does
not require a fresh capture. Failed conversion/allocation releases temporary
ownership while preserving the pending exception.

The record owns one callback reference; its existing callback field aliases
that hold. Host and script dispatch snapshot the hold and attribution before
`once` removal. The prepared scope encloses `handleEvent` lookup, callability
validation and invocation. After a getter, captured/entry lifecycle and the
target's possibly changed activity owner are checked again; returned executable
code has its own eligibility check. Missing methods produce a TypeError in the
associated entry realm. Lookup exceptions retain their identity for reporting.
Terminal bridge failures preserve a pending exception and remain uncatchable
by the script dispatch caller, including failure detected after the body.
No new task, deadline, checkpoint or recovery is introduced by preparation.

Host dispatch hands its snapshot to the guarded native invocation and clears
transient pointers before author reentry. Removal, replacement, drain and
finalization release records; plain EventTargets mark the aliased callback
edge once so cycles can be collected. Both teardown policies retain cleanup
after retirement, cancellation or terminal runtime failure.

F16's contained failed settings refuse with REALM_FAILED; ordinary retirement
refuses with REALM_RETIRED. Both suppress listener delivery. Interrupted native
or settings scopes remain globally terminal under F16's existing exclusions;
this adapter does not certify native containment or enable recovery in Yonder.

## Validation and remaining work

`tools/test_dom_host.sh --listener-settings` exercises four independent roles
on both engine profiles and both teardown policies. It covers function and
object proxies, null/mutated prototypes, collected creation contexts and ID
churn, foreign native getters/methods, host and script dispatch, duplicate and
once/reentrant delivery, removal, options ordering/failure, unprepared jobs,
role-specific retirement, adoption during lookup, getter errors, allocation
refusal, GC cycles, cancellation, deadline interruption and balanced drain.
Script-dispatched interruption is tested in both the getter and method: an
8 MiB allocation in the caller's catch must never run. The test reproduced
four failures before the adapter stopped replacing the interrupt exception.

Recorded results with ASan/UBSan and LeakSanitizer enabled:

- Final listener fixture: 1,736 checks on each engine profile, both teardown
  policies; script-dispatch interruption reaches the getter/method and cannot
  enter the caller's catch.
- Full runtime: 21,930 shipping-engine and 69,320 instrumented-engine checks,
  zero live allocations; parser allocation failures (40 checks per profile)
  and invariant exit-badge checks pass.
- Broad shipping-engine DOM: 324,293 checks and 12,952 native allocation
  cuts. This sweep preceded the final opt-in interrupt-preservation refinement;
  final listener and affected reporting/lifecycle selectors were rerun afterward.
- Final reporting/lifecycle selectors on each profile: timer settings 718,
  callback reports 1,118, realms 678, Streams realms 104, departure 353 and
  report retirement 616. Instrumented child installation: 1,306 checks.
- Six deliberate defects are rejected by specific assertions: wrong object
  entry, wrong incumbent, lookup without preparation, omitted post-getter
  checks, late capture and lost once-listener snapshot.
- Target libdom/Yonder/runtime builds, source preparation (six tests), contract
  headers, teardown allocation audit, tracker and stale-reference checks pass.

Browser callback families beyond timers and EventTarget listeners, HTML handler slots, Promise
preparation, relevant-settings security boundaries and WindowProxy/Location
remain F.4 work. Host tests and target compilation do not establish QEMU/P5 or
WPT acceptance, and F.4 remains in progress.
