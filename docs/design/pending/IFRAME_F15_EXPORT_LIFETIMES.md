# F15 C1/C2/C4 — proposed checked export lifetimes

Proposal baseline: merged `userland` `2a2420b7`, including #621 and the import
provider work in #622. Engine identity is
`83e3863bc254f13fe66b3d20ae99edaac4cb1794e9f9bac2672c2fafb6330f43`,
embedding ABI 5, bytecode format 6.

This makes the [C compatibility requirements](IFRAME_F15_IDENTITY_CONTRACT.md)
concrete enough to review a migration choice. The recommended choice is a
separate opt-in lifetime profile with checked owner tickets and scoped loans,
plus conservative compatibility retention when legacy exports escape.
The following rules are proposals, not implemented facilities. They reserve
no API names, C layouts, identifier widths, ABI revision or engine patch.
They grant no permission to release roots, free owners or reuse realm slots.

## C1: negotiation and compatibility

Select the checked lifetime profile during construction, before publishing
the runtime or an owner. Negotiate its operation contracts explicitly; an
unrecognized profile refuses construction without publishing handles. Do not
enable it late on a runtime that may already have issued borrowed pointers.
Existing ABI 5 constructors and operations retain their current defaults,
lifetimes, capability choices and cumulative limits. The CLI and existing
embedders do not enter the new profile implicitly.

In the proposed profile, persistent owner references are copied identifier
values called tickets here. A ticket identifies its runtime incarnation,
logical owner and holder record independently of the address of a reusable
metadata cell. These are semantic identity components, not a C encoding.
Operations take a caller-supplied live runtime and validate against that
runtime's incarnation and holder table. They do not dereference a runtime
pointer recovered from an obsolete ticket. A ticket from an earlier runtime
must fail against a new live runtime even if an allocator reused its address.
Passing an already destroyed runtime pointer remains an invalid native call;
this proposal does not add validation of arbitrary freed pointers.

A valid ticket preserves its promised owner lifetime. It does not establish
that the owner is executable, healthy, current, same-origin or authorized for
an operation. Existing lifecycle/budget checks and F4 security checks still
apply. In this initial proposed profile the primary owner remains
runtime-lived; the progress target concerns child owners, not primary replacement.

Legacy operations remain available with their existing contracts. Before a
legacy raw export leaves the checked boundary, record compatibility retention
for the resources its promise can require. Reserve that bookkeeping at owner
admission: an existing allocation-free context getter cannot acquire a new
allocation/failure step merely to record its export. A repeated legacy read
sets existing state; it does not append a new holder record each time.
Compatibility retention lasts until runtime destruction, without a new
operation that silently shortens an ABI 5 borrow.

Pinning only the returned descriptor is insufficient. A raw realm handle can
later be used to obtain its context; a context can expose globals, intrinsics
and foreign values; native value finalizers can require other owners' ledgers.
Per-owner attribution is usable only with a complete transitive ownership
proof. Until such attribution exists, an unclassified raw escape sets a
runtime-wide compatibility veto on per-owner release. This conservative
fallback preserves behavior; it does not promise bounded progress for a host
that continues to make untracked exports.

This compatibility veto is retention state, not engine failure or activity
state. It does not change existing execution admission, F16 READY/WAIT/root
classification, cancellation/failure records or root teardown. Its effect is
to prevent an unproved F15 per-owner release or reuse.

An internal call site is not exempt merely because it is in libdom. Existing
uses of `os64_js_realm_context`, setup/settings callbacks, child-admission
results and native providers must migrate to an explicitly scoped borrowing
contract before a checked-profile proof treats them as non-escaping. Calling
an existing public raw-export path retains its legacy promise. A new profile
alone does not reinterpret old callback signatures or provider registrations.

## Ticket and invocation rules

| Proposed operation | Metadata result | Lifetime consequence |
|---|---|---|
| Create an owner and its first ticket | Reserve owner/holder capacity and issue checked identities before publishing either. | Metadata refusal publishes neither owner nor ticket. Later engine/native construction failure retains any consumed backing/slot according to its existing failure policy; it does not fabricate a usable ticket. |
| Copy a ticket value | No new holder record or implicit retain. | Copies alias the same ticket; closing it invalidates those copies for new operations. |
| Explicitly duplicate a live ticket | Reserve a distinct holder cell and fresh holder identity before returning it. | The duplicate is independently closable. Exhaustion leaves the original ticket unchanged. |
| Resolve a ticket | Validate the supplied live runtime/incarnation, holder identity and original owner, then apply operation admission. | A closed, foreign or obsolete ticket refuses; it does not select the current occupant of an old cell. Resolution is not a security or reclamation certificate. |
| Admit an invocation loan | Reserve a bounded loan and keep the required owner resources stable before exposing a scoped view. | The loan uses the enclosing guard/budget; it cannot open a replacement task or deadline. |
| Close a ticket | Invalidate that ticket before removing its holder obligation. | New operations and duplicates through aliases refuse. Independently duplicated tickets and already admitted loans survive. |
| Close an already closed/obsolete ticket against a live runtime | No effect on another holder, including one using the recycled cell. | Return a defined stale/already-closed outcome; do not decrement the replacement's counts. Exact public status codes remain unselected. |
| End a loan | End that scoped view and remove its loan obligation. | Retaining a raw view beyond the loan violates the checked contract; it does not turn the view into an ABI 5 runtime-long borrow. |
| Destroy the runtime | Invalidate its tickets and follow the existing root teardown policy. | Idle tickets do not extend runtime lifetime. Active engine/provider/native users still obey the existing teardown order. Value-only checked diagnostic data can remain readable without resolving an owner. |

If native reentry closes the ticket that admitted the current invocation,
the invocation continues through its independent loan until unwind. It cannot
reuse the closed ticket to start another invocation. Promotion from a live
loan to an independent ticket is an explicit reservation, with its own local
capacity refusal; copying a raw view does not promote it. Closing one ticket
does not close an explicit duplicate or cancel unrelated work. It does not
revoke independently retained script functions, callback registrations or
native objects; their ordinary/failed-realm behavior keeps its existing
contract.

A scoped context/value view must not be retained or duplicated into an
untracked native owner. A host needing an existing raw API's longer promise
must enter the compatibility-export path before that escape. Persistent raw
`JSValue`, module and binding exports remain legacy/unclassified until their
families have a reviewed migration. This proposal does not introduce a new
persistent engine-value-root ticket facility.

The metadata loan surrounds the existing engine/settings invocation; it does
not replace that scope's context/value holds. Complete engine-scope unwind
and its value/finalizer cleanup while the metadata loan still protects the
native owner, then end the metadata loan. Do not drop that protection before
cleanup merely because the admitting ticket was closed.

Ticket close and metadata-loan end change holder metadata only. They enter no
engine code, invoke no callbacks, and allocate/free no records; cells return
to the reserved pool. They do not call `JS_FreeValue` or `JS_FreeContext`,
collect garbage, execute jobs or author registry cleanup, detach providers,
or acknowledge layout loans. Their last holder going away merely removes one retention reason. Root release can run
synchronous native finalizers and therefore needs the later family-specific
guarding, owner lifetime and partial-state proofs. Keeping this limitation is
essential: a ticket table is not a reclamation implementation.

## C2: bounded metadata and refusal

Use separate finite budgets for owner descriptors, explicit holder records
and active invocation loans, with their storage charged before publication.
For the initial contract, owner capacity must not exceed the existing
`OS64_JS_MAX_REALMS` boundary; it includes primary, retained and failed/private
owners. F4c's context-record cumulative cap remains a separate constraint.
This proposal does not raise either cap or enable owner-slot reuse.

Choose holder/loan capacities explicitly at construction and account for
their full reserved bytes. Do not grow a hidden registry on ticket copies,
legacy getter calls or close history. When a holder cell is free, it can serve
a later ticket with a fresh identity; preserving the old ticket's invalidity
must not require one tombstone per historical ticket. A retained owner,
outstanding loan or required security fact cannot be evicted to make room.

Runtime-incarnation, owner and holder identities must refuse exhaustion
before publication rather than wrap, truncate or restart at a recycled
address. Retire an exhausted reusable cell or refuse the operation according
to the chosen issuer policy; do not pretend an old identifier is fresh.
The issuer, counters, free-cell state and compatibility vetoes must themselves
have bounded storage. Identifier formats and numeric capacities remain for
the concrete interface review; the no-wrap/refusal rule is the proposal.

With reserved metadata, ticket duplication and loan admission either commit
their complete bounded record or refuse locally without altering existing
holders, guards, targets or charges. Failure while constructing engine/native
resources still uses those resources' established sticky/local classifications.
A close cannot roll back a finalizer because close performs no root release.

Metadata accounting covers persistent and scratch storage, retained owner
facets and any export names/caches required by an admitted family. F4 history
or exposure state cannot disappear merely because the last ticket closes.
If valid holders or unknown edges occupy the budget, refusal is expected.
Future no-export churn must demonstrate bounded progress without increasing
the cap or accumulating historical owner/holder records; ticket-cell reuse
alone does not prove that workload.

## C4: ownership and migration table

The table separates a holder obligation from an actual engine trace or native
borrow. It does not claim that all these families have been classified.

| Existing surface | Resources its promise can require | Proposed migration/release boundary |
|---|---|---|
| `os64_js_primary_realm`, realm creation/staging/context lookup, `os64_js_context`, settings-role lookup | Stable realm descriptor and the permitted later context/configuration operations; raw contexts can reach globals, intrinsics and foreign values. | Default borrows stay valid until runtime destruction. New checked operations must avoid publishing those pointers; a legacy escape retains transitive resources, or applies the runtime-wide unknown veto. Primary exposure is not harmless merely because primary itself cannot be reclaimed. |
| `os64_js_realm_setup_fn`, settings bodies and other native callbacks receiving a realm/context | The executing context, associated native owner, conversion/invocation data and any resources the current callback contract permits it to retain. | Introduce an explicit scoped contract for migrated paths. Admission protects the invocation even after reentrant ticket close. Old callbacks do not gain shorter lifetimes by being installed in the new profile. |
| Raw `JSValue`, module handles, native functions and callback records | Value roots, executable/relevant contexts, module/rejection provenance, native opaque state and finalizer-owned allocations. A plain object's creation-ID stamp is not itself a context root. | Preserve each existing ownership/release contract, including explicit module release. Do not infer a full creator-context pin from ordinary data, or a complete absence of native ownership from one reference count. Unknown transitive value ownership blocks release. |
| Pointer-free B1/B2/B3 snapshots | Their copied counters/IDs/unknown flags are data, not ownership references; callers separately keep query inputs valid. | Copying them creates no ticket or holder record. Their IDs do not become checked export keys and their zero/complete counts do not certify release. Pointer-bearing failure observations are a different family. |
| Failure observations and recovery records | ABI 5 copied records contain realm pointers valid until runtime destruction; a reader can still use those handles under their contracts. | Keep legacy observations unchanged and retained. Recommend a separate checked snapshot carrying immutable owner IDs and copied diagnostic data, without borrowed owner pointers. Such data can identify a vanished owner without making a new occupant answer for it. F16 episode tokens remain a different namespace. |
| DOM creation/admission results (`os64_dom_t *`, `os64_dom_child_t`) | Binding shells, callable data, HTML/page ownership, creation/prototype services and quota ledgers used by retained wrappers and finalizers. | Checked migration must cover the result and late native wrapping, not just a realm getter. Existing consumed-child retention and root teardown order remain intact; ticket close does not free a binding or group. |
| Adopted HTML nodes, ranges, page models and creation bytes | Original physical arenas and creator lookup/prototype services can outlive the current document, including nodes with no old wrapper. | Keep these as independent native/creation obligations until a reviewed family proves their end. An empty wrapper registry or a closed host ticket cannot authorize replacing `members[creation_byte]`. |
| Engine closures, Promise/thenable jobs, suspended work, ScriptOrModule and FinalizationRegistry | Actual traced contexts/records, values and independently selected callback/report roles; queued data has its own ownership. | Preserve the engine's mark/free and discard contracts. Metadata tickets do not replace those edges or synthesize context pins. Unknown graph edges retain/refuse the candidate; author cleanup is not a release prerequisite. |
| Script/module/import-base providers and DOM provider opaque records | Native storage must remain valid for registered and in-flight uses. The import-base callback receives current settings and returns an owned immutable string; its queued string data does not add a context root by itself. | Preserve `js_callbacks.h` detach/lifetime requirements and DOM's existing departure barrier. A provider loan protects its native opaque storage; it is not automatically an engine-context pin. Scoped migration must cover registration, callback entry and returned data. |
| Required F4c/d context/target, Location and cache holders | An active target or retained relevant-global/cache relationship can require an owner independently of host tickets. The concrete holders are not implemented by this proposal. | Consume F4's explicit lifetime/publication contract. Closing an external ticket cannot clear those relationships; missing C3 inputs refuse dependent release. No F4 pin or cache-key representation is selected here. |
| Borrowed layout/effect generations and in-flight dispatch/reaction walks | Old provider/effect/native storage remains needed through its independent acknowledgment or unwind. | Closing an export ticket neither acknowledges a generation nor ends another loan. Root teardown still waits for the established borrowers; per-owner release needs the corresponding family proof. |

Current internal context-getter call sites in `libdom/node.c`, `event.c`,
`browsing.c`, observer/custom/animation and other units illustrate why migration
cannot stop at a constructor flag. The CLI's default primary-runtime path and
existing embedding examples remain on their existing contracts. An audit
must include callbacks, module/failure results and DOM child returns that
expose raw identities indirectly, rather than exempting a source directory.

## C3: required F4 inputs, not definitions supplied here

Ticket resolution must consume the agreed F4 contract for stable context
identity, active target and detached/discarded answers; source/settings/origin
roles; target publication and stale-request validation; and retained
Location/descriptor-cache/exposure state. The lifetimes and validation points
of those inputs must be explicit through reentry and failure. This proposal
does not choose snapshot fields, generation encodings, cache keys, origins or
publication signatures for F4c/d. Absence of a required input refuses a
dependent operation; a valid ticket cannot substitute for security authority.

## Required implementing counterexamples

| Case | Required result |
|---|---|
| Copy ticket T, close one copy, recycle its holder cell for U | Both aliases of T refuse new work; closing T again leaves U unchanged. |
| Explicitly duplicate T as D, then close T | D keeps its independent holder; plain copies of T do not. |
| Admit a loan using T, then close T during native reentry | The admitted view remains valid until loan unwind; T cannot admit new work. A full holder pool refuses promotion without ending that loan. |
| Destroy one runtime and create another at the same address | An old ticket fails against the supplied new live incarnation. No operation dereferences a stale ticket-owned runtime pointer. |
| Exhaust holder capacity, identity issuer or loan depth | Refuse before publication without wrap or mutation of existing holders/targets. Failed reservation does not create a new usable ticket. |
| Invoke a legacy context/realm getter, copy a raw failure record or publish a DOM child pointer | Preserve its current lifetime and transitive resources. An unclassified escape applies the conservative compatibility veto. Zero ticket counts do not override it. |
| Close the last ticket while a foreign Blob/node/callback or engine job survives | Preserve independent native, creator, settings and engine obligations. No root drop, collection, author cleanup or storage release occurs through close. |
| Retain only a copied checked diagnostic after owner release | Its original identity/data remain immutable; resolving that ended owner refuses instead of naming a replacement. Legacy pointer-bearing records keep their longer promise. |

These cases need actual implementation tests, negative controls and both root
teardown profiles before code can claim the contract. The current PR is a
proposal only; it adds no runtime, target or P5 receipt. Review can adopt the
migration semantics while concrete interfaces, family proofs and C3 inputs
remain pending. D/E and physical reclamation are still gated by the
[F15 plan](IFRAME_F15_RECLAMATION.md).


## Validation and coordination

The F4 lane boundary-read the concrete proposal against existing callback
and import-provider contracts. No blocking mismatch was found; scoped
migration, cleanup-inside-loan ordering and C3's required-input boundary are
preserved. That read does not select C3 fields or authorize release work.

Source anchors for the current exports, CLI/default constructor path,
callbacks, providers, native creation services and detach contracts were
checked. Manifest hashes, local links, exact documentation/owned-row scope,
tracker validation and the stale-reference check pass. Only this proposal,
its overview link and F15's tracker notes change. There is no runtime change
or new runtime/guest/P5 evidence in this PR.
