# F.3b DOM validation — delivery checkpoints

2026-11-12. Work in progress against the
[reviewed retirement brief](IFRAME_F3_RETIREMENT.md). This branch starts from
`e696277f`, the merge of F.3a PR #552 into design PR #546. The F.3a review
was clean; the six design findings on #546 have evidence replies and are
resolved. Draft PR #553 carries the first DOM checkpoint at `0f9055d3`.
F.3 remains `code`, not complete.

## Implemented boundary

Mutation-observer notification and the custom-element backup queue are
agent work. Their engine jobs have neutral callback/activity ownership, so
retirement of the first enqueuing realm does not discard live siblings'
deliveries. The retained execution context still identifies the shared DOM
agent. Engine staging continues to tag these jobs with the current candidate
transaction; neutral ownership does not bypass candidate abort.

Delivery checks the operation's activity document and the function's realm.
Observer activity belongs to its registration owner. A custom-element
reaction or upgrade belongs to the element's **current** document, including
after adoption; the retained definition's creator is not an activity owner.
Bound functions and callable proxies resolve to their function realm. A
revoked proxy follows the ordinary exception-reporting path. Synchronous
`createElement` constructors use the same eligibility check as queued upgrades.

An ineligible observer loses queued records and transient registrations
without allocating a MutationRecord array. A skipped custom reaction releases
its queued payload. Slot signals for inactive documents are consumed without
dispatch. Eligibility is checked at each delivery, so an earlier callback
can retire a later recipient without suppressing unrelated work.

Storage validation (`d_context`) remains separate. Retained DOM wrappers,
observer methods, definitions and ordinary exported functions remain usable;
this checkpoint does not free a binding or introduce a departure API.

Event delivery applies the same activity/function checks to ordinary
listeners, resolved `handleEvent` methods, handler slots and window.onerror.
A `handleEvent` getter can retire a realm or adopt the target; delivery
rechecks the current document and returned method afterward. Inactive-target
listeners are skipped before their getters run. Host dispatch enters through
the active target when an object listener's registration realm has retired,
then resolves and gates its method inside that guarded entry. Suppressed
handlers neither cancel an event nor increment the host listener-call count.

Timers, requestAnimationFrame, idle callbacks and queueMicrotask refuse
rearming inactive service owners with InvalidStateError. Timer and idle
argument conversions precede that check, so reentrant retirement cannot
install new work. A binding microtask names both the callback function realm
and the owning document; revoked-proxy discovery falls back at enqueue and
the callback raises its ordinary error when executed. Host scheduler queries
and firing APIs omit inactive bindings. A dead callback on a live interval
removes that interval, while a refused callback in a live rAF/idle batch is
discarded without failing siblings. Timeout-driven idle callbacks have the
same callback gate as ordinary idle delivery.

Retained IdleDeadline objects report zero time remaining without calling the
departed document's clock. Retained input selection can still change, without
queuing a select task in its inactive document; performance entries likewise
do not rearm their inactive observer task. Existing queued payloads still
need the departure-finish cleanup described below.

Window and port messages refuse inactive posting activity before serialization
and recheck after clone getters, before detaching transfer buffers. Channel
construction rechecks after reading the new target's prototype. Borrowed port
methods follow the port's owner, and inactive targets have no message scheduler
opportunity. A weak port list lets task cleanup release payloads waiting on
unstarted ports, as well as the window queue, without destroying exported port
objects. This internal cleanup seam is not yet a public departure finish.

The missing-global diagnostic hook goes through a binding-owned bridge, so an
ordinary retained function can still run without calling its retired document's
provider. The broader provider detachment and borrow protocol remains below.

Performance, resize and intersection observers check callback eligibility
before constructing delivery records. A skipped performance/intersection
callback consumes its obsolete entries; a skipped resize callback consumes
the sampled sizes without keeping an identical report owed. Later eligible
observers still run. Inactive observer documents have no delivery or
measurement opportunity. An active observer also skips an inactive target's
or explicit root's measurement provider. Its registration remains available:
adopting a retained target into an active document can resume observation.

TreeWalker and NodeIterator check a filter's current target document and
function realm, including after an `acceptNode` getter moves the target.
An unavailable filter operation throws InvalidStateError; traversal without
a filter remains a pure tree operation. URLSearchParams, Headers and FormData
`forEach` instead have no document-owned activity: retained data can invoke a
live callback, while a retired callback is refused before each invocation.
NodeList retains its specified Array intrinsic, and ordinary JavaScript calls
and conversion iterators remain ordinary execution. This follows Web IDL's
distinction between [indexed and pair iteration](https://webidl.spec.whatwg.org/#es-iterable);
the inactive-callback refusal is this foundation's retirement contract.

## Evidence

`tools/test_dom_host.sh --realm-retirement` runs a three-binding fixture under
both FATAL and RECLAIM teardown policies. The fixture uses trusted engine
retirement; it does **not** claim to test DOM departure cleanup.

The new regression failed against unmodified F.3a: a job first queued by B
was discarded on B's retirement, stranding A/C observer deliveries. Final
tests cover both shared queues, callback/activity disagreement, bound and
proxy callbacks, revoked-proxy handling, queued and synchronous constructors,
slotchange suppression, an adopted element retaining B's definition with a
live C callback, later queue reuse, retirement during notification, retained
object access and balanced native teardown.

| Final focused suite | Shipping engine | Instrumented engine |
|---|---:|---:|
| Retirement delivery, messages, observers, traversal and retained data | 404 checks | 404 checks |
| Cross-realm binding routing | 678 checks | 678 checks |
| Events | 1,020 checks | 1,020 checks |
| Mutation observers | 32 checks | 32 checks |
| Custom elements, including allocation failures | 298 checks | 298 checks |
| Animation callbacks | 106 checks | 106 checks |
| Performance and performance observers | 523 checks | 3,169 checks |
| Intersection observers | 409 checks | 409 checks |
| Resize observers | 239 checks | 1,309 checks |
| Traversal | 11 checks | 11 checks |
| Fetch, Headers, Blob and FormData | 1,591 checks | 10,238 checks |
| Modern DOM, including URL and data-iteration coverage | 5,550 checks | 19,948 checks |
| P2b idle/message/clone and allocation sweeps | 1,174 checks | 13,324 checks |
| Core DOM, including retained selection | 332 checks | 332 checks |

These runs retain ASan/UBSan and LeakSanitizer, outside the sandbox that denied
LSan thread attachment during F.3a. Unchanged host/engine objects from the
final F.3a run were reused; `core.c`, `observer.c`, `custom.c` and the host
test translation unit were rebuilt for the shared-queue checkpoint. The event
extension also rebuilds `event.c` and the expanded test translation unit;
retirement, cross-realm and event suites were rerun on that source. The
scheduled-work extension rebuilds the affected core, browsing, timer, idle,
animation, selection and performance units and reruns retirement, cross-realm,
animation, performance, P2b and core suites. Observer/custom totals are from
the shared-queue checkpoint and event totals from the event checkpoint. The
message/provider extension rebuilds every DOM host object for its changed
binding layout, then reruns retirement, cross-realm, P2b and performance suites.
The observer/traversal/iteration extension reruns the affected suites above
after rebuilding their changed units and the shared eligibility helper.
The native `dom-library` target also
builds successfully from this worktree. No full-suite, WPT, QEMU or P5
acceptance is claimed for this intermediate checkpoint.

Nine isolated source mutants were compiled and rejected by named assertions:
restoring first-enqueuer ownership for either shared queue; ignoring callback
realm or activity eligibility; treating a definition's creator as the current
document; bypassing queued-upgrade, synchronous-constructor or slotchange
eligibility; and retaining discarded observer records. Mutants used temporary
source copies and did not modify the implementation worktree.

Seven further event mutants were compiled and caught: forcing entry through
a retired registration realm, omitting function/method/window.onerror gates,
using stale activity after a getter, losing admitted-call accounting, and
using a target's creation binding instead of its current document. The new
event fixture first reproduced retired callback execution and live object
handler refusal against the preceding checkpoint. Its expected revoked
observer error is explicitly consumed and checked separately from the host
event outcome, so an earlier report cannot contaminate the dispatch result.

Ten scheduled-work mutants were caught: wrong microtask callback ownership,
missing microtask activity ownership, rearming after conversion, retaining a
dead interval, treating a skipped rAF callback as a task failure, bypassing
idle timeout eligibility, reading a retired deadline's clock, queuing a
retired select task, advertising a retired timer as due, and throwing during
revoked-proxy enqueue discovery. The preceding checkpoint reproduced wrong
microtask delivery, repeated dead-interval wakeups and batch interruption.

Nine message/provider mutants were caught: missing pre-serialization or
post-clone checks for window/port messages, routing a borrowed method by its
creation realm, permitting channel construction after retirement, leaving
unstarted-port payloads behind, advertising retired messages as due, and
calling the retired diagnostic provider. Tests cover five reentrant conversion
or clone paths under both teardown policies, intact transfer buffers on
refusal, idempotent payload release and retained-port use afterward.

Ten render-observer mutants were caught: callback gates for the three observer
families, inactive measurement entry for resize/intersection, inactive target
and root providers, discarded performance records, and both owed-measurement
queries. Eight traversal/iteration mutants were caught: a filter getter on an
inactive target, missing function eligibility, stale target ownership after a
getter, each of the three pair-iteration callback gates, wrongly treating the
data creator as activity owner, and checking callback eligibility only once.
The fixture also reads retained Blob, byte-backed Response and closed-stream
Response data after their creator retires. These are settled-data proofs,
not a claim that the transport cancellation/provider audit is complete.

## Remaining F.3b work

- Publish the allocation-free departure mark and idempotent safe-point
  finish protocol, with reentrant dispatch/reaction/provider-borrow proofs.
- Audit and gate the other host delivery paths, self-hosted Streams callbacks,
  report routing and provider
  entry points. Inventory and detach provider storage only after borrowers
  finish; preserve retained DOM/Canvas/WebAnimation identities.
- Cancel binding-owned work by its own request tokens, consume late results
  safely, and release scheduled payloads without closing shared connections.
- Install a child DOM transactionally with staged engine setup and native
  rollback, preserving siblings and accounting for retained failed shells.
- Measure installation peak and retained active/departed charges under both
  allocator profiles, set the production reserve/margin, and prove repeated
  admission refuses locally before sticky runtime failure.
- Complete the brief's integration, allocation, budget, mutant, guest and
  pinned-WPT evidence before marking F.3 done or requesting final review.

Frame enablement, independent realm reclamation and child-local overrun
recovery remain outside F.3b. No P5 handoff is required for this checkpoint.
