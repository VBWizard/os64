# F.3 — realm retirement and task budget boundaries

Design brief, 2026-11-11. Inspection baseline: `37ec284d` (`userland`,
including F.2a #537 and F.2b #539). This is tracker slice **F.3**, within
IFRAME.md's F1 foundation stage. These are proposed implementation contracts,
not an as-built record or a claim that the proofs below have passed.

## Result and implementation cuts

A trusted host can retire one document in a three-realm cluster, stop its
scheduled work and detach its host services while its siblings continue.
Previously exported objects retain their identity, prototypes and native
owners. Failed installation of an unpublished child can leave healthy siblings
usable when the failure is local; shared-engine failures retain their existing
runtime-wide outcome. Cross-realm execution uses the enclosing task's budget.

Use two implementation PRs under the F.3 row:

- **F.3a — libjs lifecycle and job ownership.** Add irreversible retirement,
  job attribution/filtering and budget proofs, with a trusted engine harness.
  Include the interrupt audit described below. Existing standalone callers
  retain their runtime-wide failure contract.
- **F.3b — libdom departure and failed-child isolation.** Add a departure
  operation distinct from root drain, service detachment, bounded retained
  ownership and staged installation, with the three-realm host/guest proofs.
  Stack on F.3a if needed. Complete F.3 after both cuts' acceptance.

Neither cut enables frame loading, WindowProxy, origin access, navigation,
BFCache, unloading events, or a new browser scheduler. F.4 supplies the security
boundary; F.6–F.11 supply the Page tree and integrated host. Yonder's existing
single-document retirement remains a root teardown. The foundation API must
be usable without pretending that a Page or browsing context already exists.

## Source facts that constrain the design

Paths in this table are relative to `userland/` at the inspection baseline.

| Source | Present behavior | Required separation |
|---|---|---|
| `libjs/runtime/runtime.c`: `os64_js_realm`, `realm_count` | Contexts and slots remain until runtime destruction; capacity is 65 | Execution eligibility versus context/native lifetime |
| `libjs/runtime/runtime.c`: `observe`, `latch`, `interrupt` | Cancellation, engine allocation failure and execution expiry latch on the runtime | Ordinary document departure must not call runtime cancellation |
| `libjs/runtime/runtime.c`: `os64_js_drain_jobs`, `checkpoint` | Each calls `JS_ExecutePendingJob`, then receives its context | Decide eligibility before invoking the job and before charging an executed job |
| `libjs/upstream/quickjs.c`: `JSJobEntry`, `JS_EnqueueJob2` | Jobs hold a context and argument values | Preserve these holds while adding explicit execution ownership |
| Same file: `fulfill_or_reject_promise`, `perform_promise_then`, `js_promise_resolve_function_call` | Promise job enqueue sites pass their current context | A handler/then function's realm can differ from that context |
| `libdom/core.c`: `d_context`, `binding_drain` | `closed` blocks object access; drain releases retained values and task state together | Departed objects remain valid; scheduled activity stops separately |
| `libdom/core.c`: `create_binding` | Ownership transfers before installation; later failure poisons/drains the cluster | Unpublished child failure must not drain healthy siblings for a local refusal |
| `libdom/core.c`, `internal.h`: cluster members and wrapper buckets | Bindings and native creation slots remain stable; wrappers are strongly retained | Keep slot/native-owner identity across departure and failed installation |
| `libdom/kept.c`, `canvas.c`, `webanimation.c` | Payload finalizers and borrowed effects use original owners | Departing a host does not free those owners or borrowed data |
| `apps/yonder/scripts.c`: `retire` | Drains the binding and destroys the engine | Keep this as the root path; it is not a child departure helper |

The engine file above is pinned upstream input. Engine edits belong in the
hash-checked patch series, manifest and generated engine, not in that input.
Patch 0018 exposes `JS_GetFunctionRealm`; inspect the generated implementation
and its bound-function/proxy cases before using it for job ownership.

## Lifecycle and entry contracts

Use explicit states rather than widening `closed`:

| State | Scheduled script/service delivery | Object/native backing |
|---|---|---|
| Installing | Trusted setup under its guarded task; no public host delivery | Private child, already charged where allocated |
| Active | Eligible under normal task/callback rules | Stable cluster member |
| Departing | No new task or asynchronous delivery; an existing stack may unwind | Provider storage and in-flight records still held |
| Departed | No document-owned scheduled activity; no return to Active | Context, binding, registries and native owners retained |
| Closed | Root teardown has shut entry and drained references | Released in the existing engine-before-native order |

The implementation may combine Departing/Departed with an outstanding-cleanup
flag, but the observable boundaries stay separate. Retirement is owner-thread,
idempotent and allocation-free. It publishes ineligibility before it touches
queues. A mark made from a native callback does not free records under a
running event, custom reaction, observer or engine stack. Deferred cleanup runs
at an explicit host safe point after the outermost entry and its checkpoint
have returned. A sticky failure uses the root cleanup path instead.

Proposed libjs entry `os64_js_realm_retire` accepts a validated realm and ABI;
it can mark the current realm from a guarded callback without re-entering JS
or changing the enclosing outcome. A fresh eval/run/module execution or host
task for that realm returns a distinct non-sticky retired-realm result. It
does not set `runtime->cancelled`. Existing context/member queries remain
available for retained-object operations and cleanup while the runtime is
healthy. A new status/API changes the binding ABI and generated engine/header
identity in the implementing PR; do not silently extend ABI 3.

Keep `os64_js_realm_create`'s active-realm behavior for existing trusted hosts.
Add an explicit staged-creation/activation pair for transactional child setup;
their names and signatures belong beside retirement in the public header.
Staged creation reserves a stable slot before DOM installation, activation
requires successful setup with no tagged work pending, and abort retires the
unpublished slot. Ordinary task/run entry refuses an Installing realm. The
privileged installer guard is native-only and does not let page code choose
an arbitrary realm or open a fresh nested budget.

Proposed `os64_dom_depart` coordinates that mark with a binding's activity
state. A separate owner-thread finish operation reports BUSY while an engine
entry, DOM dispatch/reaction or provider callback could borrow its records.
Only after finish succeeds may the host free provider opaque storage. It must
remain possible to finish an already marked child after sibling exceptions.
Standalone `os64_dom_drain` and cluster root drain retain their existing meaning.

**Retirement is not revocation of existing JavaScript references.** A live
realm can directly call an ordinary function exported by a departed realm,
or manipulate a retained DOM object, within the live caller's task. This
does not reactivate the old document. Separate object/member validation from
activity checks: a blanket refusal in `d_context` would break adopted nodes,
Blob reads and Canvas contexts whose creation binding departed.

Host invocation of a callback additionally checks the callback's relevant
realm and the operation's owning document. Preserve their separate identities:
an active B listener on an A-created node adopted into B must still run after
A departs; a callback belonging to departed A must not be revived by delivering
B's timer or event. Recheck after argument conversions and preceding callbacks,
which can cause departure. The source inventory includes event listeners,
custom constructors/reactions, traversal filters and synchronous callbacks,
not just timers. Origin/entry/incumbent attribution beyond this lifecycle gate
remains F.4; runtime membership grants no cross-origin authority.

HTML explicitly permits a destroyed Document to remain script-accessible.
This is the reason for separating host activity from storage release, rather
than treating document destruction as a context-free operation.
[Document destruction](https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document).

## Job ownership and suppression

Add an engine-supported inspect/discard operation for the queue head, or an
equivalent pre-execution filter with explicit executed/discarded outcomes.
Libjs owns the filter. It is non-reentrant, does not call script or providers,
and cannot allocate. A rejected entry releases its held values and context
using the engine's normal job cleanup. Keep the remaining entries in FIFO
order. Both manual drains and host-task checkpoints use the same primitive.

The job needs two facts: the context used by its native implementation, and
the realm whose author work makes it eligible. Do not infer the latter from
the former, the promise's constructor, or the task that happens to drain it.

| Job | Execution ownership required |
|---|---|
| Promise reaction with a handler | Handler function's realm |
| Thenable assimilation | The `then` function's realm |
| Reaction with no handler | Explicit neutral ownership; propagation must not be discarded just because its promise originated in a departed realm |
| Async continuation / module evaluation continuation | The code realm that will resume; audit optimized internal reactions too |
| Dynamic import | Importing code's realm and its loader; an obsolete completion cannot enter a departed host |
| FinalizationRegistry cleanup | Its applicable registry/callback ownership; native value release is not author cleanup execution |
| Binding-created microtask | Explicit owner at enqueue, or agent-wide work with per-record eligibility (mutation observers/custom reactions) |

Candidate installation also carries a transaction tag independent of execution
ownership. Jobs enqueued during isolated setup, including neutral jobs, cannot
escape that transaction on abort. This tag must outlive its queued entries;
it cannot point to a stack-local setup record. Normal cross-realm reactions
outside installation are not tagged by whichever realm happens to settle them.

The distinction for reactions and thenables follows
[NewPromiseReactionJob](https://tc39.es/ecma262/multipage/control-abstraction-objects.html#sec-newpromisereactionjob)
and [NewPromiseResolveThenableJob](https://tc39.es/ecma262/multipage/control-abstraction-objects.html#sec-newpromiseresolvethenablejob).
The host mapping is described by
[HostEnqueuePromiseJob](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob).
Null ownership is explicit; an unrecognized foreign context is an invariant
failure, not permission to run. Bound functions and callable proxies resolve
to their underlying function realm. If `GetFunctionRealm` throws (including a
revoked proxy), use the current realm at enqueue and consume that discovery
exception; enqueue itself does not throw. The callback fails when executed.

Do not implement eager removal by walking promises and rejecting them.
Suppressing an ineligible reaction does not synthesize an AbortError or settle
its derived promise. A live B handler attached to a promise created in A can
still run after A departs if that promise is settled by a retained resolver.
A's handler on B's promise is suppressed. Handlerless propagation can still
reach a later B handler. Once a job is admitted, its custom promise capability
resolve/reject functions use ordinary ECMAScript `Call`, without another
retirement gate. An A-defined resolver wrapper can settle a live B promise
after A departs, just like another retained ordinary function. Test custom
species fulfillment and rejection through that wrapper. The document
retirement policy must not be mistaken for a test of the promise's realm.

Queued jobs predating departure and jobs queued afterward by retained direct
calls are both subject to the gate. A currently executing job may finish/unwind; it
cannot use retirement to reset its enclosing deadline. Discarded jobs do not
increment `jobs_executed` or consume the executed-job cap. Count queue visits
against a manual drain's slice work so discard floods cannot turn a bounded
slice into an unbounded loop; observe the original deadline while visiting.
Discarded heads can be removed even at the executed-job cap, but a remaining
eligible job still produces the existing job-limit failure. Once dead work
is removed, `jobs_pending` and turn closure must not strand a healthy sibling
behind an empty queue. Expose discard counts separately for the proof.

Agent-wide mutation delivery and custom reactions cannot simply carry the
realm of the first enqueuer: their shared lists can contain B's work after A
departs. Filter records at invocation, retain live records' order and keep a
live route to delivering them. Audit rejection routing and owed module
failures in the same pass. Remove obsolete report deliveries without deleting
live siblings' rejection identity/handling records.

## Stopping host activity without draining objects

Departure finish has three phases: detach/cancel external activity while
providers are still valid; release obsolete task references; clear host
callback pointers. Marking ineligible precedes all three. Cancel by the
binding's request token, not by the HTTP/2 connection shared with siblings.
Late answers carry their original binding/document identity and release their
own payload without executing script or following a cleared provider pointer.

| Resource | Departure action | Backing retained until |
|---|---|---|
| Context/global, anchors, wrapper cache, prototypes | Keep; refuse new document task entry | Root engine destruction |
| Document arena, page state, creation slots, adopted nodes | Keep storage ownership and mutations available; no slot recycling | Root native teardown after holds/pins/borrowers |
| Timers, idle/rAF callbacks, queued messages and selection/report tasks | Cancel owned deliveries and drop queue holds; refuse rearming departed activity | Task holds released at safe point; exported objects keep their own holds |
| Event listeners, custom definitions, live collections, ranges | Preserve object records; gate callback execution separately | Root registry/native release or existing object-specific release |
| Mutation/resize/intersection/performance observers | Remove departed delivery eligibility without draining shared sibling queues; preserve exported observer records | Existing disconnect/release or root teardown |
| In-flight XHR/fetch/module requests | Cancel owned request work and detach deliveries; no author abort events during native cleanup | Request-owned values/payloads released safely; exported settled data remains |
| Blob, ArrayBuffer, Response bodies and DKept | Keep refcounts and original accounting; separate pending transport from settled bytes | Existing last-reference release, with post-engine sweep as fallback |
| Canvas/context/path/gradient/bitmap | Keep pure drawing and pixel operations; route adopted-node damage to the current active document | Object/native release with creation owner still alive |
| Web Animations and layout-borrowed effects | Stop departed timeline scheduling; retain effect identity and borrowed generations; preserve active-document targets' service routing | Tree-gone acknowledgements plus native release |
| Geometry, image/text, UI, navigation, storage and report providers | Detach safely; settle storage once while provider is valid; no late provider call | Host may release opaque data after finish succeeds |

For provider-dependent methods on an inactive document, use the interface's
specified inactive-document behavior where defined; otherwise refuse the
unavailable host operation with InvalidStateError. Returning an old host
pointer or silently redirecting a creation-owned operation to the caller is
not a fallback. Pure node/Blob/Canvas operations remain available. Constructors
and methods that create background work must check the defining service owner
even when borrowed into B; cleanup methods stay usable and idempotent.

The builder must inventory provider entry sites from `dom.h` and `internal.h`
and record their inactive behavior beside the implementation. Include
`options.host_opaque`, geometry/hit/text/image providers, network callbacks,
storage, console/module/report services and the animation tree-gone path.
Mark/finish must cover conversion re-entry and cancellation hooks that return
an answer immediately. Moving a node from departed A into B still updates
native ownership, controls, observer provider routing and animation discovery
as F.2 specifies.

Root order stays: shut deliveries; drain binding/task references; destroy the
engine once; release remaining layout borrowers; sweep binding/native state;
release page states/documents and their lifetime groups. Retired members are
included. Exercise FATAL and RECLAIM teardown; payload ownership must not rely
on a finalizer that RECLAIM can omit.

## Retention limits and admission

Choose bounded conservative retention for this cut. A successful realm slot
and a consumed binding slot remain occupied until root destruction, including
departed and failed unpublished children. Cap the runtime at the existing
**65 realms total (primary plus 64)**; cap cluster bindings at 65 independently.
Do not assume libjs realm indices equal native creation slots. Preserve the
explicit mapping and the creating binding even if the node first gets a
wrapper after its creator departed. An aborted child with no public reference
still consumes its reserved slot in this model.

This is a cumulative admission limit, not merely a live-frame limit. At the
limit, refuse script-realm admission before context/DOM allocation and leave
existing realms and siblings unchanged. This result means **no script realm
for the candidate**, not a command to discard a renderable document. F.7 must
specify and test the visible navigation policy before loading is enabled:
retain the previous document, or commit the candidate as script-disabled.
It must distinguish that outcome from a failed document load and expose the
refusal in diagnostics. F.3 does not select a navigation policy for its caller.
Navigating the top-level page creates
a new cluster. Repeated child navigation can exhaust admission even after
dropping exported references; report that limitation honestly. Reclaiming
retired slots requires a later reachability design, not resetting a counter.
Tracker F.15 records reclamation. F.11 requires either that proof or recorded
Tier 1 browsing-session measurements showing acceptable realm consumption
and tested refusal behavior. A possible later optimization records whether a
realm was ever reachable by a same-origin peer at F.4's boundary; it still
needs proofs for queued jobs, engine references and native owners before
freeing a context. Cross-origin status alone is not a reclamation proof.

Keep F.2's aggregate foundation limits, including retained and staging bytes:
64 MiB HTML, 16 MiB page state, 8 MiB DOM, 256 MiB kept bodies and 128 MiB
Canvas. Keep one shared engine heap allowance (64 MiB in the standalone
profile; the host's configured value otherwise). Per-owner limits remain
additional checks. No separate departed allowance and no multiplication by
the number of realms. F.8 owns the shipping frame-tree budget, including
styles, pictures and integration overhead; these are foundation ceilings.

Engine headroom is part of admission, before `JS_NewContext`. F.3a exposes
the charged engine bytes and remaining configured allowance without running
script or latching failure. F.3b measures the retained and peak installation
cost of an empty fully installed realm in both engine profiles, including
first-use class/bootstrap costs, and records the figures and build identity.
Define a named admission reserve from the measured worst peak plus explicit
sibling operating headroom; document the margin and recheck it when bindings
grow. Refuse locally when the remaining engine allowance is below that
reserve, including in a nested setup. The reserve is admission protection,
not a promise that arbitrary later script or system OOM cannot exhaust memory.
The repeated empty install/depart proof, with A/C work between admissions,
must reach local headroom or slot refusal before an engine-memory latch.

Measure active/departing/departed/failed slot counts separately and retain the
existing byte census across retirement. Report native registry values still
held; do not label a departed binding drained or claim its bytes reclaimed.
Check admission before installation; include the peak coexistence of old and
candidate documents. F.3 exposes a reservation/preflight contract for callers;
F.7/F.8 must apply it before parsing/fetch allocation in the integrated host.

## Failed child installation

Installation uses a fresh, unexposed realm with a candidate document/state
pair. Healthy siblings cannot observe its global, wrappers, jobs or host
services before commit. Standalone setup opens a guarded task only with no
pending earlier turn. Nested setup borrows A's existing task, deadline, job
count and outcome; A may already have queued jobs. It cannot open or close
A's turn, drain A's jobs, or checkpoint arbitrary siblings. Do not evaluate
untrusted author code or invoke external providers
during the transaction; bootstrap code is restricted to installation work.

Preserve F.2's explicit ownership answer and add a result distinguishing a
local refusal, a failed consumed child, and a failed shared runtime:

1. Preflight refuses invalid membership, duplicate bindings, slots, engine
   headroom or native charges before consumption. Caller retains the
   document/state pair. Local nested refusal returns an explicit native
   admission result with no pending engine exception or sticky latch; A's
   task continues unchanged. F.7 maps that result to its documented frame
   API/navigation behavior rather than treating it as shared-runtime failure.
2. After consumption, keep a valid cleanup shell, owned and charged by the
   cluster, even if installation stops halfway. The failed realm becomes
   ineligible; detach its partial services and discard its owned jobs before
   a task checkpoint could execute them. Do not set `cluster->poisoned` for
   a local native refusal or ordinary installation exception on a healthy
   engine. The caller must not free consumed backing.
3. Roll back candidate additions to shared observer/reaction/index structures
   by ownership, and restore sibling-visible lists and ordering. Registered
   engine classes may remain reusable; per-context prototypes/globals may
   remain private in the failed realm. Retained cleanup shells stay charged
   and consume admission; rollback does not promise zero retained bytes.
4. A shared JS allocator limit/OOM, invalid clock, runtime cancellation or
   sticky execution failure still ends the shared runtime. Do not clear the
   allocator flags or failure latch to make a failed installation look local.
   Preserve the original failure while completing non-executing cleanup.
5. While Installing, a privileged setup guard may run that candidate's
   bootstrap and tagged jobs under the existing setup budget, without opening
   ordinary task entry or providers. Commit publishes the binding and enables
   host delivery only after setup and its candidate-tagged work succeed.
   A candidate-only setup drain can execute tagged jobs without running or
   reordering A's pre-existing jobs; it charges the enclosing budget. Abort
   discards only the candidate's tagged jobs, including neutral propagation,
   before leaving the setup guard. Both commit and abort preserve the order
   of other queued jobs. Standalone setup then closes its own task; nested
   setup returns to A without a sibling checkpoint. Any bootstrap job that can
   publish a child reference or call a sibling/provider violates the staging
   contract and must be removed or staged before this path can ship.

The transaction needs explicit native partial-initialization cleanup; root
`binding_drain` is not reusable unchanged. Allocation-failure sweeps must
separate native local refusals from engine-sticky failures. For a local cut,
run A and C afterward, mutate existing adopted objects, and successfully
admit another child within remaining limits. For a global cut, assert that
new script is refused and both teardown profiles release native payloads.

## Budget and interrupt decision

One outer task/turn owns the deadline, executed-job cap, stack allowance and
failure outcome. A → B → C calls, raw engine calls from native bindings,
getters, coercions, module operations and checkpoints borrow that budget.
Changing the configured duration affects the next task, not the current one.
Departure and dropping jobs cannot renew time or create a second checkpoint
budget. Native work retains its own bounded I/O and `os64_js_check_budget`
obligations; engine interrupts do not stop a blocked syscall.

**Use IFRAME.md's conservative runtime-wide overrun fallback for F.3.**
Normal child departure and local admission refusal leave siblings running.
Execution/job exhaustion, runtime cancellation, engine memory failure and
host failure keep libjs's existing sticky runtime-wide behavior. Stack
overflow keeps its existing classification: EXCEPTION unless the engine
exposes a reliable STACK limit signal; do not parse exception text. A timeout
in B entered from A therefore retires the whole script cluster; no surviving
host re-enters an interrupted environment. This avoids changing the public
failure contract while proving ordinary document retirement. It is a stated
limitation, not a claim that child timeout recovery is impossible or proven.

F.3a must inspect `JS_ThrowInterrupted`, interpreter unwinding, native callback
boundaries and both job-drain paths. Test interruption during allocation-heavy
loops, nested cross-realm calls, catch/finally, Promise reactions and native
coercions. Assert sticky refusal after unwind, unchanged outer accounting and
clean native teardown. Keep results in the validation record. An experiment
that clears a latch and runs a sibling is not acceptance evidence for recovery.
End the audit with a written verdict on the generated engine: what unwind
soundness is established and what interrupted-realm attribution is missing.
The existing EXECUTION/JOBS/MEMORY limit reasons already distinguish causes;
the unresolved question is safe recovery, not inventing a new classification.

Per-child timeout recovery requires a separate reviewed extension: distinguish
recoverable execution/job exhaustion from engine/allocator/clock failure;
identify interrupted realms across raw JS calls and queued-job execution;
prevent future entry into failed environments while preserving legitimate
retained references; prove heap and native invariants after complete unwind.
Book the runtime-wide fallback in DEBTS/DIVERGENCES when F.3 lands and name
this proof as its exit condition. Tracker F.16 records that recovery work.
F.11 requires either the recovery proof or P5 measurements showing Tier 1
frame workloads remain within the shared budget, with the fallback explicitly
recorded in acceptance. Root/global cancellation remains available
for emergency stop; document navigation uses departure instead.

## Required evidence

Use the current F.2 source as the comparison baseline, not its pre-review
validation totals. Fixtures should assert behavior and ownership, not just
that a call survived. Add focused selectors to existing harnesses where useful.

| Proof | Required counterexample and observation |
|---|---|
| Lifecycle | Retire B idle, inside its own callback, and from A while B is on the stack; existing stack unwinds, new B tasks are refused, A/C remain usable after ordinary departure |
| Jobs | Interleave A/B/C jobs, retire B, verify exact surviving order and no B side effect; repeat after jobs enqueue additional jobs and across manual slices/checkpoints |
| Promise ownership | Both directions of cross-realm handlers/resolvers, already-settled and pending promises, null handlers, thenables, bound/proxy/revoked callbacks, async/await, custom capabilities and late modules |
| Shared queues | B enqueues the cluster delivery first, A/C enqueue afterward, then B departs; live mutation/reaction records still deliver in order |
| Callback ownership | A-created node adopted into B with both A and B callbacks; depart A; B's callbacks and node operations survive, A's host callbacks do not |
| Providers | Poison/free provider opaque storage after departure finish; late network/module/image/geometry/report results and borrowed methods never touch it; B/C providers still receive their own calls |
| Retained objects | Keep a function, unwrapped descendant, Attr/view, range, observer, custom definition, Blob/ArrayBuffer/Response, Canvas context/gradient and borrowed effect across creator departure; check values, identity, destination routing and native charges |
| Failure isolation | Sweep candidate admission and installation allocations under both teardown profiles; distinguish non-consumed, consumed-local and global-sticky outcomes; no sibling drain or duplicate free |
| Nested failure isolation | Install B inside A's task with A jobs already queued; abort at each stage, discard candidate-tagged work only, then verify A's jobs run in order with unchanged task outcome and budget |
| Bounds | Measure retained and peak installed-empty-realm engine bytes; tiny caps and full profile; repeated install/depart with A/C work reaches local headroom or slot refusal before memory latch; prior realms remain usable; no slot reuse for unwrapped nodes |
| Budget | A → B → C cannot extend time or job cap; dead jobs are discarded without executing; a flood remains slice/deadline bounded; interruption remains sticky across later entries |
| Teardown | Departed and failed shells plus live siblings; alternate layout-borrow release order; FATAL and RECLAIM release native payloads, handles and registry roots |

Run relevant libjs runtime/contract-header tests, the complete shipping DOM
suite and focused instrumented realm/retirement/provider suites; broaden to
affected observer, animation, fetch/streams and custom-element tests based on
the implementation audit. Preserve ASan/UBSan and LeakSanitizer; rerun outside
the sandbox if attachment restrictions prevent the leak check. Use bounded
logs and source-qualified evidence. Run focused mutants after the positive
cases: wrong job realm, post-execution filtering, shared-queue suppression,
slot reuse, provider-after-finish, sibling drain, and budget renewal.

Rebuild/link the affected target libraries and a three-realm guest consumer.
Its parent observes the actual child exit status; exercise ordinary B
departure, a retained A/B/C object and a refused candidate, with heap checks.
Compare named results in the affected pinned WPT families without weakening
expectations. This foundation has no P5 acceptance requirement; integrated
frame behavior and hardware acceptance remain F.11.

## Completion and follow-ons

F.3 stays in progress after F.3a. Each implementation PR records its exact
source, tests, proof limitations and review result. F.3b prepares the row's
done flip with both implementation PR numbers after its acceptance. Update
libjs CONTRACT/TEARDOWN, DOM lifecycle/provider contracts, IFRAME.md and the
retention/overrun DEBTS and DIVERGENCES entries with the implemented behavior.
Do not mark this design PR as an implementation or retire the F.4/F.8/F.11
proof obligations. The retained-slot limit and runtime-wide overrun fallback
must remain visible when the browser integration is proposed.
