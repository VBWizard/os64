# F.4d — Private Location property semantics

The private origin-bearing binding bootstrap installs a native Location exotic.
Ordinary bindings keep their existing objects. This is the property layer over
[retained Location routing](IFRAME_F4_LOCATION_CONTEXT.md), not a WindowProxy
publication API or browser frame enablement. F.4 stays in progress.

## Property boundary

A Location has a hidden ordinary backing object and an empty native exotic
surface. The backing object supplies same-origin storage, prototype traversal
and accessor receiver behavior. Its initial own properties are unforgeable;
the captured default-key set controls reflective configurability and refusal
of redefinition. Added properties retain their ordinary descriptor semantics.
The surface has a null engine prototype so prototype and property operations
reach the native hooks rather than an unguarded ordinary chain.

Property access uses current settings, including the realm of a borrowed
Reflect/Object builtin. This differs from entry settings and from the later
pre-WebIDL member caller. Missing origin-bearing current settings refuse.
Origin comparisons use immutable binding tokens, not document URLs.

The cross-origin surface contains the href setter and replace operation plus
the specified undefined-valued fallback keys. Other reads, writes, existence
queries and descriptor queries refuse; cross-origin delete/define refuse.
Same-origin defaults report configurable descriptors but cannot be redefined.
Prototype changes accept only the currently reported prototype; the object
reports extensible and refuses prevention of extensions. These rules implement
the [Location internal methods](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-location-interface)
and [cross-origin helpers](https://html.spec.whatwg.org/multipage/nav-history-apis.html#crossorigingetownpropertyhelper-(-o,-p-)).

Cross-origin descriptor functions are anonymous builtins in the accessing
realm, bound to the original Location independently of their invocation receiver.
The cache key is accessing binding plus property key within one relevant
Location. Equal origins do not collapse global identities. Invocation passes
through existing member and navigation checks, so a cached function cannot
retain obsolete navigation permission or a previous target generation.
Its creation realm may depart while a live caller retains the function.

The existing supported member algorithms remain in force: href/hash navigation,
assign/replace/reload and URL reads. Other component setters retain their prior
no-op implementation. WindowProxy internal methods, global aliases/events,
public context publication and final F.4 security/lifetime acceptance remain
separate gates. F.15 collection and slot reuse remain gated.

## Engine and ownership audit

| Operation | Native boundary and backing operation |
|---|---|
| Get / Set | Explicit hooks; same-origin GetPropertyInternal/SetPropertyInternal preserve alternate receivers; cross-origin functions remain bound to the original object |
| Has | Explicit hook; same-origin HasProperty traverses the backing prototype; cross-origin descriptor semantics reject unknown keys |
| Own descriptor / keys | Explicit hooks; initial keys receive configurable presentation; cross-origin descriptors are cached and non-enumerable |
| Define / Delete | Explicit hooks; defaults refuse even identical definitions; ordinary backing enforces expando descriptors and deletion |
| Prototype | Explicit get/set hooks; actual engine prototype remains null |
| Extensibility | Explicit hooks; true / refuse, including Object.freeze/preventExtensions |

The engine's ordinary own-slot fast paths find no slots on the exotic. The
native hooks cover language property access and Reflect/Object helpers. The
existing nonextensible guard carries the engine's throw/strict interpretation
for refused default definitions. Delete preserves the outer engine's strict
handling. Native exotics avoid JavaScript Proxy target-invariant constraints.
No engine patch or public header/export change is introduced by this milestone.

Each binding adds one pointer. An origin-bearing binding owns its exotic
record, captured atom lists and a retained backing object. Cached functions
hold the target anchor and Location through ordinary engine data-function
tracing, and DValue supplies explicit strong roots. Native records and root
storage are charged through the binding/cluster allocator; atom lists and
function objects use the shared engine's allocator. Two descriptor keys and
the bounded retained binding registry bound cache cardinality per Location.
Cache insertion commits after native allocation, function creation and root
retention; refusal frees unpublished resources. Repeated lookup adds no holds.

On the tested x86-64 layout, the binding is 17,488 bytes (+8), the exotic record
is 64 bytes, a cache cell is 32 bytes, DValue is 112 bytes and the allocator
header is 16 bytes. Initial native record/root storage charges 208 bytes beyond
the binding; each cached function adds 176 native bytes, plus engine allocations.
The retained registry permits 65 bindings, giving a conservative ceiling of
130 cache cells per Location. Existing raw-context/settings bridges retain their
compatibility veto; these roots do not establish ticket-only reclamation safety.

Root drain releases atom lists before engine destruction and releases DValue
roots through the existing registry. Native records are freed after engine
teardown. Departure retains them. No engine finalizer owns a native binding or
reclaims this registry. A failed candidate follows grouped admission ownership;
its partially prepared record is drained through the same cleanup path.

## Proof

`--location-exotic` covers the property matrix, alternate receivers, numeric
and symbol keys, strict assignment/deletion, prototype and extensibility,
default versus expando descriptors, current-versus-entry settings, distinct
cache globals/targets/keys, forced GC, target-side getter suppression, navigation
policy revocation, replacement/discard, creation-realm departure, native quota
refusal, measured construction-tail cuts and first-descriptor allocation cuts.
Test-only linker interception also sweeps every measured host-visible allocation
inside private child installation. Its parent has registered the class already;
this is not an allocation sweep of fresh class registration. The sibling mutation
runner uses the same link interception so it continues to consume these objects.
The retained Location and navigation fixtures prime descriptors before measuring
request allocation, keeping cache accounting separate from navigation accounting.

The host cohort uses merged base `c9fe984b` with this diff and unchanged engine ID
`bcc933c20d304c1d5be5b1cb0e703b9d0f198bc7dc59fdf374050f48f190ca5f`.
Both teardown policies pass with ASan/UBSan and LeakSanitizer enabled outside
the sandbox (the sandbox denied LSan thread attachment with EPERM/TracerPid 0).
The shipping engine is reused uninstrumented; the second profile instruments
the engine with ASan and the host/binding code with ASan/UBSan.

| Host probe | Shipping engine | Instrumented engine |
|---|---:|---:|
| Location exotic | 1,932 | 3,052 |
| Retained Location context | 424 | 424 |
| Window | 648 | 820 |
| Navigation authority | 1,796 | 1,796 |
| Browsing contexts | 1,078 | 1,078 |
| Binding authority | 526 | 526 |
| Window authority | 304 | 304 |
| Browser jobs | 801 | 801 |
| Retirement queues | 9,768 | 9,768 |
| Realm routing | 678 | 678 |

All checks pass. First-descriptor and private-installation sweeps each cover
2 host-visible allocations per policy in the shipping profile and 6 in the
instrumented profile. Native cache allocation/root refusal, retry and the last
three measured child native construction allocations are covered separately.
Ten behavioral controls fail the intended assertions without sanitizer errors:
origin bypass, cache-key omission, collapsing same-origin globals, entry/current
confusion, mutable defaults, missing configurable presentation, ordinary-object
substitution, leaked prototype, receiver retargeting and the wrong fallback key.
The existing aggregate-cap mutant is also caught by the sibling runner.

Yonder, js, dom-realms-test and js-runtime-test build; engine preparation tests,
cross-header syntax, target allocation/visibility audit and tracker validation
pass. The libdom defined dynamic symbol names match the prior retained-Location
milestone. Focused host binaries reuse unchanged companion objects between
fixture revisions; the changed binding layout was rebuilt across the cohort.
No full DOM/runtime suite, guest execution, WPT iframe integration or P5
acceptance is claimed. The public proxy and frame tree remain gated.
