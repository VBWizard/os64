# F.4b — Current-settings base URL for imports without a source record

Inline handlers and indirect eval can execute `import()` without an active
ScriptOrModule. The import-settings opt-in resolves that case against the
executing realm's API base URL. DOM bindings supply their live document base,
using the same algorithm as `Node.baseURI`. Existing Yonder construction keeps
its configuration; F.4 remains in progress.

## Selection, timing and ownership

The engine preserves the active source record before argument conversions.
When that record exists, its source name and realm continue to select module
resolution; a different executing realm still owns the returned Promise.
When no record exists, the executing realm supplies the checked current
settings. Entry, incumbent and the caller's native ABI context are not fallback
identities for that lookup.

The base provider runs after specifier string conversion and import-options
processing. It therefore observes a base change made by `toString` or an options
getter, including a conversion that temporarily enters another realm. It returns
an owned immutable JS string. Queueing retains that string, so later document
mutations cannot change the queued import. Loading and awaited module settlement
use the existing prepared module-job path.

The source basis is ECMAScript's
[EvaluateImportCall](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-evaluateimportcall)
and HTML's
[HostLoadImportedModule](https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule)
and [module specifier resolution](https://html.spec.whatwg.org/multipage/webappapis.html#resolve-a-module-specifier).
The null-record branch uses current settings' API base; a referencing script
continues to supply its own settings and base. These rules do not complete
browser module fetching, import maps, origin policy or document activity checks.

## Interfaces and lifecycle

`os64_js_enable_import_settings` extends the finalization/Promise opt-ins before
execution. `os64_js_realm_set_import_base` independently registers a trusted
native provider and opaque pointer; the existing module-loader structure stays
unchanged. `os64_dom_cluster_create_with_import_settings` composes this policy
with the existing DOM callback settings. Legacy opt-ins retain their previous
null-record rejection behavior.

The raw engine callback is `JSHostImportBase`, configured by
`JS_SetHostImportBase`. The wrapper requires a checked current-settings scope,
matching execution context and eligible realm before calling the provider. It
rechecks retirement and runtime health on return. Missing settings/provider,
non-string return and provider exception reject the import. The provider must
not execute author code or return borrowed string storage. Runtime cancellation
and engine allocation failure retain their enclosing task's sticky outcome.
A native module resolver returning NULL keeps its existing resolution-miss
contract; it is not converted to an engine allocation failure.

DOM installation publishes the provider after other fallible installation.
Successful binding shells remain valid through engine destruction; the callback
holds the existing provider loan while computing the base, and refuses closed,
retired or recovering bindings. Departure and contained recovery detach it with
the module provider. Replacement/detachment requires idle execution, except for
the existing failed-realm recovery preparation boundary. Initial registration
can occur during private setup before evaluation. The trusted host keeps opaque
storage alive until detachment or runtime destruction.

Patch 0036 adds two runtime hook pointers and per-wrapper provider storage,
charged by the existing allocation accounts. Public wrapper layouts and ABI 5
remain unchanged; bytecode format remains 6. The prepared engine identity is
`83e3863bc254f13fe66b3d20ae99edaac4cb1794e9f9bac2672c2fafb6330f43`.
Rebuild consumers with matching prepared headers and objects. No reclaimable
realm capacity, F15 reachability certificate or new context pin is introduced.

The allocation proof also found a missing release in `js_load_module_fulfilled`:
namespace construction failure retrieved an owned exception and passed it to a
borrowing rejection callback without releasing it. Patch 0036 releases that
value after the callback. The corresponding allocation cut must finish with no
retained engine objects under either teardown policy.

## Proof and remaining gates

The shared runtime fixture separates entry A, incumbent B and executing C.
It covers conversion-time base changes, foreign conversion reentry, mutations
after queueing, source-record precedence across realms, returned Promise realm,
awaited modules, provider detachment, missing/raw settings, legacy opt-ins,
exception identity, invalid provider results, retirement and cancellation.
Host sweeps inject each allocation during snapshot/enqueue and later module
loading/settlement. The shared fixture also compiles into the guest consumer.

The DOM fixture uses actual lazy inline attributes, native event dispatch,
indirect eval, a source-bearing foreign caller, live attribute edits, base
removal/document fallback and refusal of the base scratch allocation. It checks
module provider selection, Promise realm, settled namespace value and departure.

Focused runtime tests pass **1,238 / 9,774 checks** in shipping/instrumented
profiles. The allocation sweeps cover **22 / 400 injected failures** across both
teardown policies. DOM integration passes **1,378 checks per profile**. Negative
controls capturing the base before conversion or selecting entry instead of
current settings fail in both profiles; omitting the exception release trips
the retained-object invariant in the instrumented allocation sweep. The target
allocator's arena inventory does not expose that individual namespace failure.

The full runtime suite passes **27,681 / 88,873 checks**, with zero failures
and live allocations. Parser refusal and fatal-invariant paths also pass. Full
shipping DOM passes **375,492 checks**, zero failures, including **13,596 binding
allocation-sweep cases**.

Target runtime, libdom, Yonder and js builds, manifest/header checks and
**225 engine / 83 runtime export** audits pass. Adjacent fully instrumented DOM
checks pass: Promise 246, handlers 5,526, retention 3,652, window 820, callback
reports 1,118, recovery 3,198, departure 353 and finalization 234. Sanitizer runs
retain ASan/UBSan and LeakSanitizer. Host execution and target compilation do
not establish P5 or browser acceptance.
Remaining F.4b work includes realm-less capability policy, browser finalization
task scheduling, remaining WebIDL conversions and role-specific lifecycle and
pre-member security checks. F.4c contexts/origins and F.4d WindowProxy/Location
behavior keep their own implementation and acceptance gates.
