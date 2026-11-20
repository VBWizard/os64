# F15.30 — bounded checked module-handle family contract

Contract PR **#685**.

## Baseline task list — 2026-11-19

- [x] Reconcile remaining persistent export families into visible F15 child tasks.
- [x] Audit module compilation, request visits, evaluation/watch state and release.
- [x] Select a bounded numeric module family with explicit owner, root and loan obligations.
- [x] Specify rollback, asynchronous evaluation, retirement and guarded root teardown.
- [x] Record implementing counterexamples and preserve independent raw/traced/native gates.
- [x] Validate source references, tracker and contract; publish #685 for Codex review.

This is a **contract and source audit**, following the selected callback family
#683 and F15.22's scalar C3 adapter. F.15.31 implements and proves this family
later. F.15.23 remains open: neither this proposal nor one implementing family
completes the remaining-export migration, public checked profile or reclamation.

## Remaining export audit

The audit uses `runtime.c`, private owner contracts and the current DOM/Streams
consumers. A checked registration or a diagnostic snapshot covers its named
operation; it does not migrate the callbacks or durable results of that service.

| Remaining family | Concrete follow-up | Preserved boundary |
|---|---|---|
| Persistent module handles and evaluation state | F.15.30 contract; F.15.31 implementation | A handle root, engine module graph, watch reaction and original owner are distinct edges. |
| Import-base provider delivery | F.15.32 contract; F.15.33 implementation | #662 checked registration still delivers through a raw settings getter and raw context callback. Provider opaque detach remains explicit. |
| Promise incumbent capture | F.15.34 bridge/tracing contract | `job_incumbent_capture` uses the public settings getter; its retained engine context/activity edge is not a callback C root. |
| Script-host exception/rejection callbacks and rejection routing | F.15.35 family contract | Reporting, original relevant owner, selected destination and returned raw realm/context/value borrows need their own scoped envelope. |
| DOM native callback consumers and engine-held callback edges | F.15.36 classification/migration plan | Timers/listeners/scheduled C roots differ from Streams and other engine-traced cycles. #683 does not migrate those consumers. |
| Kept bytecode/blob results | F.15.37 export contract | Caller-owned kept bytes and their physical allocation lifetime are separate from a module's JS roots. |
| ABI 5 raw realm/context/settings/failure APIs and legacy scoped/setup callbacks | F.15.24 negotiation and embedder migration | Compatibility getters retain their sticky unknown-retention veto. No general exemption is introduced. |
| DOM creation/adoption/kept payloads, native providers/registries/effect generations | F.15.25, split there by family | A numeric owner or C3 scalar snapshot does not acknowledge native borrowers, detach providers or replace physical ledgers. |

F.15.30–F.15.37 are **ADDED 2026-11-19**, after source inspection made these
remaining export boundaries concrete. Completed contract work and pending
implementation stay separate on the board. Existing F15 IDs are preserved.

## Existing module lifetimes

The audit baseline is `runtime.c` at `c0b28e9a` (F15.22 source); no module code
changes relative to the callback milestone #683. Relevant source anchors:

| Anchor | Current behavior |
|---|---|
| `struct os64_js_module` | A linked native handle stores its original realm, module JSValue, evaluation promise, run/rejected state, owed-report order and copied failure. |
| `module_compile` | Compiles without resolving imports; allocates/copies temporary source, constructs module metadata, links the completed handle, then records legacy raw retention and publishes a pointer. Source and bytecode entry points share it. |
| `module_owned` | Validates a live pointer by membership in that original realm's retained list; it is not an incarnation-safe reusable numeric identity. |
| `os64_js_realm_module_requests` | Converts each request atom to a CString, lends it to the native visitor, then frees it. The borrow ends after that visitor returns. |
| `module_body`, `os64_js_realm_module_run` | Resolve/link and evaluate once in an open task; the handle then retains its evaluation promise. Linking and author execution can reenter provider/native services. |
| `module_watch`, `module_rejected` | Pending evaluation is watched using the engine's saved Promise `then`. Function data retain the promise and original realm index, not the native handle pointer. A later callback resolves the retained list by promise; release makes that handle-specific lookup absent. |
| `module_failed`, `module_owed_fold` | Immediate and late failures retain their existing diagnostic/reporting semantics. Owed order is distinct from handle identity; retired realms are excluded from late outcome folding. |
| `os64_js_realm_module_state` | Reports run/evaluation state and copies failure without exposing the promise. The current API still accepts a pointer handle. |
| `module_free`, `os64_js_realm_module_release` | Idle release unlinks the handle before freeing module/promise values and native storage. Active release refuses. Dropping a host handle does not unload the engine's imported module graph. |
| `os64_js_destroy_report` | Frees retained handles while their realms/engine still exist, before context destruction. Grouped retention remains the storage boundary. |

The watcher prevents page-controlled Promise constructor/species lookup on its
internal evaluation promise. A checked family must preserve that protection;
it cannot replace the saved engine operation with an ordinary author-visible
`promise.then` lookup. Existing engine-held reactions remain traced edges.

## Proposed selected family

Select one private runtime-owned module family explicitly before publication.
Reserve a positive finite record pool in charged owner metadata, with checked
size arithmetic and actual allocator admission before engine construction.
The first implementing PR chooses concrete C names/layout/statuses; it must
implement this contract as one compile/visit/run/state/close family.

A typed module key contains runtime incarnation and a fresh nonzero serial in
the module namespace. Copies alias one record; they add no roots or holders.
Never issue from a native address, realm slot, promise pointer or C3 generation.
Full capacity or issuer exhaustion refuses, without eviction/rebinding/wrap.
Failed admission may burn a serial; no issuer is rewound. The key does not keep
its runtime alive, and APIs require the live runtime on its owner thread.

Each published record owns:

- An independent holder for its **original module realm**. It cannot be replaced
  by the primary realm, active F4 target, current provider or reporting realm.
- One module-value C root and, after evaluation starts, its owned evaluation
  promise reference when present. These are separate from engine graph edges.
- Run/rejected/watch/report state and a copied diagnostic, with explicit live,
  clearing and free states. Failure diagnostics contain no borrowed engine data.

The host retains no raw `os64_js_module_t *`, `JSValue`, context or realm result
through this interface. A checked record cannot simply wrap a publicly exported
legacy handle and quietly waive the raw-retention veto. Refactor/share private
implementation primitives as needed; keep legacy public pointer semantics and
legacy leak detection intact.

## Admission and compile rollback

Compilation takes an eligible original-owner ticket and temporarily lends that
owner through source copying, parser/bytecode entry, metadata construction,
diagnostics and final temporary-value release. Reserve a record and persistent
owner holder before fallible engine work. Publish a key only when the complete
result and final health permit it. Capacity, ticket, allocation, syntax or engine
failure leaves the caller's key unchanged; unwind temporary roots/holders and
return the record to its bounded pool. The active outcome cannot be overwritten
by reentrant alias use.

Compilation does not resolve imports or run a module. Preserve its existing
idle/task admission semantics; do not open a fresh author turn merely to reuse
a scoped executor, renew a caller budget or invent settings/incumbent roles.
A module's original owner is not its task/activity/report owner.

Borrowed immutable bytecode input may be used only during the guarded compile
operation. The first checked module family **does not produce kept bytecode
buffers**: requesting a kept output refuses before publication. F.15.37 owns
that separate export contract. A blob cache miss/fallback still follows normal
source compile admission; input bytes are not retained by the key.

Known parser allocation failures in [DEBTS.md](../../../DEBTS.md), patch 0011b,
remain an independent engine robustness limit. This proposal supplies no new
universal parser-OOM guarantee. Implementing receipts must name their exercised
native/engine cuts, excluded debt paths and invariant behavior accurately.

## Request visitor, evaluation and observation

Request enumeration admits one original-owner loan and holds the record through
all visitor calls and CString/atom cleanup. Lend each immutable specifier only
for its callback; copying it is the host's separate allocation. A private scope
may be supplied for owner observations, but no context/value is implicitly
exported and no settings role is fabricated. Close of a live record during an
active operation refuses busy; native visitor storage survives that call.
Explicit scope export remains a compatibility operation with its normal veto.

Run requires an eligible original module owner and the existing open task. Admit
its original-owner loan through link/resolve, loader reentry, author evaluation,
watch installation, diagnostics and value cleanup; the enclosing task separately
protects its activity owner. Preserve existing entry/incumbent/current/report
selection and global receiver behavior. Do not capture an arbitrary compiler
incumbent as module execution authority. Owner identity does not authorize an
F4 target, an origin access or a navigation.

The record transitions to run before link/evaluation begins, so retry cannot
rerun partially executed module code. Preserve immediate link/evaluation errors,
fulfilled/pending/rejected states and deferred failure order. No new implicit
checkpoint or reporting hook is selected. Jobs and pending evaluation continue
under the runtime's existing task/checkpoint budget.

A watcher may keep the evaluation promise and original realm through engine
function data. It must resolve a still-live checked registration using a stale-
safe identity or equivalent independently proven engine-edge representation;
it must not retain a reusable native cell address. After close, a late watcher
cannot update a replacement record. Losing a host record does not cancel engine
evaluation, unload its module graph or remove unrelated rejection/report hooks.
Preserve the existing handle-specific absent-lookup behavior deliberately.

State inspection returns only copied lifecycle/evaluation/diagnostic facts.
It performs no author property access, Promise species lookup, allocation or
clock poll. Missing/foreign/stale keys refuse unchanged output; retired/failed
original owners remain distinguishable from eligible execution. A pending
promise or zero live handles does not supply graph reachability or release
eligibility.

## Close, failure and root shutdown

Close is an idle cleanup operation, including retired/failed owners. An active
module/visitor/task engine invocation refuses busy with the registration intact,
matching the conservative first-family admission. No borrowed operation can
lose its persistent record underneath it.

At admitted close, invalidate the key and detach it from watcher lookup before
releasing values. Hold original-owner metadata and native cell storage through
module/promise finalizers, with the persistent holder released last. Guard
cleanup against ordinary wrapper author entry; no new holder/loan, bookkeeping
allocation, clock or healthy-runtime requirement is needed to drop owned roots.
A stale close cannot affect a replacement. Another live-record close during
cleanup may refuse busy; do not introduce unbounded recursive managed cleanup.

Root shutdown closes admission and invalidates the selected family's keys before
its finite managed-root drain, while the contexts/engine and metadata are live.
Release selected module/promise roots and holder state before wrapper context
release. Keep the scalar/cell bookkeeping needed by allowed finalizer diagnostic
queries until that cleanup ends. Engine/native storage teardown then follows its
existing grouped order. No raw engine root may be silently ignored or laundered
into an owned root just to make the leak report clean.

Neither close nor root drop asserts collection. Imported graphs, compiled code,
suspended evaluation, Promise reactions, raw ABI exports, DOM creation owners,
provider borrows and F4 cached/exposed objects retain their independent proofs
and release vetoes. C3 remains diagnostic input; physical owner release and slot
reuse remain F.15.26–F.15.29 work.

## Required implementing counterexamples — F15.31

| Transition / refusal | Required independent evidence |
|---|---|
| Same source compiled in two original owners | Distinct owner facts; the primary/current active target cannot substitute. |
| Full record or holder pool; exhausted record issuer | Unchanged key, no published root/holder; old registrations still usable; no wrap or eviction. |
| Native allocation/syntax/bytecode fallback failure | Transactional output, bounded charge and finalizer-safe root/holder unwind, with parser debt qualifications. |
| Request visitor reentry/close and refusal | Correct original-owner loan through CString release; busy close preserves the live record. |
| Link/loader/author reentry and active close | Guard, original-owner loan and task budget survive; no double run or implicit checkpoint. |
| Pending evaluation closed, cell reused, then old promise rejects | Replacement is untouched; engine watch/graph lifetimes remain traced and reporting semantics stay explicit. |
| Immediate rejection, late rejection and retired owner | Correct copied diagnostics/report ordering; retirement blocks execution without inventing collection. |
| Stale/foreign keys and same-address runtime reconstruction | Old copied identity cannot resolve a new record/runtime. |
| Idle close and root shutdown with native finalizers | Key invalid first; cell/owner survive values; ordinary reentry refuses; selected roots drain before contexts. |
| Unrelated raw root retained alongside selected modules | Existing legacy leak reporting still detects it; no global suppression. |

Use both teardown policies and shipping/instrumented engine profiles, leak-
enabled ASan/UBSan outside the sandbox, named sensitivity controls and matching
production object/guest evidence appropriate to the implementation. This
contract adds no new runtime, sanitizer, guest, WPT or P5 receipt.

## ADDED contract findings

- **ADDED 2026-11-19 — late watchers are not raw handle pins:** the current
  watcher stores promise plus realm index and resolves the retained list. A
  checked implementation needs stale-safe lookup after cell reuse, while the
  engine-traced promise/reaction may outlive the native host registration.
- **ADDED 2026-11-19 — kept bytecode is another export:** sharing the legacy
  bytecode compile path would otherwise quietly add a durable native buffer
  result to a JS-root family. Track it explicitly as F.15.37 and refuse kept
  outputs in the first checked module implementation.
- **ADDED 2026-11-19 — provider registration is not checked delivery:** #662's
  provider setters and a new module key do not migrate loader/import-base or
  Promise incumbent callback escapes. Preserve those independent F.15.32–35
  tasks and the raw-retention veto.

## Contract validation

The source audit verifies **16 named runtime anchors** and an unchanged runtime
source against `c0b28e9a`. Local file links, the tracker and the exact owned-row
comparison pass; existing IDs and unrelated slices remain unchanged. The eight
ADDED rows make completed contract work and pending implementation visible;
F.15.23 stays `doing`, and F.15.31–F.15.37 stay `todo`. A separate F4 boundary
read found no blocking contract mismatch. PR #685 carries the contract for Codex
review. No implementation or new runtime acceptance is attributed to this PR.
