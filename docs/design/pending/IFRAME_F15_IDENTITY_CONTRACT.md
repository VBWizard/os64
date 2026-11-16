# F15 C — identity and export compatibility requirements

Design PR: **#621**, proposal source `a1d9e88f`.

Design baseline: merged `userland` `20e6f772`, including the F15 diagnostic
slices #610/#614/#617 and F4 finalization settings #618. Engine identity is
`1a60b7a86e45e2e82bc5f359c3295ad68333a2f18f1ea79f3f710fd9cea9b598`,
embedding ABI 5, bytecode format 6.

This is a compatibility proposal for review with F4c/d. It recommends the
identities and proofs an eventual release contract must distinguish; it does
not select a public API, ABI revision, handle encoding, pin type, engine patch
or reclamation permission. Existing pointer lifetimes and cumulative caps
remain in force. F15 stays in progress.

## Recommendation

Keep stable browsing-context identity, document identity, settings/global
identity and recyclable physical storage separate. Navigation replaces a
context's active target; it does not turn old document owners, callbacks or native handles into
references to the replacement. A document owner can have several independently
retained parts: settings/intrinsics, engine roots, native shells, physical
arenas, quota ledgers and provider/effect loans. Releasing one part does not
establish that another can be recycled.

Preserve the ABI 5 contract for existing callers. A new opt-in native export
contract is the recommended route to owners eligible for bounded reuse. It
must specify how an export begins, what it keeps alive and how its lifetime
ends. Untracked legacy exports retain their existing promise and prevent
release of the resources needed to honor it. These are requirements on a
future contract, not a lease facility added by this document.

A generation inside a recycled descriptor does not repair a borrowed pointer:
an old caller still presents the same address. Checked identities must carry
enough independently retained information to reject an obsolete occupant.
Under that new checked contract, an ended identifier must refuse lookup,
rather than resolve to a new owner. Existing borrowed pointers cannot acquire
that shorter lifetime by implication.

## Current identity and export inventory

Source paths below are relative to the repository root. Facts in this table
describe the baseline; the last column records the compatibility obligation.

| Identity or export | Representation and current lifetime | Required distinction before reuse |
|---|---|---|
| Wrapper realm/context exports | `libjs/include/os64/js_engine.h`, `libjs/CONTRACT.md` and `runtime/runtime.c` under `userland/`: `os64_js_primary_realm`, realm creation/context lookup and `os64_js_context` expose borrowed handles/contexts until runtime destruction, including retirement. Descriptors occupy the fixed `realms[]` table. | Keep an issued ABI 5 handle's original meaning. A retired or unpublished diagnostic owner is not an available descriptor merely because it cannot execute. |
| Function data, modules and rejection routing | `userland/libjs/runtime/runtime.c`: `data_realm` resolves numeric function-data slots; module handles retain a realm pointer/value; rejection entries route through realm indices. A module handle has its explicit release/destroy contract. | Retained native functions, outstanding module handles and pending rejection provenance cannot select a replacement at an old index. Explicitly released module handles do not gain a new validity promise. |
| Failure observations and recovery tokens | `userland/libjs/include/os64/js_failure.h`: copied failure records contain realm pointers borrowed until runtime destruction, including a possible unpublished task owner. `runtime/recovery.inc` checks runtime plus episode generation. | Preserve old observations without rewriting them. An F16 episode authenticates that recovery attempt; it is not an owner/slot identity or a release certificate. |
| Callback value, entry, incumbent and report roles | `userland/libjs/include/os64/js_callbacks.h` and `userland/libdom/internal.h`: native holders root/trace values; captured wrapper handles use stable storage. Invocation views can borrow roots while the owning record is cleared reentrantly. | Name each required owner through conversion, invocation and unwind. A value root and a borrowed settings handle are different edges; do not count them as independently implemented context pins. |
| Engine creation identity | `userland/libjs/patches/0031-object-realm-identity.patch`: runtime-scoped monotonic context IDs are stamped on objects. A non-callable object's stamp is not a context root; `JS_GetObjectRealm` can return NULL if its context is absent. | Preserve non-rebinding identity without inventing a context pin for ordinary data. A matching address, prototype or new occupant is not a substitute for the recorded creator. |
| Engine execution and queued roots | Patches `0019-job-ownership`, `0020-job-transactions`, `0034-active-script-records` and `0035-finalization-settings`: execution/callback/activity contexts, setup groups, closures, reactions, jobs and registries have distinct retained/borrowed roles. | Trace/free and discard policies must cover their actual edges. A setup-group address belongs to its descriptor. ScriptOrModule records and registry captures remain unknown to the F15 scalar snapshots. |
| HTML creation owner and physical allocation | `userland/libhtml/internal.h`, `libhtml/core.c` and `libdom/node.c`: nodes keep a creation byte; `d_creation_owner` resolves it through `cluster->members[]`. Adoption changes the node document while the allocation owner and creation byte remain. | A surviving unwrapped node can still require the original prototype/settings owner. Replacing `members[byte]`, or seeing no old wrapper, cannot establish safety. Retain the required creation services or review a compatible replacement contract. |
| Native identity and storage epochs | HTML arena IDs/node serials, snapshot pin handles/group epochs, page model references and DOM effect generations describe their respective native resources. | Keep resource identity, snapshot lifetime, document revision and publication generation distinct. B2's complete local list does not classify other group borrowers. |
| Payload ledger ownership | `userland/libdom/kept.c`: physical charges and native backing references remain on the allocating binding. Foreign Blob aliases/slices can retain them; a constructed Response uses engine storage outside that census. | Keep ledger descriptors and backing valid until their permitted releases. A zero quota, one native reference or complete prefix is not proof that script/native aliases ended. |
| Proposed F4 origin identity | `IFRAME_F4_SECURITY.md` uses copied tuple/opaque origin values; opaque identities are nonzero, monotonic and distinct from URLs. | Origin identity is not a context/owner ID or publication generation. URL mutation and serialized `null` cannot reconstruct an opaque identity or restore old authority. |
| Proposed F4 context and target | `IFRAME_F4_SECURITY.md`: a cluster-owned stable context is distinct from a binding/Page; it holds active target, origin/relation state and publication generation. F4c retains context records and failed reservations until root teardown under its cumulative cap. | F15 must agree with that contract before changing retention. Target replacement, context discard and resource reuse are separate transitions; F4c itself does not promise reusable context capacity. |

The engine build identity, bytecode version and process class IDs describe
other compatibility namespaces. Equality in one of those namespaces does not
prove that a realm, context target or allocation has the same identity.

## Bounded stable identity

The recommended contract gives a logical owner an identity that is not
reassigned within its runtime's namespace. It can outlive a particular
physical allocation when retained handles or security state require it. A
physical slot can host a later owner only after the reviewed contract ends
the earlier occupant's required lifetimes. Identifier width and storage
layout remain open; exhaustion refuses before publication and leaves the
old target and charges intact.

This does not require a permanent table entry for every historical admission.
Charge retained descriptors, export records, caches, history/veto state and
temporary reservations to explicit bounded metadata budgets. A closed export
cannot discard security state or an identity another valid holder still
needs. An owner with no remaining required holders should not leave an
unbounded tombstone chain; the implementing proof must show how it ends that
metadata lifetime. If the bound is occupied by valid retained owners, refusal
is the correct result, rather than eviction or a larger hidden cap.

Legacy callers can keep an owner retained for the runtime's lifetime. The
bounded-progress workload must therefore distinguish retained/exported owners
from no-export owners under the future opt-in contract. Its eventual proof
requires more admissions than the old cumulative cap with bounded descriptors,
native/engine charges and temporary peaks. Raising the cap, keeping one
tombstone per admission or moving charges to an unmeasured ledger does not
meet that workload. This requirement belongs to F15's later implementation;
it does not change F4c's reviewed interim cumulative-cap boundary.

## F4 publication and exposure obligations

F4c/d need to identify the stable context, old document and required settings
owners, new target, target generation and operation source independently.
Document and settings lifetimes must stay explicit even where a global can
be reused across document changes.
Prepare resources and validate host policy before departing the old target.
At the native safe point, publish the target/origin/generation relationship
consistently without author execution or allocation in the commit. Refusal
preserves the old target. Recheck target/generation, health and relevant loans
after reentry; a failed or stale preflight cannot commit. Provider/effect
loans and existing departure barriers still govern service detachment.

Exposure/history information is a conservative veto and a compatibility
obligation. An unset flag, detached element, empty wrapper registry or missing
proxy is not a positive no-export proof. F4c/d must define which observations
are recorded, their lifetime and how publication invalidates stale requests
and authority caches. Do not discard those facts to make metadata appear
bounded. Exact native interfaces remain F4c/d implementation decisions.

The HTML [WindowProxy algorithms](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-windowproxy-exotic-object)
preserve the proxy while changing its wrapped Window. A retained
[Location](https://html.spec.whatwg.org/multipage/nav-history-apis.html#the-location-interface)
keeps its relevant-global association, but its relevant document comes from
that global's browsing context's active document, or is absent. Thus an old
Location is neither a frozen old URL nor a context-wide object rebound to the
new Window. Method-specific entry/incumbent checks, source selection and
navigation policy still apply. The [security algorithms](https://html.spec.whatwg.org/multipage/nav-history-apis.html#security-windowproxy-location)
also distinguish relevant globals and descriptor-cache keys. F4d supplies
the operation-by-operation answers; these semantics do not by themselves
certify that an old document/settings owner can be released.

## Required counterexamples and future implementation proofs

| Case | Observation required before the corresponding feature can ship |
|---|---|
| Old realm pointer/context, module handle, function-data index and copied failure record | Preserve each still-valid original identity. Under an explicitly different checked-export contract, an ended handle refuses instead of reaching the replacement. The negative control reuses an address/index while an old holder is valid. |
| Initially unwrapped A node adopted into B, then A departs | First wrapping still uses A's required creation/prototype services and original physical charges. Test reverse departure order, a wrapper retained only in C, and an adopted subtree with mixed creation owners. The negative control replaces `members[creation_byte]` after checking only wrapper counts. |
| Retained old Location across same-origin then cross-origin replacement | Proxy identity remains stable; retained Location/global/cache identities remain distinct; document-dependent operations and security/navigation checks use their specified roles. Include discarded context and stale navigation generation. Negative controls freeze the old URL or substitute the new relevant global. |
| Foreign listener/timer/observer/custom callback cleared during invocation | Value, associated entry, captured incumbent, report owner and invocation view survive their required scopes independently. Ordinary retained calls keep their established behavior; F16-failed executable aliases remain sealed. The negative control releases a settings owner based only on clearing the registry value. |
| Promise/thenable, suspended execution, ScriptOrModule and FinalizationRegistry | Account for the actual traced/captured owners and queued report roles through GC, failed/retired contexts and job discard. Do not run pending author work or a registry cleanup callback to manufacture a release point. Missing mark/free or ownership classification retains/refuses the candidate. |
| Provider reentry, borrowed effect generation, new cancellation or OOM | Exclude active loans before release/publication; late failure and reentry preserve the original diagnostics, target generation and charges. Keep shells/ledgers valid through synchronous native finalizers and report retained partial states honestly. |
| No-export churn mixed with permanently retained owners | Make bounded progress for proven releasable owners while retained aliases stay usable and charged. Measure context/descriptor/history/export metadata, shared engine storage, native owners and peaks; root cleanup balances under both teardown policies. |

Future code needs named negative controls, matching target/instrumented
engines, ASan/UBSan/LSan, actual target allocator balance and exact guest exit.
The [B1](IFRAME_F15_RETENTION_DIAGNOSTICS.md),
[B2](IFRAME_F15_NATIVE_BORROWS.md) and
[B3](IFRAME_F15_KEPT_DIAGNOSTICS.md) receipts remain diagnostics with explicit
unknown coverage. F11 separately owns browser/P5 acceptance or its recorded
measurement alternative. This design PR claims no new runtime proof.

## Decisions for joint review

| Decision | Recommendation | Agreement needed before implementation |
|---|---|---|
| C1 — Native export compatibility | Preserve ABI 5 defaults; use an explicit opt-in contract for tracked exports of future reclaimable owners. | Chris and F4/F15 choose the migration/negotiation model and the end of each export's lifetime. No API name, ABI number or encoding is reserved here. |
| C2 — Bounded retained identity | Bound retained owner/export/cache/history metadata and refuse when valid holders occupy that budget; avoid a permanent descriptor per no-export admission. | Agree which metadata must survive each holder and how its lifetime ends without revoking legacy promises. Keep F4c's interim cap distinct from F15's later churn target. |
| C3 — Target and exposure publication | Stable contexts publish a named target/generation/origin relationship; exposure history vetoes release without serving as a reachability certificate. | F4c/d define the actual snapshots, cache keys, unavailable/discarded answers and stale-request checks. F15 cannot infer them from binding addresses or URLs. |
| C4 — Ownership of required settings/creation services | Give engine traces, native exports, invocation loans and allocation owners separate release obligations. | Audit who keeps each entry/incumbent/report/creator alive, including late native wrapping and finalizers, before selecting pins or releasing a root. Existing callback/registry layouts remain owned by their implementing slices. |

The concrete [export-lifetime proposal](IFRAME_F15_EXPORT_LIFETIMES.md)
recommends migration semantics for C1/C2/C4, including tickets, scoped loans,
compatibility retention and bounded refusal. It leaves C3 as required F4 input;
its interfaces and implementation are still subject to review.

Review of this brief records the proposed direction and unresolved decisions.
It does not complete C's implementation gate, authorize D/E, or settle the
F4c/d contract by assumption. Those stages need the agreed concrete interfaces
and their implementing proofs under the [F15 release plan](IFRAME_F15_RECLAMATION.md).


## Validation and coordination

The F4 lane read this proposal and found no shared-boundary mismatch. That
read confirms the stated design boundary, not agreement to C1–C4's eventual
interfaces or implementation. The independent import/base-URL work does not
supply those decisions.

Source-anchor and patch-family checks, manifest hashes, local links, the
iframe tracker validator and the stale-reference check pass at the baseline.
Only this brief and F15's tracker notes change; status, dependencies and
completion fields are unchanged. No runtime code, new runtime measurements,
reclamation, guest acceptance or P5 evidence is claimed by this design PR.
