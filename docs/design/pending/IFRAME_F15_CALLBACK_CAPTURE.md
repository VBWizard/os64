# F15.20 — persistent callback-capture contract and audit

## Baseline task list — 2026-11-19

- [x] Audit legacy callback capture, invocation, clear/trace and native consumers.
- [x] Select one bounded persistent callback family and separate its value,
  incumbent, invocation-entry, reporting and native-storage obligations.
- [x] Specify charged storage/identity, capture/invoke/clear, atomic refusal,
  tracing, reentry, failure and root-teardown requirements.
- [x] Record named counterexamples and a coherent F.15.21 implementation plan.
- [x] Coordinate roles and ownership with F4 while preserving the C3 gate.
- [x] Validate source anchors, local links, owned tracker row and documentation
  scope; prepare publication for Codex review before implementation.

These are this contract task's baseline audit/delivery steps. Discoveries that
change the work list will be recorded separately as **ADDED** with their date
and reason. Completing F.15.20 does not implement F.15.21 or close F15.

## Scope

Contract PR **#675**, audit source `76eba995`.

Contract/audit only, based on merged userland `2816491f`, including #667/#668,
tracker #669. No runtime, engine, native
layout, public API or ABI changes are part of this task. C3/public activation,
physical reclamation, provider release and resource/realm-slot reuse stay gated.

## Audit baseline and existing obligations

This proposal specializes [C1/C2/C4](IFRAME_F15_EXPORT_LIFETIMES.md) after
[checked settings invocation](IFRAME_F15_CHECKED_SETTINGS.md). It describes
semantics for review, not implemented operations. It reserves no C API name,
layout, identifier width, capacity default, ABI revision or engine patch.
Existing constructors, callback records and their ABI 5 lifetimes are unchanged.

The source audit is pinned to `2816491f`; historical F4/F15 receipts keep their
own baselines and exclusions. No new runtime, sanitizer, guest, WPT or P5
receipt is claimed by this documentation task.

| Source and search anchors | Existing ownership/lifetime | Consequence for this family |
|---|---|---|
| [callback header](../../../userland/libjs/include/os64/js_callbacks.h): `os64_js_callback_t`; [runtime](../../../userland/libjs/runtime/runtime.c): `os64_js_callback_capture`, `os64_js_settings_realm` | Capture duplicates one object value and stores a runtime pointer and borrowed incumbent realm pointer. The public settings query records sticky compatibility retention. There is no independent owner-ticket hold in this legacy record; wrapper contexts remain runtime-lived. | Do not reinterpret or retrofit that record. A new checked capture needs its own selected-incumbent holder and an explicit bounded owner for its persistent value. |
| Runtime: `callback_settings_body`, `os64_js_callback_with_settings`, `realm_call`, `os64_js_callback_call` | Invocation snapshots the held value before reentry. Associated entry, captured incumbent, executable callback realm and reporting owner have separate selection rules. The raw with-settings path returns an owned result/pending exception to its caller. | Preserve the raw paths. A new no-result invocation can use #667's scoped settings/cleanup semantics; it must not claim to replace the reporting or callback-type contracts. |
| [timer.c](../../../userland/libdom/timer.c): `timer_method`, `release_values`, `os64_dom_timer_fire` | Native timer records hold callbacks and arguments. Intervals take independent invocation references; one-shot delivery transfers the table's ownership before freeing its record. Delivery has separate task/activity and reporting owners. | Timers illustrate C roots, but migrating them also requires arguments, DOM/native storage, clock/reentry and reporting proofs. No timer migration is selected here. |
| [event.c](../../../userland/libdom/event.c): `d_event_target_mark`, `d_event_target_finalize`, `set_callback_record` | Registry-held and engine-held EventTargets differ. Engine-held targets mark their listener callback edge; selected captured/value fields can alias one owned reference. Removal and once delivery preserve an active invocation hold. | Do not count aliases as extra roots or move engine-held listeners into a runtime C-root table. |
| [report.c](../../../userland/libdom/report.c): `d_synchronous_capture`, `scheduled_capture`, `d_synchronous_clear`, `d_scheduled_clear` | The shared capture structures combine value ownership with borrowed entry/incumbent facts. Clear first empties the holder. Consumers own the surrounding native storage and its accounting. | Value, settings-owner, containing-storage and trace obligations must be named separately. A copied struct is not a new independent ownership reference. |
| [observer.c](../../../userland/libdom/observer.c): `observer_mark`, `observer_finalizer`; [streams.c](../../../userland/libdom/streams.c): `StreamCallback`, `stream_callback_mark`, `stream_callback_free`; [walker.c](../../../userland/libdom/walker.c): `walker_mark`, `walker_finalizer` | Engine-held callback edges participate in mark/free pairing and collectible cycles. Their native containers can have other registrations/roots. | Keep these families engine-traced. A permanent runtime C root would change cycle liveness; numeric owner tickets do not supply the missing engine edge. |
| [associated-realm patch](../../../userland/libjs/patches/0030-callback-associated-realm.patch): `JS_GetCallbackRealm`, `creation_realm`, `JS_DupContext`, `mark_func`, `JS_FreeContext` | A callable proxy retains its associated creation context after target revocation; bound/proxy association differs from executable-target discovery. Ordinary callback objects have their own type-specific association rules. | A value root is not permission to guess its associated entry or to attribute all its transitive storage to one owner. |
| Runtime: `os64_js_destroy_report`; [teardown contract](../../../userland/libjs/TEARDOWN.md) | Existing host C roots drain before destruction. The wrapper guards teardown, releases owned roots/contexts, performs the fatal/reporting engine verdict, and keeps owner metadata alive through engine finalizers and tracked allocator bailout. | Any new wrapper-owned C-root family must add an explicit pre-context drain, not rely on engine bailout to reclaim an otherwise live root. Legacy external C roots keep their existing drain obligation. |

The existing [callback fixture](../../../userland/libjs/tests/callback_settings.h)
and [F4 capture validation](IFRAME_F4_CALLBACK_CAPTURE_VALIDATION.md) establish
useful legacy counterexamples: conversion-time incumbent, object callback
lookup, self-removal, finalizer reentry and collectible traced cycles. They do
not prove a bounded numeric callback registry, new constructor accounting,
record reuse or the proposed managed-root shutdown.

## Recommended first family

Select a private, runtime-owned registry of **C-root callback records** before
runtime publication. Each populated record owns one callback-object reference
and one independent holder for the captured incumbent's original owner.
It publishes a copied numeric record key, not an address, raw value, realm or
context. The first family adds no general persistent-value getter and no
persistent native opaque pointer or retained argument list.

The host validates its callback type before capture. The retained value must
be a live object from this runtime's engine, already borrowed/rooted by the
caller for the operation. This is a trusted native-value precondition, not
validation of arbitrary `JSValue` bits, a foreign-engine value or a freed scope.
The storage operation does not inspect properties or derive a function realm;
object callback-interface lookup belongs inside the later invocation body.

The host keeps a separately established associated-entry ticket for as long
as it promises later invocation. The record itself does **not** own that entry,
a reporting/activity owner, a DOM binding, creation arena, provider opaque or
physical payload ledger. Losing the host's entry holder can therefore make a
future invocation refuse while the captured callback remains closable.
A record key cannot reconstruct or replace that missing obligation.

For example, capture may occur with incumbent A while the callback object's
associated entry is B, its wrapped executable target is C, and task/reporting
owners are D/E. The registry owns the A hold and callback value. The host
supplies a valid original-B ticket at invocation; engine tracing still owns
its actual code/value edges. Task/reporting/creator/native obligations remain
separate, and none of these identities supplies origin or target authority.

This is a C-root storage family, not an engine-object ownership adapter.
Runtime root accounting must not present these references through
`os64_js_callback_mark` as if they were internal engine edges. Conversely,
engine-held observer/Streams/listener cycles must not enter this registry by
an implicit conversion. They remain later family work under F.15.23/F.15.25.

## Identity, bounded storage and accounting

A record key semantically identifies its runtime incarnation and fresh record
identity. Record identity is a namespace distinct from owner IDs, holder/loan
IDs, F16 episode tokens, HTML creation IDs and F4 publication generations.
Its encoding and issuer implementation are for F.15.21 review. Issuance must
refuse exhaustion before publication, without wrapping or using a cell address
as freshness. A key from an earlier runtime fails against a supplied new live
runtime even if its address was reused; using a destroyed runtime pointer
remains invalid native code.

Reserve a finite record pool and its bookkeeping in the explicit private
construction path. Charge actual allocator capacity and alignment, persistent
record/issuer state and required bounded scratch before publishing the runtime,
following [charged admission](IFRAME_F15_OWNER_ADMISSION.md). Refusal publishes
no runtime or callback key. Legacy constructors select no new pool and keep
their existing defaults/allowances. No public profile negotiation is implied.

The record pool, incumbent-holder pool and invocation-loan pool are separate
finite resources. A free record cell does not manufacture a holder or a loan.
Capture is allocation-free after selected construction; it consumes a record
cell, fresh key identity and independent incumbent holder atomically. No hidden
list grows on key copies, failed attempts or close history. Returned record
cells remain part of the reserved charge until actual storage is freed.

Registry storage charges do not transfer the callback's reachable engine or
native payload charges to its incumbent. Engine allocations remain with the
runtime allocator; physical native bytes remain with their original ledgers.
The implementing proof must distinguish reserved cells, populated/clearing
records, persistent roots/holders and temporary invocation references; an
unbounded deferred-close list or uncharged scratch is not an alternative to
these budgets. Zero record counts still do not certify the transitive graph.

Copying a record key aliases one registration. Close invalidates its copies
for future use; it neither closes another capture of the same value nor
revokes an already admitted invocation. Independent persistent duplication is
not selected by this first family. A separate capture owns a separate root
and incumbent holder and consumes their capacities explicitly.

## Operation contracts

### Capture

Require a live selected runtime, matching caller ABI, valid invocation-only
scope and active wrapper settings frame, with distinct caller output outside
runtime/metadata/active outcomes. The caller supplies no incumbent override:
select conversion-time incumbent from the actual engine settings, using the
scalar mapping boundary from [#665](IFRAME_F15_SCOPED_SETTINGS.md). Require an
assigned, healthy, eligible original realm. ABSENT/UNMAPPED, staged, retired,
failed, setup/destruction, bad ABI/type/output or exhausted metadata refuses.
Do not substitute the admitting scope owner, entry, current or primary.

Peek the existing failure/cancellation/allocator flags without polling a new
clock or consuming a pending exception, as legacy capture does. Capture is
not an execution entry and cannot renew deadline/jobs or make a failed runtime
usable. A still-unobserved elapsed deadline remains for the enclosing guard's
ordinary health observation; this operation creates no exemption from it.

Before publication reserve the complete record identity/cell and an independent
holder for the selected incumbent owner. Promotion from the admitting loan is
insufficient when that loan names B but the incumbent is A. Duplicate the
borrowed callback value, commit the record, then publish its key. Reservation
failure leaves the caller output, existing records/holders/loans, pending
exception and legacy retention flag unchanged. Roll back any partial metadata
reservation; no root or holder may remain hidden on refusal. Fresh issuer
serials consumed by a failed attempt need not be recovered; never rewind an
issuer or reuse a published/consumed identity to make rollback appear exact.

A successful checked capture creates the named C root/holder obligation; it
is not an untracked raw export. It neither clears an earlier compatibility
veto nor classifies the callback's transitive native/foreign graph as complete.
Incidental public getters/capture calls inside a checked scope keep their
legacy sticky retention, without an ambient exemption.

### Invoke

The caller supplies the live runtime, record key, independently established
original-entry ticket and an invocation-only native body/opaque. The body
borrows the held callback object, entry context and checked scope; it returns
one owned temporary value or the pending exception for wrapper diagnostics and
cleanup. No raw callback/context/value/result getter is provided. Retention
beyond the invocation needs an explicit reviewed export path; the scope and
native opaque do not become persistent by being queued elsewhere.

Resolve the live record and entry, check ABI/outcome aliases and the existing
entry/incumbent/runtime lifecycle/health rules. Unknown or ineligible owners
refuse; a valid record is lifetime metadata, not authority. Admit independent
entry and captured-incumbent loans, including when they name the same owner,
and snapshot a temporary callback reference before exposing the body or doing
any potentially reentrant engine work. Any pre-body refusal unwinds acquired
loans and temporary roots with the required owner/storage still alive.

Use [#667](IFRAME_F15_CHECKED_SETTINGS.md)'s guarded explicit-settings semantics:
borrow an active guard or enter an open task, keep its owner/deadline/job counts,
run no checkpoint, and select no reporting hook. The implementation may need
an internal common dispatcher so snapshot/loan admission and unwind compose
once; blindly adding a second layer of role loans would change capacity.
No concrete helper or signature is reserved by this proposal.

Snapshot the callback and captured-owner obligation before author reentry.
The body may close/reuse the record key, close original host tickets, collect,
or install a new record. The admitted invocation continues with its original
value and owner loans; it must not reread a record cell that may name a new
registration. New calls through the old key refuse. Result, exception
diagnostics/coercion and temporary callback release finish inside the selected
settings frame and guard while both loans remain alive. End role loans after
that unwind and any guard opened here; a borrowed outer guard remains the
caller's. The registry does not run deferred author cleanup or extend this
scope into queued work.

The associated entry is not inferred from captured incumbent, executable
function target, registration binding, current document or task owner. A
native binding still proves its callback-type association, conversions,
receiver/arguments, reporting policy and C3/security rules. Its executable-
target/activity eligibility checks remain in place: an entry/incumbent loan
cannot authorize otherwise ineligible code. Active script
frames can override the incumbent backup under the existing engine rules.

### Close and root shutdown

Close is owner-thread cleanup, usable after retirement, cancellation or sticky
failure and during valid synchronous native-finalizer unwind. Validate the
live runtime/ABI/key before touching its payload; a stale, foreign or malformed
key has no effect on another registration. Repeated close has defined stale
or already-closed semantics, not a decrement against a replacement occupant.
It polls no health/clock, allocates no records, consumes no exception and opens
no execution budget. Ordinary native finalization still runs when values free.

Invalidate the record key before releasing its value. Detach its payload into
bounded cleanup state, retaining the record's independent incumbent holder
and containing storage through `JS_FreeValueRT` and synchronous finalizers.
Do not release that holder merely because the externally visible record looks
empty. Keep a clearing cell unavailable to capture until its cleanup finishes;
no allocation or new holder/loan reservation may be required to close it.
Use a cleanup guard (or the caller's existing guard) that refuses ordinary
wrapper execution while permitting the established native cleanup operations.
Native finalizers retain their engine contract; the guard does not validate
arbitrary direct raw-engine reentry by untrusted C code.

After value/finalizer cleanup, close the detached incumbent holder and return
the record cell to its reserved pool. Other independently captured records,
active invocation roots/loans and transitive graph/native obligations survive.
Closing an invocation's record does not end its loans, detach providers,
acknowledge effects or release a realm/context/native owner.

Root shutdown disables new capture/invoke, retains the wrapper cleanup guard,
and drains the selected registry's owned C roots **before context/engine
teardown**, while owner metadata and required native shells/ledgers remain
live. Remaining records can be cleared by valid synchronous cleanup; each key
is invalidated before its own payload release. The drain must be bounded,
reentrant-safe and allocation-free after construction. Capture/invoke remains
refused during the drain, including from native finalizers; recursive close
may only consume existing registrations or observe their stale/clearing state.
This prevents shutdown from chasing newly published managed roots. Its bounded
scratch/native-stack policy must be chosen with construction capacity, not
left to an unbounded recursive cleanup chain. It must tolerate
cancellation, failed realms and a full holder/loan pool. It neither executes
author finalization tasks nor relies on the reporting allocator bailout to
release managed roots. Free registry/owner metadata after engine finalizer/
tracked teardown unwind, preserving actual charges until storage is freed.

Keys do not extend runtime lifetime. Native users stop before root destruction;
old numeric keys cannot be used to touch its freed storage. Legacy callback
records/external C values still require their existing host drain and must
continue to trigger fatal/reporting leak evidence when actually leaked.
Draining this wrapper-owned family is not permission to suppress such reports.

## Named implementing counterexamples

These are required F.15.21 proofs, not results of this contract PR. Exercise
both engine allocation granularities and fatal/reporting root policies, with
ASan/UBSan and supported outside-sandbox LeakSanitizer enabled. Compilation
errors, sanitizer faults and invariant aborts alone do not replace named
behavioral negative controls.

| Case | Required observation / faulty variant |
|---|---|
| Incumbent A, admitting/entry B, wrapped executable C | Capture retains original A and the actual callback value. Invocation enters supplied original B; actual code and reporting stay independent. Reject promoting B or using entry/current/primary as incumbent. |
| Object callback with getter/return coercion in another realm | No properties inspected at capture. The trusted type-specific body performs lookup/call/conversion inside the selected settings frame; it cannot silently become a generic callable-only or property-discovery API. |
| Capture followed by host-incumbent holder close and GC | Record's independent incumbent holder and value root remain. Host still retains a separate entry ticket. Reject a copied/aliased incumbent holder or omitted C root. |
| Full record pool, holder pool, or exhausted key issuer | Refuse before key publication with no root/holder leak, pending-exception consumption or output mutation. Roll back a partially reserved cell/holder; later admissible capture still works. |
| Failed runtime, missing/foreign/unmapped incumbent, staged setup | No fabricated primary or admitting-owner fallback, no callback publication, no new clock/budget. A foreign record key/ticket refuses; arbitrary foreign-engine raw value bits remain outside the native input contract. |
| Invoke with insufficient entry/incumbent loans or settings nesting | No body/value export. Temporary snapshot and partial loans unwind; original record remains live and existing task/source/deadline/job counts remain unchanged. |
| Close/reuse record and host tickets during invocation | Old aliases refuse and a replacement has a fresh identity. Active callback/settings/result/exception/finalizer cleanup uses original roots/loans; no cell reread or early owner release. |
| Close idle or during failed/finalizer cleanup with pools full | Record key is already stale when its value finalizes; incumbent holder and clearing storage are still alive. Recursive close is harmless, cell cannot be reused early, no extra loan/clock/allocation is needed. |
| Same callback captured twice; copies of one key | Independent captures own independent references/holders. Closing one key invalidates its aliases but not the other capture or admitted invocations. |
| Root destroy with managed records still populated | Drain owned C roots before contexts/engine; native cleanup sees live metadata. Final reporting policy has `leaked == false` and both target heap and host native/engine accounting balance. A deliberate unrelated legacy raw-root leak must still be detected. |
| C-root versus engine-owned cycle | A C-root record survives collection until explicit clear/root drain; cleared cycles become collectible. Existing traced observer/Streams/listener cycles stay collectible without an added runtime root or double-counted GC edge. |
| Copied stale key, cell reuse, new runtime at the same address | Obsolete keys never name replacements and stale close changes no replacement counts. No wrapped serial, address-based freshness, per-close tombstone or inferred security authority. |
| Native callback/foreign payload, retained job or adopted owner survives | Callback close is not a complete graph certificate. Independent engine/native/provider/creation/F4 obligations retain/refuse release; the test must not reclaim leaked roots and call that success. |

Actual target validation must link the matching runtime/engine and native
fixture, verify source/object inputs, require an exact parent-observed badge
and target heap integrity/accounting, and freeze the small tested executable/
library cohort with a SHA-256 manifest. A private host proof is not F11
browser/P5 acceptance or a public checked-profile receipt.

## Coherent implementation plan and remaining gates

F.15.21 should implement and prove this **private C-root family** together:
explicit charged admission, bounded record/key lifecycle, capture, no-result
scoped invocation, close and pre-context root drain, with the counterexamples
above. Keep related admission/cleanup proofs in that PR; split a coherent
implementation further if evidence requires it rather than publishing an
unusable half-contract. Concrete signatures/storage and exact refusal codes
are selected in that implementation brief after review of these semantics.

Do not migrate existing DOM timers/listeners/observers/Streams or replace their
mark/free paths as part of that first family. Public negotiation and required
host migration remain F.15.24; persistent export and traced-holder families
remain F.15.23; native creation/provider ownership remains F.15.25. F4 defines
the context/target/exposure contract consumed by F.15.22 (dependency F.4.37).
No record lifetime or empty-count observation substitutes for those inputs.

F.15.26–F.15.29 still require complete conservative eligibility, guarded
per-family release with honest partial states, safe resource/realm admission
reuse and bounded progress under retained aliases. This proposal authorizes
none of that release behavior. Parent F15 remains `code`; child counts are
not effort percentages or a fixed PR estimate.

## ADDED findings from the baseline audit

- **ADDED 2026-11-19 — C-root/engine-edge split:** current EventTargets,
  observer/Streams holders and legacy cycle tests use traced internal edges;
  native queues use C roots. The first family is explicitly C-root-only so
  registry admission cannot silently pin a collectible engine-owned cycle.
  Later traced-holder migration is already tracked in F.15.23/F.15.25.
- **ADDED 2026-11-19 — managed-root shutdown seam:** current destruction
  releases wrapper roots/contexts before its engine leak verdict, while host
  C roots are externally drained. A new wrapper-owned registry therefore needs
  a pre-context drain with live incumbent/metadata through synchronous cleanup;
  otherwise even a correctly registered managed callback becomes an engine
  leak. This is part of the baseline clear/root-teardown obligation, not a
  new reclamation task or permission to mask legacy leaks.
- **ADDED 2026-11-19 — invocation admission composition:** #667 takes role
  tickets and acquires its own loans. New callback snapshot admission must
  compose that ordering once, including partial refusal, rather than nest a
  second loan layer or assume a copied record stays usable through reentry.
  F.15.21 must select and prove the internal seam; this proposal reserves no
  implementation helper.

## Coordination and validation

The F4 lane boundary-read the ownership model: entry/incumbent/report/activity
remain separate, only explicitly native-owned callbacks enter the C-root
family, key invalidation precedes value release, and held metadata survives
finalizer reentry. Its requested capture rollback/no-partial-publication and
root-drain/reentrancy restrictions are explicit above. No F4 interface/layout
reservation or C3 representation was selected.

Source anchors and local links are checked against the pinned baseline.
Only this contract and the F.15.20 row change; the parent remains `code`,
F.15.21 remains `todo`, and other tasks/AGENTS conventions remain with their
own lanes. Documentation/tracker validation supplies no runtime proof.


Validation for this contract: 17 local links and ten pinned source/patch/fixture
groups resolve; retained-source/patch manifest hashes match; comparison of CSV
rows confirms only F.15.20 changes. The tracker schema, diff and stale-reference
checks pass. Re-run the repository checks with:

```sh
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
git diff --check
bash tools/stale_refs.sh
```
