# F4/F15 C3 — retained target and exposure lifetimes

Contract PR **#682**. F.4.37 supplies F4's lifetime inputs to F.15.22. This contract describes the
private implementation; it does not enable early destruction, slot reuse,
public frame publication or a complete release certificate. ABI 5 pointer
handles retain their existing meanings until runtime destruction.

## Initial task list

- Separate stable context, document/settings owner and publication generation.
- Enumerate F4 exposures, cache roots and native borrows with their owners.
- Specify publication, reentry, failure and teardown obligations for F15.
- Agree consumption with F15 and retain explicit unknown release dependencies.

## Identities and authority

`DBrowsingContext` is a cluster-owned retained identity. Its address is stable
until grouped teardown; `generation` qualifies a publication, not a reusable
storage incarnation. `active` names the eligible document or NULL; `window`
retains the most recently published binding even after discard. The context
and each binding's wrapper realm are distinct identities. A prepared candidate
may borrow the proxy as `window_identity` before publication; it cannot run
author code or replace the active target until the quiescent commit succeeds.

A retained Location's relevant global is its original binding. Its document
selection follows that binding's stable context. Its original origin does not
change when the context publishes another document. WindowProxy property
operations instead select the current context Window and compare the current
settings origin. Native IDL member checks use NATIVE_CALLER and the actual
receiver. Navigation uses its separately captured source/incumbent policy.
Entry, incumbent, current, relevant, report and task owner are separate roles.
An F15 owner ticket proves identity/lifetime only; it grants none of these
property or navigation permissions and does not make a retired realm active.

## Existing retention inventory

| Holder / source | Retained resource and accounting | Required validity / drain |
|---|---|---|
| `DBrowsingContext`, `browsing_context.inc` | Cluster registry, copied name storage, parent/child links, generation, active and retained Window pointers; charged context record and cumulative realm cap | Context identity and ancestry survive discard; no reuse. Freed after binding/engine users at grouped teardown. Rename changes no generation; root origin reset changes name only at valid publication. |
| `DWindowProxy`, `window_proxy.inc` | Native record owned by the binding current at first proxy creation; context pointer, key atoms, descriptor chain; proxy DValue is a strong binding root | Stable proxy object across replacement. Context and original owning binding must survive it. No proxy finalizer releases context storage. |
| `window_identity` | Binding alias of context proxy DValue; engine global-this hook holds the proxy | Alias is not an independently allocated/rooted record. Prepared candidate and raw storage remain distinct; engine global-this and all native aliases must stop using the proxy before its backing ends. |
| `DWindowDescriptor` | Cache keyed by source binding, target binding and key; held DValue roots a three-part descriptor array, containing functions/values | Source and target pointers cannot rebind; the target anchor belongs to that cached target, while context-following members separately select current document state. Functions have engine creation-context holds and target anchor data. Cache identity is target-specific across publication; do not reuse an old cell for a new target. |
| `DLocationExotic` / `DLocationDescriptor`, `location_exotic.inc` | Relevant binding, ordinary backing DValue, default/key atoms; cache keyed by source binding and key with rooted function DValue | Functions retain relevant anchor and Location in data; source creation realm and relevant binding survive cached or exported functions. Location identity stays relevant-global-specific, even as navigation target changes. |
| WindowProperties, `window_names.inc` | Opaque pointer to binding-owned backing DValue; ordinary prototype edge engine-traced | Retained name object remains tied to original document; replacement does not retarget it. Live HTMLCollections and wrapped nodes have separate existing binding/creation-owner obligations. |
| `DNavigationRequest`, `navigation_authority.inc` | Pending context request captures source/target and generation, URL and policy inputs, charged directly to the cluster | Request is not a lease to future active target. Dispatch rechecks expected target generation, eligibility and policy. Replacement/discard invalidates the pending destination; dispatch clears the pending kind but keeps the allocated cell charged until cluster free. |
| DOM aliases/events/callback receivers | JSValue roots/engine edges returned by `d_window_identity` and event conversion paths | A returned proxy can escape its immediate native caller. A zero native cache count does not establish that escaped references ended. Raw globals, callbacks and documents keep their own relevant identities. |

The inventory is semantic, not an additive refcount formula: aliases of one
DValue do not create extra roots, and a DValue root, engine function context
edge and checked owner hold are different edges. F15 must avoid both counting
one root twice and using one edge as proof that another does not exist.

## Snapshot inputs and unavailable states

A consumer observes these facts from the original retained records, with no
script/provider callbacks between validation and copy:

| Input | Native source / meaning |
|---|---|
| Stable context | Membership in `cluster->contexts`; identity is the record, not its mutable name, active binding or generation. |
| Publication | `generation` starts at 1; `active`, `window`, `discarded`, parent/live child links. The target query requires expected generation and `d_delivery_active`. |
| Relevant owner | Original `os64_dom_t::realm`, copied immutable `binding_origin`, and object-specific original binding. An owner ID must map that realm, not `context->active` by convenience. |
| Settings roles | Checked wrapper CURRENT, NATIVE_CALLER, ENTRY and INCUMBENT at their operation boundary. Missing/retired/unregistered settings refuse authority. |
| Known native state | Proxy existence and owner; target-specific Window descriptor cells; relevant-global Location cells; pending navigation cell/kind; exact category presence/count if the adapter can inspect it. |
| Unknown state | No complete escaped-JS exposure history, graph reachability or all-native-borrower census exists. A missing enumerator is unknown, never zero. |

A failed expected-generation check refuses the operation; it must not silently
return the current target under the old request. An active binding with no
context is **unassociated**, not discarded. Discarded context snapshots retain
identity, generation, ancestry and last Window, but have no active target.
A separately retired/failed document can make the active pointer ineligible
without a new context publication; copy both publication facts and lifecycle,
rather than treating non-NULL as usable. Drained/poisoned cluster or unavailable
runtime metadata refuses a live observation. Snapshots contain scalar copies
only and do not keep any resource alive.

The agreed F15 selected adapter gives contexts numeric names through explicit
registration: a nonzero, nonreused monotonic serial scoped by runtime incarnation,
with a bounded charged mapping from validated native context identity. This is
new adapter metadata, not an existing F4 numeric issuer. Register only while
cluster membership and grouped lifetime are established; retain entries through
root teardown, and refuse capacity/issuer exhaustion before exposing an ID.
Never cast an address to a numeric ID or use publication generation as identity.
The implementing F.15.22 PR must establish the bound, storage charge and issuer
failure proof; this contract does not claim that registry is implemented.

Original owner mapping must preserve the compatibility path: existing raw realm
admission/getters are not exempt from the sticky unknown-retention veto.
A selected test host can explicitly convert a known live realm through
`js_owner_runtime_realm_ticket`, copy its original `JSOwnerRealmSnapshot`, and
close the temporary holder. Output publication follows successful mapping;
capacity refusal leaves output unchanged. Default DOM runtimes have no selected
owner arena and cannot fabricate a mapped owner. Registration/mapping is fallible
preparation; subsequent copied observation does not create an owner/root or
change a target. A later checked DOM admission family would require its own
reviewed migration contract before avoiding legacy raw retention.

## Quiescence and storage end

`d_browsing_quiescent` first requires `os64_js_quiescent`, then refuses a
poisoned/drained cluster and checks `cluster->recovering`, `provider_borrows`
and `dispatching`, each member's `provider_borrows`, and `departure_busy`.
The latter includes departure finishing, dispatch/current event, animation and
Web Animation invocation, idle invocation, pointer running/canceling/draining,
click/submission, custom reactions and observer/intersection/resize delivery.
The wrapper additionally excludes open execution/settings/task/setup and queued
microtask/finalization work. These are operation barriers, not exhaustive pins
or a release census. A busy observation is refusal, not a zero-borrow snapshot.

Cluster drain closes entry doors before releasing callbacks in members. Binding
drain releases exotic atoms, callback records and rooted DValue values while
the engine is alive. Engine destruction follows. Native binding free then
releases cache/proxy/Location records, origins and DValue storage without engine
calls; binding shells survive all native sweeps so cross-member accounting stays
valid. Cluster free releases shells, navigation cells and context records,
then page/HTML groups. The adapter must detach any live record references before
their storage end; copied IDs/facts can survive, but later resolution refuses.
A registration cannot extend native storage beyond this root teardown contract.
The adapter closes registration and live observation before cluster drain;
reentry during drain refuses before dereferencing a stored native identity.
Its mapping storage survives any engine finalizer that its implementation allows
to inspect copied diagnostics, then ends before native context/binding storage
is freed. A host-owned adapter must not outlive its host; a runtime-owned adapter
must finish its native-reference cleanup during runtime destruction. After
runtime destruction, retained numeric diagnostics cannot resolve an owner or
consult its arena. The F15 implementation must choose and prove one storage
owner/order, rather than relying on this choice to remain implicit.

## Reentry and failure

A native pointer or generation-qualified query is a borrow, not a pin across
arbitrary script/provider reentry. Current implementation backing survives via
grouped retention and existing wrapper/provider guards. Any future export family
must retain the owners required through final value release, including native
finalizers, and revalidate operation authority and target generation after
reentry. Clear invalidates the public key before freeing rooted values; metadata
and owner holds survive finalizer reentry until cleanup finishes. This ordering
is compatible with F15's bounded callback-root family, but does not classify
engine-traced cycles as C roots.

Publication requires wrapper and native quiescence, no pending jobs or cleanup
tasks, a matching generation, an eligible privately prepared candidate and
headroom for descendant generations. All fallible preparation precedes the
allocation-free commit. Refusal leaves old publication intact; engine failure
still requires its specified recovery/root cleanup. Successful replacement
retires the old target and descendants, publishes the new target and increments
generation. Provider departure cleanup follows and can require retry; its
failure cannot be reported as rollback of an already committed publication.

Descriptor/cache construction publishes only after its value root succeeds.
Partial allocation is released without inserting a cache cell. A retained
cached function is checked again when invoked; caching is not authorization.
Discard keeps the last Window and origin for tombstone/property policy, removes
live child links, and disables active document delivery. Closed/parent/length,
Location and cross-origin answers follow their individual F4 contracts; there
is no single rule that every operation on a discarded context throws.

## F15 consumption and explicit unknowns

F.15.22 must map its checked owner identity to the original binding/realm and
keep publication generation separate. It must identify which of the inventory
rows its record actually represents, who owns its storage/root, how clear and
failure unwind work, and which native invocation borrows remain protected.
Unknown categories stay unknown in snapshots; they cannot become zero counts.
Counter exhaustion or unavailable owners refuse admission before publication.
No weak ticket may substitute for a strong value root or a required settings hold.

C3 supplies no complete census of raw ABI handles, engine cycles, DOM creation
owners/adopted nodes, provider tasks, native exports, or user-held JS values.
Those dependencies remain release vetoes until their implementing contracts
and independent evidence cover them. Zero known exports, no current target,
retirement, a drained callback family or a complete scalar snapshot each remain
insufficient for reclamation. Existing cumulative bounds and grouped teardown
remain in force.

Acceptance for this design is source reconciliation and F15 agreement, with
F.4.38 retaining the integrated security/lifetime proof. F.15.22 owns the
implementation and failure tests for the representation it consumes.

## Coordination and required consumption proof

F4 and F15 agree on the bounded selected-adapter registration above, explicit
compatibility owner mapping and unchanged raw-retention veto. F.15.22 owns the
adapter implementation; no separate F4 numeric issuer is needed for that scoped
consumer. F.4.37 closes the source contract, not F.15.22 or F.4.38.

The consuming PR must cover at least these counterexamples:

| Transition | Required observation |
|---|---|
| Same context, replacement document | Stable numeric context ID; new publication generation/target owner; retained relevant Location owner unchanged. |
| Rename, including root reset | No context-ID reassignment; name mutation alone changes no publication generation. |
| Retire active binding without context publication | Same generation, ineligible lifecycle; no fabricated active permission. |
| Discard | Original context ID and last Window remain distinguishable from absent active target. |
| Missing/default owner arena or mapping refusal | Output unchanged; no fake numeric owner, no removal of raw compatibility retention. |
| Register second cluster/context, then destroy/recreate runtime | Distinct serials within incarnation; old copied identity cannot name a new runtime/context. |
| Full map or exhausted issuer | Refuse before registration publication; no eviction/rebinding of retained entries. |
| Provider/dispatch/settings reentry or drain | Refuse live observation at the defined barrier; no stale native dereference or zero-count release claim. |
| Cached/exported JS function survives target departure | Known roots/aliases are classified without claiming a complete exposure census; unknown graph/native categories still veto release. |

Source reconciliation covers `browsing_context.inc`, `navigation_authority.inc`,
`window_proxy.inc`, `location_exotic.inc`, `window_names.inc`, `core.c` drain/free
and member checks, and runtime quiescence/owner-ticket contracts. Local links,
tracker and stale-reference checks apply. This document adds no runtime or
hardware test receipt; implementing evidence remains pinned to the consuming
PR and the existing F4 briefs.
