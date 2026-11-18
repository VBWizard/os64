# F.4d — Private WindowProxy property boundary

The internal host seam `d_window_proxy` prepares one native proxy per retained
browsing context. Property operations follow that context's current Window;
replacement keeps proxy identity, while discard retains the last Window for
property security and detached relation answers. This builds on native context
publication and [Location exotics](IFRAME_F4_LOCATION_EXOTIC.md).

This is an incomplete private integration, not a browser publication API.
[Private Window identity integration](IFRAME_F4_WINDOW_IDENTITY.md) connects
global-this, document/event values and callbacks. Same-origin Window named
properties, remaining descriptor integration and complete F.4 security/lifetime
acceptance remain gated. The host tests deliberately exchange
raw globals alongside proxies as independent oracles. Such raw globals must
not be exposed by a production frame adapter. F.4 remains in progress.

## Property operations

The proxy has no own engine storage and a null actual prototype. Explicit
native hooks implement the [WindowProxy internal methods](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-windowproxy-exotic-object).
Same-origin descriptors/storage are supplied by the wrapped global. Numeric
indices use the native child order; absent indices remain absent from own
properties, and index definitions/assignments refuse. Ordinary prototype lookup
and alternate accessor receivers are preserved. Reflected nonconfigurable
properties may change when the wrapped Window changes, without JavaScript
Proxy target-invariant constraints. Prototype assignment accepts only the
reported prototype; extensibility reports true and prevention refuses.

The origin gate queries CURRENT settings before dispatching a getter or
setter, including borrowed Reflect/Object builtins. Existing native members
still check the pre-native caller. Window receiver selection recognizes a
proxy and resolves its current target without author code. Window/self/frames
getters return a prepared proxy; their replaceable setters still write the
real global. Length/top/parent/closed use the native context when opted in.
Unprepared ordinary bindings retain standalone answers.

Cross-origin own keys contain child indices, the specified Window member names
and fallback keys. Unknown property reads/has/own-descriptor queries and
non-location writes refuse. Define/delete refuse across origins. A permitted
child target name returns its proxy, with the first duplicate considered
before the child's origin is compared with the target Window's origin.
Builtins precede child names; child names precede fallback keys. Document IDs
are not a cross-origin name source, and returned children retain their own
security boundary. Native ordering is the host's list order; mapping DOM
container insertion/reordering into that list belongs to the frame adapter.

Cross-origin descriptor caches distinguish accessing binding, relevant target
binding and property key. Each descriptor owns an anonymous builtin in the
accessing realm. It performs the original IDL operation on the captured Window,
ignoring an alternate receiver. Noncallable original method values are cached
as values; a callable author replacement does not replace the IDL steps.
Replacement changes the target key for new lookups. Retained getters keep the
old relevant Window: its Location remains distinct, and its detached parent
and top are null. Location setters use retained Location routing and recheck
navigation permission/current generation. A live caller may invoke a cached
function after the function's creation realm departs.

Close/focus/blur retain the existing no-close/no-focus behavior; there is no
opener relationship. `postMessage` on proxy-bearing Windows explicitly throws
NotSupportedError before conversion, including the ordinary member path. It
cannot silently use the standalone self-post implementation; F.5 owns the
cross-realm adapter.

## Ownership and refusal

The context stores its last Window and retained proxy handle. The binding that
first prepares the proxy owns its native record, well-known atom references,
cache nodes and DValue roots. Lazy preparation uses bootstrap-captured symbols,
not a mutable `Symbol` property. Descriptor construction uses private array
own definitions, avoiding inherited author setters. Cache insertion commits
only after the complete descriptor and root have been prepared. Native quota
or engine allocation refusal leaves no partial proxy/cache publication.

The bounded context/binding registries and finite descriptor-key set bound
cache cardinality; allocation is charged to the owning binding/cluster and
shared engine. Departure retains these records. Root drain releases atom
references and the existing DValue registry releases engine roots; native
records are freed after engine teardown. No JS finalizer owns context lifetime.
Existing raw-context/settings access continues to veto ticket-only reclamation;
this milestone supplies no C3 evidence or slot reuse.

Measured x86-64 layout: context 112 bytes (+16), proxy record 96, cache node 40,
DValue 112 and allocator header 16. Initial proxy native record/root storage
charges 240 bytes; a cached descriptor charges 184 native bytes plus its engine
array/functions. At this property-boundary milestone the binding gained a pointer
but stayed 17,488 bytes because of layout padding. The
[identity integration](IFRAME_F4_WINDOW_IDENTITY.md) records the larger current
binding layout; affected objects must be rebuilt. With at most 65
retained bindings, 13 cached property keys and each target binding associated
with one context, a conservative cluster-wide ceiling is 65 × 65 × 13 entries.
The configured aggregate budgets can refuse well before that ceiling.

## Global-property correction

The wrapped global uses QuickJS variable-reference storage. The previous
JS_DefineProperty VARREF branch incorrectly assumed these descriptors were
writable, permitting a new value on a nonconfigurable, nonwritable global
property. Patch 0044 checks SameValue before updating such a global reference.
Same-value redefinition (including NaN) remains valid; signed-zero changes,
distinct objects and changed primitive values refuse. Configurable or writable
global properties retain their supported updates. This fixes ordinary globals
as well as WindowProxy forwarding; no binding-only workaround masks it.

Engine ID:
`038c999a4588e07b30691d4848af28df7d2c530e42fb8cb864bb551cccb696ce`.
Public headers and exports are unchanged. The global receiver host/guest fixture
covers this regression directly, and the WindowProxy fixture covers forwarding.

## Validation

Cohort base: merged `c9aa57c3` with this diff. Host/binding/runtime code uses
ASan/UBSan; the second engine profile also instruments engine allocations with
ASan. LeakSanitizer is enabled in both profiles outside the sandbox, following
the previously diagnosed sandbox thread-attachment EPERM restriction.

| Probe | Shipping engine | Instrumented engine |
|---|---:|---:|
| WindowProxy matrix and allocation cuts | 1,426 | 3,726 |
| Full runtime | 41,293 | 141,665 |
| Location exotic | 1,932 | 3,052 |
| Retained Location | 424 | 424 |
| Window | 648 | 820 |
| Navigation authority | 1,796 | 1,796 |
| Browsing contexts | 1,078 | 1,078 |
| Binding / Window authority | 526 / 304 | 526 / 304 |
| Browser jobs | 801 | 801 |
| Retirement queues | 9,768 | 9,768 |
| Realm routing | 678 | 678 |

All positive checks pass. Runtime invariant-exit checks and compiler refusal
sweeps pass. Per teardown policy, the WindowProxy sweeps cut every measured
host-visible allocation in preparation (2 shipping / 4 instrumented), first
location descriptor (3 / 15) and own-key enumeration (0 / 2). The shipping
allocator services this key-list operation from its pool without a host call;
zero is not a claim of instrumented coverage. Refusal checks assert no partial
identity/cache publication or native charge growth. Both teardown profiles
cover retained descriptors, creation-realm departure, replacement/discard,
policy revocation, native caps and forced GC.

Ten behavioral controls are rejected without sanitizer failures: bypassed
origin checks, stale target origin, entry/current confusion, missing target
cache identity, aliased accessor keys, collapsed equal-origin source globals,
raw-window escape, exposed cross-origin prototype, receiver retargeting and
skipping a disallowed first duplicate child. An engine control removes patch
0044 while keeping the current build identity: both the direct global fixture
(4 failed assertions) and WindowProxy fixture (2) reject it. The accessor-key control isolates aliasing between getter/setter descriptors.

Yonder, js, dom-realms-test and js-runtime-test build. Six engine preparation
tests, cross-header/manifest checks, the target allocation/global/visibility
audit and tracker validation pass. libdom's 153 defined dynamic symbol names
are unchanged. Fresh objects were built for the new binding/context layout;
unchanged companion objects were reused while iterating the focused fixture.
The readonly-global patch rebuilt engine/runtime objects before final tests.
No full DOM suite, guest execution, frame WPT integration or P5 acceptance is
claimed. Those and the remaining exposure/lifetime proofs stay separate from
this private property milestone.
