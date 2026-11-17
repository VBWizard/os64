# F.4b: synchronous WebIDL callback settings

The synchronous-settings opt-in covers NodeFilter conversion and traversal in
TreeWalker/NodeIterator, and the pair-iterable `forEach` methods on Headers,
FormData and URLSearchParams. Capturing a callback while A runs, retaining an
associated object from B, executing code from C over data from D, and later
calling from E must preserve those distinct roles.

`os64_dom_cluster_create_with_synchronous_settings` extends the neutral-Promise
constructor. It enables the existing callback families as well as this family;
ordinary constructors and Yonder retain their existing mode. There is no new
JavaScript-visible switch, WindowProxy publication or visible iframe support.

## Capture, invocation and cleanup

A private `DSynchronousCallback` owns one callback value and records its
associated entry realm separately from the captured incumbent. Associated
objects use the engine's creation identity; callable objects use the established
callable association policy. Changing a prototype does not change this identity.
The existing wrapper retains realm handles until runtime teardown; this is not
a new context-reclamation mechanism or an F.15 export ticket.

NodeFilter capture happens after root/whatToShow conversion and before publishing
the traversal object. Its GC mark visits the owned filter value once. Failed
construction and finalization release the record; cleanup resets the holder
before releasing a value that could reenter native finalization.

Traversal prepares associated entry B and captured incumbent A around the whole
callback operation: `acceptNode` lookup, argument wrapping, invocation and
unsigned-short return conversion. The active flag covers lookup and coercion as
well as the call. Getter reentry can adopt the candidate or retire settings:
before calling the selected method, the binding rechecks captured settings,
the candidate's current activity owner and executable eligibility. Callback
exceptions retain identity; a noncallable interface method produces B's TypeError.

Pair-iterable `forEach` captures once per operation, including an empty list,
then releases its stack-owned record on success or abrupt completion. Each
invocation uses the recorded settings, preserving `thisArg` and the value/key/
original-object arguments. Existing list rereads preserve callback mutation.
Pure-data iteration does not acquire the container document's activity gate;
callback eligibility governs execution. Terminal cancellation or host failure
remains terminal rather than becoming a catchable binding exception.

NodeList's borrowed Array `forEach` is an ECMAScript call path and is outside
this pair-iterable conversion family.

The algorithm references are WebIDL's
[callback invocation](https://webidl.spec.whatwg.org/#invoke-a-callback-function),
[user-object operation](https://webidl.spec.whatwg.org/#call-a-user-objects-operation),
[iterable binding](https://webidl.spec.whatwg.org/#es-iterable), and DOM's
[node filter](https://dom.spec.whatwg.org/#concept-node-filter).

## Allocation-failure correction

The instrumented callback allocation sweep exposed an existing engine crash:
`JS_NewObjectFromShape` dereferenced a null result while adding an array's
`length` property. The same sweep with synchronous settings disabled reproduced
it. The upstream claim that this operation could not fail was false: shape
cloning or growth can allocate.

Patch `0038-array-length-allocation.patch` frees the object's current shape,
property storage and object on refusal, before GC publication. Upstream source
files remain unchanged. The manifest hashes the additional patch and generated
engine identity is
`ab1447d8364f77abe3739396bcef1e0b74dc288a5144f79c5fbb2d6357c26e41`;
public ABI 5 and bytecode version 6 remain unchanged.
The allocation test retains both enabled and legacy callback paths.

## Proof and remaining gates

`tools/test_dom_synchronous_settings.inc`, selected by
`tools/test_dom_host.sh --synchronous-settings`, exercises both teardown policies
with five independently identified realms. It covers callable proxies and
ordinary callback-interface objects, spoofed prototypes, getter/call/coercion
exceptions and recursive traversal, adoption and retirement during lookup,
nested listener capture, iteration mutation, cancellation and unprepared raw
conversion. Allocation cuts cover traversal publication, callback execution and
cleanup through both binding and shared-heap allocators.

Validation on the generated engine identity above:

- Focused DOM: 33,662 shipping-core and 66,254 instrumented-core checks, zero
  failures, both teardown policies. Per policy the enabled path injects three
  binding cuts and 107/253 shared-heap cuts (shipping/instrumented); the legacy
  array regression adds two binding cuts and 15/37 shared-heap cuts.
- Full shipping DOM: 411,113 checks and 13,602 binding-allocation sweeps,
  zero failures.
- Full runtime: 29,477 shipping and 95,701 instrumented checks, zero failures
  and zero live allocations. Compile-failure sweeps and fatal invariant probes
  also pass.
- Instrumented DOM neighbors: walker 11, modern APIs 20,334, listener settings
  1,736, custom-element settings 12,000, retention 3,652, recovery 6,672,
  departure 353 and neutral Promise 480 checks, all passing.
- Two private negative controls are rejected: replacing captured incumbent with
  associated entry causes 154 failed assertions; removing post-getter settings/
  activity checks causes 20. The pre-fix instrumented array sweep crashes in
  legacy mode at shared-heap cut 12; the final sweep passes that cut.
- Target Yonder, `js`, runtime guest consumer and shared libraries build with
  warnings as errors. Manifest/header checks, six preparer tests, ELF/link
  audits and the 226 engine/84 runtime export checks pass. The new DOM
  constructor is exported; its helper symbols remain private.

ASan/UBSan and normal-exit LeakSanitizer remain enabled for host validation.
The upstream engine uses ASan without UBSan, following the established harness;
the binding, wrapper and support code use both.

The [settings interface](IFRAME_F4_SETTINGS.md) retains the other F.4b gates:
remaining WebIDL sites including self-hosted Streams, browser finalization task
scheduling, neutral-job exception-reporting policy and role-specific
lifecycle/security admission. F.4c contexts/origins and F.4d exotics/navigation
remain separate. These host proofs do not claim guest or P5 acceptance.
