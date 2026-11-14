# F.15 — departed realm reclamation

Design PR: **#600**. Reclamation is not implemented by this brief.

Design and source audit, 2026-11-14. Baseline: merged `userland` `932551a2`,
including F16 #597 and F4 handler settings #596. Prepared engine identity:
`39063a8f7f5747a9737b4f15b67dddb87f6344021bfba9fe3ae79a12ef020fbf`;
embedding ABI 5. Source symbols below are search anchors at this baseline.

This brief changes no engine/native API, layout, collection policy, realm cap
or browser behavior. It proposes proof obligations and reviewable cuts, not
a reclamation certificate. F4c/d's context, WindowProxy, exposure and navigation
contracts must be settled before choosing an implementation's eligibility and
identity rules. Observer/custom-element/Promise settings work extends the
inventory as it lands; an unmerged chapter is not a baseline guarantee.

## Result and acceptance boundary

Reclaim storage that an inactive child no longer needs, and admit subsequent
children without confusing their identities with old handles or retained
objects. Preserve usable ordinary-departure aliases, adopted node identity,
callback settings, native payloads and the irreversible sealing of F16-failed
code. Retained aliases may legitimately prevent reclamation. Bound and account
for those cases instead of freeing their owners or promising unlimited churn.

Releasing a runtime-owned context reference, collecting a context, freeing a
binding, freeing its physical arena and reusing an admission slot are separate
events. Partial byte reclamation does not complete the tracker exit requiring
safe slot reuse. Neither increasing 65 nor removing admission checks meets it.
F11 retains its alternative gate: reviewed F15 evidence or accepted Tier 1
session-consumption/refusal measurements. This brief satisfies neither gate.

The [HTML browsing-context lifetime rules](https://html.spec.whatwg.org/multipage/document-sequences.html#groupings-of-browsing-contexts)
relate accessibility to associated Documents and WindowProxy reachability.
Element removal or loss of full activity alone is not that lifetime proof.
[ECMAScript's weak-reference processing model](https://tc39.es/ecma262/2026/multipage/executable-code-and-execution-contexts.html#sec-objectives)
does not guarantee collection or finalizer timing. Native safety cannot depend
on a JavaScript finalization callback eventually running. Those constraints
guide this proposal; the concrete native release policy remains an embedding
proof, not a rule inferred from either specification.

## Established contracts and remaining gaps

| Fact at the baseline | Consequence for F15 |
|---|---|
| F3 retirement removes scheduling eligibility; retained direct functions from an ordinarily departed realm remain callable. | Retirement cannot revoke those calls to make an otherwise live owner collectible. |
| F16 recovery seals supported failed executable realms and detaches affected activity after a certified native commit. | READY permits affected provider release and later independent admission; it does not free contexts, shells, slots, data or arenas. |
| Root teardown drains the cluster, destroys the engine, then frees native bindings and lifetime groups. | A per-child release must establish its own ordering; it cannot call the root recipe on one member. |
| F4 callback captures hold a value and a conversion-time incumbent; invocation also uses an associated entry realm. | Value reachability and incumbent/entry lifetime are different edges. Current stable realm storage supplies lifetime without per-record realm pins. |
| F4c/d describe stable browsing-context identity, target replacement, origins and proxy behavior; they are not implemented by this baseline. | Neither a proposed exposure flag nor the absence of an exported proxy certifies a reclaimable document. |

The current [F3 ownership proof](IFRAME_F3_PROVIDERS.md),
[F16 native policy](IFRAME_F16_DOM_RECOVERY.md),
[F16 acceptance](IFRAME_F16_HOST_ACCEPTANCE.md),
[F4 security brief](IFRAME_F4_SECURITY.md) and
[F4 settings contract](IFRAME_F4_SETTINGS.md) retain their stated boundaries.
F15 must not broaden the F16 checked-settings/Streams/global-failure subset.

Coordination addendum: [observer settings #599](https://github.com/VBWizard/os64/pull/599)
at `4e78c29e`, outside the pinned baseline, uses `DScheduledCallback` for
value/captured incumbent/associated entry. Engine-owned observer objects trace
the callback value; delivery retains the observer through disconnect/GC.
That invocation/object hold does not add an independent incumbent/entry context
pin: the stable wrapper storage still supplies those lifetimes. Reconcile
this inventory with that source when adopting its baseline.

## Source inventory: edges and owners

The inventory distinguishes engine roots, native ownership and temporary
borrows. It is a starting audit of the named families, not an assertion that
a single count enumerates the heap's incoming edges.

| Owner / source anchors | Retained edge or lifetime requirement | Release proof needed |
|---|---|---|
| `libjs/runtime/runtime.c`: `os64_js_runtime`, `os64_js_realm`, `os64_js_realm_create`, `os64_js_destroy_report` | Inline stable realm slots hold contexts, cached `promise_then`, module lists and host configuration. `realm_count` grows cumulatively. | Drop wrapper roots separately from engine collection; do not compact handles while trusted native clients can retain them. |
| Same: `data_realm`, `Rejection`, `os64_js_module` | Native function data and rejection routing use numeric realm indices; module handles store a realm pointer and engine values. | Old function data, rejected-promise routing and outstanding module handles must not resolve to a replacement child. |
| Patches `0019-job-ownership`, `0020-job-transactions`, `0023-execution-scope-snapshot`; runtime setup/settings guards | Jobs retain execution/callback/activity contexts; setup groups and active scope records also refer to realm-owned state. | Inspect/discard ownership without running jobs to manufacture a safe point; finish scope/group loans before owner release. |
| Patches `0029-recovery-entry-barrier`, `0030-callback-associated-realm`; engine `JS_FreeContext` | Callable, proxy-associated, module and suspended execution state can retain contexts after a stack or job completes. Context reference release does not imply destruction. | Audit engine mark/free pairs and actual collection; retained bound/proxy/async/generator/module aliases remain correctly associated. |
| Patch `0031-object-realm-identity`: `creation_context_id`, `JS_GetObjectRealm` | Non-callable creation identity is a monotonic scalar, not a context root. Lookup returns a live matching context or NULL; it does not infer another from prototypes. | Preserve the no-reuse namespace and prove required callback entry/context ownership. A surviving ordinary object need not itself pin its creation context. |
| `js_callbacks.h`; runtime `os64_js_callback_capture`, `os64_js_callback_mark`, `os64_js_callback_clear` | A captured JS value is rooted/marked; the incumbent is a stable native realm handle. The record does not independently duplicate an incumbent context. | Retain required settings owners through capture and invocation snapshots, including reentrant record clearing. Marking the JS value alone is not that proof. |
| `js_failure.h`; runtime `failure.inc`, `recovery.inc` | Failure observations contain realm pointers borrowed until runtime destruction; recovery has a separate episode generation. | Preserve old observations and tokens across reclamation attempts. F16's generation is not a general slot identity generation. |
| `libdom/core.c`: `d_retain`, `binding_drain`; `internal.h`: `DValue`, cluster buckets | Per-binding registries strongly retain wrappers and helper values. The cluster lookup shares node identity; a native anchor lets retained methods find the binding. | Preserve wrapper identity and tracing while releasing eligible host roots. Wrapper-cache eviction alone does not establish absence of script/native aliases. |
| `libdom/node.c`: `d_creation_owner`, `d_wrap_constructed`; `libhtml/core.c`: `os64_html_node_creation_realm` | Nodes keep a byte creation slot; wrapping a previously unwrapped adopted node resolves `cluster->members[slot]`. Logical document and physical allocation owner are separate. | Surviving nodes must keep their creation identity and prototype owner. Slot reuse can fail even when no old wrapper was ever created. |
| `libhtml/core.c`, `internal.h`, `range.c`; `libpage/state.c` | Lifetime groups retain arenas/states. Adopted allocations stay charged to their original owners. HTML holds, readers, snapshot pins, ranges and page models borrow backing. Group teardown is the current release boundary. | Add reviewed per-owner retirement/release contracts before freeing a grouped arena/state; count adopted-out allocations and borrowed strings/models, not just live-document membership. |
| `libdom/event.c`, `timer.c`, `animation.c`, `idle.c`, `custom.c`, observer families | Registrations, callback captures, arguments, pending records and dispatch snapshots can cross owners. Ordinary departure cancels activity without removing exported registration identity. | Split inactive activity from retained callable/data state; retain creation, associated entry, incumbent and allocation owners as required. Refresh this row for F4's remaining conversions. |
| `libdom/kept.c`, `fetch.c`, `xhr.c`, `blob.c`, `canvas.c` | Kept bodies have independent references and an originating binding ledger; payload/engine finalizers may use native owners. Root cleanup has a native sweep even if engine finalizers were skipped. | Keep ledger descriptors and backing alive through the last permitted use; establish exact per-owner accounting and finalizer order before freeing shells. |
| `libdom/message.c`, `clone.c`, `streams.c` | Messages retain clone data plus source/port values; entangled ports trace partners. Streams retain private bridge/continuation state and foreign-realm data. | Include queued/native/internal edges, not just published wrappers. Streams' exclusion from local F16 recovery is not an exclusion from F15's lifetime audit. |
| `libdom/range.c`, `walker.c`, `adopted.c`, selection and style records | Live ranges/walkers use native nodes and deferred-dead records; Selection and adopted style sheets hold engine/native state. | Sweep deferred records safely; preserve both ends of adopted references and avoid finalizers reaching released HTML/page state. |
| `core.c`: `departure_busy`, `departure_detach`; `webanimation.c`: effect mark/release; `internal.h`: provider borrows | Dispatch, reaction walks, providers and borrowed animation generations can survive a native return. Detaching a provider does not acknowledge an old layout generation. | Exclude active borrowers across members and retain old provider/effect storage until explicit completion; successful departure/recovery alone is not a layout release. |

## Identity hazards to resolve before an implementation

1. **Opaque pointers are still identities.** ABI 5 returns stable
   `os64_js_realm_t *` handles, including borrowed pointers in copied diagnostic
   records. A generation stored inside a reused slot cannot distinguish two
   callers presenting the same old address. Choose an explicit compatible
   lifetime contract or a negotiated new handle/export protocol; do not
   silently change the meaning of existing handles. Permanent tombstones need
   their own bounded accounting and do not themselves solve cumulative caps.
2. **Several namespaces name different things.** Native handle slots, HTML
   creation bytes, engine creation IDs, setup groups, F16 episode generations,
   and F4 browsing-context/target generations are not interchangeable. List
   users of each before changing one. Exhaustion refuses before publication;
   resetting, wrapping or truncating a counter is not reuse safety.
3. **Creation survives adoption.** A live node or native allocation outside
   the old document can depend on its old owner without a live old wrapper.
   Moving a node or editing `members[]` cannot transfer its charges or replace
   its prototype realm. Reachability and accounting must follow those edges.
4. **Security history is a veto, not a reachability oracle.** An ever-exposed
   record may conservatively prevent reclaiming some state. A false
   ever-same-origin flag, detached element or missing proxy proves neither
   absence of direct aliases nor absence of native exports. F4c/d must define
   the relevant observations and publication epochs before F15 selects a
   positive eligibility certificate.

## Proposed release protocol and instrumentation

The intended implementation is opt-in and conservative. Root cleanup remains
the response to global failure or an unproved owner. No proposed entry point,
patch number, layout or ABI revision is reserved by this brief.

At an owner-thread safe point, classify a candidate as ordinarily departed,
privately failed installation, or F16-contained executable failure. These
states preserve different alias behavior. Failed, retired, activity-stopped,
context-unrooted, context-collected, native-released and admission-reusable
must remain distinguishable in diagnostics. A permanently sealed failed
closure still roots data/context that F15 may not overwrite.

First establish a complete owner/exposure snapshot under the agreed F4
contract. Refuse candidates with unknown exports or unclassified holders.
Exclude engine tasks, checkpoints, setup/settings calls, native walks,
provider loans and effect borrowers before destructive work. Pending work is
either a named retaining edge or discarded by its existing audited policy;
it is not executed with a new budget to satisfy reclamation preconditions.

Separate reversible preflight from release of host-held roots. Allocation
failure before mutation leaves identities and charges unchanged. A collector
may synchronously invoke native finalizers when references are dropped, so
keep shells, ledger owners, groups and a reentry guard alive during that
phase. Do not use collection itself as proof that native state was safe to
free. JavaScript cleanup jobs are author work and cannot run under a native
release certificate. Define the retained partial state after a failed attempt;
do not promise rollback after a finalizer has already run.

After root release/collection, recheck surviving engine/native edges and
borrows. Collection can leave a candidate retained; report that result and
keep its identity. Publish a reusable resource slot only after the handle,
creation-owner and F4 target contracts certify that no old identifier can
name the new occupant. Keep counters charged until the corresponding memory
is actually released. Recheck late global health and publication generations;
a candidate cannot use a stale successful preflight to commit later.

The first implementation cut should supply read-only evidence:

- Per-role lifecycle and retaining-reason counts for contexts, wrapper roots,
  callback entry/incumbent handles, modules/rejections/jobs, native owners,
  raw exports and F4 target/history records. An uninstrumented holder reports
  unknown rather than zero. Record which counts are roots, pins or borrows.
- Provider/dispatch/effect loans, HTML holds/readers/pins/ranges, page model
  borrowers and physical-owner ledgers. An arena with adopted-out allocations
  remains charged even if its logical document has departed.
- Before/after engine allocation census, live-context observations and native
  ledgers, including tombstones and scratch peaks. Runtime-wide engine bytes
  are not exact per-realm attribution; shared atoms/shapes must not be counted
  independently for several owners or called leaks solely from a delta.
- Bounded snapshots and deterministic failure/reentry injection at each
  preflight, root-release, finalizer and publication boundary. Inspection runs
  no script or collection and preserves active outcomes and failure records.

Counts expose reasons and support counterexamples. They are not by themselves
a complete incoming-edge or non-observability proof.

## Concrete proof cases

| Case | Required observation / counterexample |
|---|---|
| Repeated no-export children | More admissions than the old cumulative cap, stable bounded metadata and a measured steady retained footprint; distinguish root drop from actual collection. Keeping tombstones forever under a larger cap fails this case. |
| Retained departed function, bound/proxy function and suspended async/generator | Ordinary alias behavior and associated settings remain valid; reclaim stays deferred while needed. Failed executable aliases remain sealed after F16 containment. |
| Adopted-out node, initially unwrapped node, Attr/collection/Range/Selection and constructed sheet | Identity/prototype/current-document services and original charges survive source departure; old creation-slot metadata cannot resolve to a new child. Test both departure orders. |
| Foreign listener/timer/handler/observer/custom definition | Registered value, entry, incumbent and allocation owners survive independently. Removal/reassignment during dispatch cannot release an invocation's pins. Add Promise/thenable cases when F4 supplies their contract. |
| Old realm handle, function-data index, failure record, module handle and recovery token | After an attempted reuse each still denotes its original identity or is explicitly refused under its negotiated contract; none reaches the replacement. Test raw native exports as well as JS paths. |
| Queued message/clone/port, engine job/rejection/module and Streams bridge | Retain or discard through the established owner policy without draining siblings to force admission; no dangling native owner remains after finalization. |
| Blob/ArrayBuffer/Response/XHR/Canvas retained in a sibling | Payload remains usable and charged to a live ledger until its release; test ordinary collection and both root teardown policies after the attempted reclamation. |
| Provider callback or layout effect borrowed at release | Candidate stays retained; recursive release/publication is refused; old effect generations remain readable until explicit host acknowledgment. |
| Cross-origin/opaque, once-same-origin, proxy retarget/detach and reentrant getter | Use F4c/d's actual identity/exposure rules and retained Location/method answers. A no-exposure heuristic or serialized origin comparison must not authorize reclamation. |
| OOM, cancellation, native failure and identity exhaustion at each phase | No partially reused slot or charge rollback over live storage; preserve global health and original diagnostics. Root cleanup is still leak-free after any retained/partial result. |
| Mixed pinned and reclaimable owners, then final root teardown | Unpinned owners make measured progress while pinned ones preserve semantics; all groups/ledgers remain correct across reverse finalizer order and final cleanup. |

Each implementing cut needs positive and named negative/mutation evidence
under fatal and reporting/reclaim root teardown. Keep ASan/UBSan and
LeakSanitizer enabled; use the supported outside-sandbox leak run where
necessary. Include engine bookkeeping, actual target heap/allocator identity,
native per-owner balances and exact guest parent exit. Root reclamation of a
leak is a failed proof, not successful child cleanup. Preserve default
constructors and qualified named WPT baseline comparisons.

F15 acceptance must measure churn with retained aliases, refusal behavior,
temporary peaks and bounded metadata in a coherent native host. F11 separately
supplies the actual browser/P5 path; host-only counters cannot certify
visible-frame acceptance. Unreachable objects need not be collected immediately, but the
selected bounded-progress implementation must meet its recorded workload and
resource contract without depending on nondeterministic author finalizers.

## Proposed small PR cuts and shared decisions

| Cut | Reviewable result | Gate before the next behavior change |
|---|---|---|
| A — this brief | Source inventory, proof cases and owned tracker design state | Design review; no reclaim behavior or completion flip. |
| B — diagnostics | Bounded scalar owner/borrow/root snapshots and injection points | Current lifecycle, F16 and F4 behavior unchanged; unknown holders refuse certification. |
| C — identity/lifetime contract | Explicit native exports/pins and compatibility story for handles, creation slots and diagnostics | Agreement with F4c/d and public header/ABI consumers, including CLI; no silent pointer reuse. |
| D — native owner families | Reviewed per-owner release for registry/native/group families with trace/finalizer pairing | Per-family retained/adopted alias and failure-injection proofs; unpublished capability remains inert. Split families further where necessary. |
| E — bounded reclamation/admission | Narrow opt-in release and reuse policy using the agreed certificates | Engine/native graph proof, stale-identity mutations, safe partial outcomes and bounded accounting. |
| F — host acceptance | Coherent guest/web/churn receipts and actual integration limits | Reviewed engine/native reachability, teardown and safe slot-reuse proof before F15 completion. F11 separately owns browser/P5 acceptance and its measurement alternative. |

These are dependency cuts, not a fixed PR-count estimate. Before C or any
engine/native change, coordinate: which F4 target/exposure facts are available;
which layer pins incumbent/entry/associated objects; how trusted raw exports
end; how stable native handles differ from recyclable resources; which HTML/
page owner groups can release independently; and how publication excludes
callbacks/finalizers without violating their existing cleanup contracts.

Do not implement those interfaces in active F4 files through this brief.
Unknown ownership, incompatible stable handles or an incomplete F4 contract
leave reclamation disabled and retained charges visible. F15 stays in design
until an implementing cut has its reviewed contract.

## Validation of this document

Check source anchors and local links against the baseline, review the
engine/native mark/free and root-destruction order, and run:

```sh
python3 -I tools/tracker.py --csv docs/design/pending/IFRAME_TRACKER.csv --prefix F --quiet
git diff --check
bash tools/stale_refs.sh
```

No runtime tests, new memory measurements or collection/reuse proof are
claimed by this documentation-only PR. Existing F3/F16/F4 evidence belongs to
its original supported scope.
