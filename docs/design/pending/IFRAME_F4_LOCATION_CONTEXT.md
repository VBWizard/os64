# F.4d — Retained Location and the active document

This milestone separates three identities: the Location receiver's original
binding, its context's active document, and the calling operation's settings.
It uses the private origin-bearing context seam; it does not publish a
WindowProxy or a cross-origin Location exotic.

## Behavior and boundaries

A native Location member selects its branded receiver by retained registry
identity, including when the member was borrowed from a different binding.
Prototype lookalikes and JavaScript Proxy wrappers do not acquire the brand.
The pre-WebIDL caller is checked against that receiver's original immutable
origin. `href` writes and `replace` retain their distinct cross-origin exception.
A departed original binding can supply this identity when it has a native
context; a departed caller cannot gain permission from that retention.

After the IDL check, the context supplies the active document. URL reads and
supported navigation operations use that document, with an independent entry
origin check where required. The original origin never changes to the replacement
origin. Thus an entry from B can call a function from A that reads A's retained
Location after its context moves to B, while a direct call from B still fails
the original-global IDL check. The reverse combination fails the document check.
These roles follow the [Location algorithms](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-location-interface)
and [IDL security integration](https://html.spec.whatwg.org/multipage/nav-history-apis.html#integration-with-idl).

Supported navigation retains the existing policy seam: the current target and
its generation, entry URL base, incumbent source, conversion revalidation and
policy authorization are recorded together. Reload selects the current document
as source. Old providers are not used. A discarded context exposes the blank URL
through permitted reads; writes and navigation methods convert their string
arguments and then do nothing. An inactive target without committed discard
refuses access instead of presenting itself as a blank document.

Default single-document hosts keep their existing behavior. Component setters
other than `href` and `hash` retain their existing no-op implementation; this
milestone does not supply a complete URL mutation implementation. Location
exotic property operations/descriptors, WindowProxy methods, global alias/event
closure, browser frame publication and the final security/lifetime acceptance
remain open. F.4 remains in progress, and F.15 reclamation remains gated.

## Ownership and validation

Lookup is allocation-free and uses the existing bounded context and binding
registries. No new object holds, fields, public API, engine patch or ABI change
are needed. The retained relevant binding owns the Location; context publication
chooses the document independently. Reclaiming either registry before root
teardown remains outside this contract.

The `--location-context` host fixture runs both teardown policies. It covers
borrowed members, rejected brands, distinct caller/entry authority, conversion
ordering, same/cross/same replacement, old-provider detachment, active-document
URL components, assign/hash/reload/replace routing, generation changes, discard,
throwing conversion, bounded native storage and departure without publication.
The older navigation, binding authority, Window, context and retirement suites
supply the adjacent regression checks. Local `Reflect.apply` deliberately uses
the local native caller; a foreign function's own `Function.prototype.call`
executes in that foreign realm. The fixture proves these distinct outcomes,
including rejection when that intermediate caller has departed.

Validation after integrating base `da7ca2c2` (including merged #658):
implementation `e46e9d67`, integration `bea6ea7e`.

| Host fixture | Shipping engine | Instrumented engine |
|---|---:|---:|
| Retained Location, both teardown policies | 422 | 422 |
| Window, including engine allocation cuts | 648 | 820 |
| Navigation authority | 1792 | 1792 |
| Native contexts | 1078 | 1078 |
| Binding authority | 526 | 526 |
| Window invocation authority | 304 | 304 |
| Browser jobs | 801 | 801 |
| Realm/provider retirement | 9768 | 9768 |
| Realm routing | 678 | 678 |
| Blob | 61 | 61 |

All rows have zero failures. The Location fixture includes one conversion
allocation-refusal cut per teardown policy, throwing conversion identity and
native charge stability. ASan/UBSan/LeakSanitizer are enabled (the instrumented
engine uses the established ASan-only engine instrumentation). Sandbox diagnosis
reported thread-attachment EPERM with `TracerPid: 0`; supported runs passed
outside it with leak checking enabled.

The finalized fixture rejects the merged implementation and six targeted
controls: receiver pinned to function owner, omitted entry-origin check,
rejection of retained original bindings, stale URL reads, stale navigation target,
and inactive target treated as discarded. Each exits 1 on behavioral assertions
without a sanitizer report. The baseline and controls use the instrumented
engine with the same final fixture and unchanged companion objects.

Preparation's six tests, source manifest, cross-header checks, target allocation
and export audit, tracker and stale-reference checks pass. Yonder, JS CLI,
runtime-core-test and DOM-realms consumers compile. The Location change adds no engine or public ABI
change; the new internal helper is absent from the dynamic exports. Engine ID:
`bcc933c20d304c1d5be5b1cb0e703b9d0f198bc7dc59fdf374050f48f190ca5f`.

The initial host cohort was built from `5a67b444`. Integrating #658 required
rebuilding the host fixture and every DOM translation unit for its new private
layout and animation signatures. Both engines, runtime and lower-level objects
were unchanged and reused; those sources do not include the changed DOM headers.
Focused Location, adjacent regressions and controls were then rerun on the
combined cohort. Shipping consumers rebuilt against the new public signatures.
Frozen evidence records sources, both host binaries, controls, logs and hashes.
No new guest, full DOM suite, runtime suite, WPT or P5 acceptance is claimed;
this is a private routing milestone, not final F.4 acceptance.
