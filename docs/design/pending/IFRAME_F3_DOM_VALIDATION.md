# F.3b DOM validation — delivery checkpoints

2026-11-12. Work in progress against the
[reviewed retirement brief](IFRAME_F3_RETIREMENT.md). This branch starts from
`e696277f`, the merge of F.3a PR #552 into design PR #546. The F.3a review
was clean; the six design findings on #546 have evidence replies and are
resolved. Draft PR #553 carries the first DOM checkpoint at `0f9055d3`.
F.3 remains `code`, not complete.

The rendering-provider checkpoint merges `userland` through `a965756f` in
`231c9f60`. Its host objects were rebuilt for the merged headers and added
binding/cluster loan counters. Earlier results below remain checkpoint-specific.

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

Streams separate author callbacks from internal continuations. The private
factory checks the actual function realm for start/size and promise-returning
pull/cancel/write/close/abort/transform/flush callbacks; an unavailable callback
throws or rejects with InvalidStateError as its operation requires. Bound and
proxy callbacks retain that rule, and revoked proxies retain TypeError.
Retained stream records can invoke live callbacks from another realm.

Internal promise reactions use native neutral wrappers (engine patch 0021),
and internal microtasks carry explicit neutral ownership. These helpers are
private to the factory. They preserve budget charging and installation tags,
while leaving ordinary function-realm identity intact. This repairs a concrete
failure: a closed Response body could be read after its creator retired, but
cloning it stranded tee continuations. Default and byte-stream clones now
finish both branches. Internal neutrality does not bypass the author checks.

Pipe abort is registered in AbortSignal's native algorithm list, with traced
function holds and explicit removal when the pipe settles. It runs before
author abort events, including for a retained signal whose document departed.
Completed-pipe removal preserves the order of later algorithms. Signal
finalization clears Response back-pointers before releasing internal closures
that might own those Responses. This keeps cleanup independent of event
callback eligibility and `stopImmediatePropagation`.

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
| Retirement delivery, messages, observers, traversal, Streams and retained data | 434 checks | 434 checks |
| Cross-realm binding routing | 678 checks | 678 checks |
| Events | 1,020 checks | 1,020 checks |
| Mutation observers | 32 checks | 32 checks |
| Custom elements, including allocation failures | 298 checks | 298 checks |
| Animation callbacks | 106 checks | 106 checks |
| Performance and performance observers | 523 checks | 3,169 checks |
| Intersection observers | 409 checks | 409 checks |
| Resize observers | 239 checks | 1,309 checks |
| Traversal | 11 checks | 11 checks |
| Fetch, Headers, Blob, FormData and Streams | 1,514 checks | 10,526 checks |
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
The Streams extension rebuilds the complete DOM host cohort and both engine
profiles for patch 0021, then reruns retirement and fetch/Streams coverage.
Focused engine realm tests pass 3,137 shipping and 32,820 instrumented checks,
including native continuation identity, bound/proxy ownership, executed-job
limits and candidate abort. The native `dom-library` target also builds
successfully. No final full-suite, QEMU or P5 acceptance is claimed for this
intermediate checkpoint.

The integrated Yonder runner matches the recorded Streams WPT expectations at
pin `a699fb16767be39ba3e61a8e8b6bef60383f08ee`, with leak checks enabled:

| WPT group | Pass | Existing fail | Not run |
|---|---:|---:|---:|
| readable-streams | 349 | 13 | 0 |
| readable-byte-streams | 245 | 4 | 0 |
| writable-streams | 197 | 0 | 0 |
| transform-streams | 133 | 1 | 0 |
| piping | 230 | 0 | 0 |
| transferable | 10 | 71 | 1 |

There are no timeout or harness errors. Worker/shadow-realm variants remain
excluded by the host runner. Existing failures are not claimed as passes.
Nine Streams mutants are caught: each of the four start paths, size callbacks,
promise callbacks, reaction ownership, microtask ownership and incorrectly
using the stream's creation document as callback activity. Together with the
prior campaigns below, 62 isolated source mutants are caught.

The native pipe-abort extension reruns retirement and fetch/Streams under both
engine profiles, and piping WPT retains 230 passes with no failures. Its
regressions reproduce stranded aborts before the fix, then cover live/retired
signals, stopped author events, removal order and collectible unobserved pipe
cycles. Three additional mutants (skipped internal abort, reordered removal,
untraced function holds) are caught, bringing the caught total to 65. The
tracing mutant initially survived while the fixture retained the signal; the
unobserved-cycle WeakRef assertion closes that gap. A destructor-only hold
omission mutant is not detected by this fixture and is not counted as caught.
The normal implementation passes the leak-enabled allocation sweeps.

A separate audit observation remains unresolved: a B-created readable stream
piped into an A-created writable stream fails the self-hosted private-brand
check, even before departure. The retained-pipe retirement regression uses
B-created streams on both sides to isolate the lifecycle behavior. Mixed-realm
stream interoperability is not claimed by these results.

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

## Rendering-provider checkpoint

Native provider loans are counted on both their binding and cluster, including
nested sibling calls. Rendering entry points refuse inactive owners and stop
multi-pass queries or native walks when a callback marks retirement. Image
loans capture their producer and opaque through consumption and release, even
if that producer is replaced during acquisition. Font loans extend through run
release. Retained Canvas pixels remain available, and adopted nodes use their
new document's providers. Completed layout cost remains charged when the
result is refused after retirement.

The [provider inventory](IFRAME_F3_PROVIDERS.md) lists guarded sites and the
remaining families. These counters are groundwork for safe-point finish;
they do not yet authorize a host to free provider opaque storage.

Both shipping and instrumented engine profiles pass with ASan/UBSan/LSan:

| Coverage | Shipping | Instrumented |
|---|---:|---:|
| Provider retirement, both teardown policies | 854 | 854 |
| Existing retirement and initial provider cases | 1,270 | 1,270 |
| Cross-realm routing | 678 | 678 |
| Canvas | 548 | 548 |
| Resize | 229 | 1,309 |
| Intersection | 409 | 409 |
| Geometry, scrolling, view and history | 313 | 313 |
| Ranges | 126 | 126 |

The provider-only total includes stronger assertions added after the combined
retirement run: synchronous post-mark refusal, replacement-safe image release,
and cost accounting. Fourteen scenarios each run under both teardown policies;
they include expanded hit/range buffers, native image polling, font refusal,
observer measurement, and normal/reset Canvas damage visitors. The font test
covers the refusal path; it does not independently prove a successful run's
release during retirement.

Nine isolated mutants are caught: missing cluster accounting, clearing an
outer loan on a nested return, admitting inactive owners, losing the captured
image release, ending its loan before release, accepting retired geometry,
continuing damage delivery after retirement, and dropping completed cost in
either hit or range queries. The cumulative caught count is 74. The normal native
`dom-library` target builds successfully.

The complete shipping-engine DOM run passes 46,421 checks with 3,991 native
binding allocation sweeps and leak detection enabled. The instrumented results
above are focused runs; they are not a completed full instrumented acceptance.

An initial broad run found stale 64 KiB history expectations after the merged
limit grew to 2 MiB. Updated tests prove exact-limit primitive JSON acceptance,
one-byte overflow refusal and preserved state. They use a fresh binding so
the preceding large media-query fixtures do not exhaust native storage before
the JSON boundary is reached. This changes tests, not the merged history limit.

## Synchronous-service checkpoint

Dialogs, parser write/open, activation/validation notices, script-connected
notices, focus, entropy, user-agent text, cookies and two diagnostic hooks
now participate in provider loan accounting. Entry checks follow argument
conversion where conversions can run script. A cookie response that retires
its document cannot start a larger retry; a script-connected callback that
retires its document prevents later notices in that tree change. Focus reads
on a departed window document return null/false without host calls.

Entropy supplied during retirement is erased from scratch and does not change
the caller's output. Empty fills and SHA-256 on retained Crypto objects still
work. Constant Navigator values no longer query the user-agent provider;
agent-derived values borrow the text through copying. Retained Attr records
still detach safely when their native attribute disappears, without calling
an inactive diagnostic hook.

The new native-provider scenarios failed against the preceding implementation
before these guards were built. The final fixture has 29 native-callback
scenarios and five conversion/invalid-event scenarios, each under both teardown
policies. The complete shipping-engine DOM suite passes **47,551 checks** and
3,991 native binding allocation sweeps. The instrumented combined retirement
suite passes **2,418 checks**. The affected focused suites also pass:

| Coverage | Shipping | Instrumented |
|---|---:|---:|
| Cross-realm routing | 678 | 678 |
| Core DOM | 332 | 332 |
| Window, including crypto | 648 | 820 |
| Document | 986 | 1,502 |
| Attributes | 2,499 | 3,426 |

ASan/UBSan/LSan remain enabled; the native target library builds. The final
diagnostic extension is covered by the complete shipping run and instrumented
retirement run; focused-suite totals above precede that fixture extension.
Eleven additional isolated mutants are caught: late alert/write/activation,
an unborrowed validation notice, continued script notifications, publication
of entropy after retirement, late focus/cookie-set/broken-attribute/global-miss
calls, and unborrowed user-agent text. The cumulative caught count is **85**.
These are service-entry proofs, not a public departure/finish guarantee or
full instrumented/guest acceptance.

## Clock, timing and zoom checkpoint

Host millisecond/microsecond reads now share provider-loan helpers. Binding
storage retains their last samples and the last navigation-timing snapshot,
so inactive retained objects do not call old clock/timing opaque storage.
`performance.now()` and mark/measure operations needing a current timestamp
refuse inactive activity. Explicit timestamps, event timestamps and sampled
navigation timing remain usable. Clock storage is included in the binding's
native charge; its layout change required rebuilding the complete host cohort.

Timer and idle-timeout creation, frame scheduling and message posting recheck
after clock callbacks. In particular, the MessagePort timestamp is sampled
before transfer detachment, so retirement from that native read preserves the
caller's buffer. Native idle/frame/Web Animation opportunities stop after
clock-triggered retirement. Web Animation effect/provider cleanup remains in
the remaining inventory; these are clock and scheduling guards.

Screen/media queries borrow their zoom provider and recheck activity before
publishing results. A media environment uses one zoom sample. A failed
matchMedia construction releases its private native record before publication;
native media delivery stops on retirement. Stored color depth and orientation
remain readable.

The fixture covers 50 native-callback scenarios and five conversion/invalid
event scenarios, each under both teardown policies. An initial native media
case left the viewport unchanged and did not trigger re-evaluation; the final
case changes the viewport before testing retirement. The preceding implementation
fails the new clock/zoom assertions. Final results retain ASan/UBSan/LSan:

| Coverage | Result |
|---|---:|
| Complete shipping-engine DOM suite | 50,007 checks; 3,991 native allocation sweeps |
| Provider retirement, shipping and instrumented | 4,440 checks each |
| Instrumented performance | 3,169 checks |
| Instrumented animation callbacks | 106 checks |
| Instrumented Web Animations | 675 checks |
| Instrumented geometry/scroll/view/history | 313 checks |
| Instrumented P2b messaging/idle/clone | 13,324 checks |
| Instrumented events | 1,020 checks |
| Instrumented fetch/Streams | 10,526 checks |

All pass. The native target library also builds. Twelve isolated mutants are
caught: unborrowed clock/zoom calls; lost clock/timing samples; timer, idle
timeout and rAF publication after clock retirement; accepting a current-time
result after retirement; publishing a new media query or match result after
retirement; sampling a port timestamp after transfer; and continuing a native
Web Animation update after clock retirement. The cumulative caught count is
**97**. Full instrumented and guest acceptance remain outstanding.

## History and navigation checkpoint

History length/update/fragment callbacks borrow provider storage and recheck
activity on return. URL/title conversion and state serialization can retire
the binding; none may lead to a later native history call. History methods
and properties use the interface's SecurityError for inactive documents,
including state and scroll restoration. Already exported state objects keep
their identity and remain mutable. Location operations refuse unavailable
navigation with InvalidStateError; saved address reads remain local to the
binding. Stable proxy routing across replacement documents is F.4 work.

A host callback that accepts an entry and marks retirement does not cause a
local binding commit or fragment fallback. The host's accepted entry is not
rolled back. A refused callback that retires also prevents fallback. Native
history moves refuse inactive documents, owed popstate/hashchange events are
discarded, and taking a previously queued navigation discards its node holds.
Link/form activation cannot enqueue a departed document's navigation. Removing
the history provider does not revive the local fallback path.

The fixture now covers 56 native callback scenarios and 15 conversion/event
scenarios under both teardown policies. Active sibling history and exported
state are checked after departure. Tests release unexpected returned navigation
holds so a failing baseline reports assertions instead of failing fixture
teardown. A duplicate lexical declaration in the extended fixture was corrected
before the final run. The final fixture reports **984 failures** against the
preceding implementation and passes against the guards:

| Coverage | Result |
|---|---:|
| Complete shipping-engine DOM suite | 52,381 checks; 3,991 native allocation sweeps |
| Provider retirement, shipping and instrumented | 6,814 checks each |
| Instrumented scroll/view/history | 313 checks |
| Instrumented events/navigation | 1,020 checks |
| Instrumented cross-realm routing | 678 checks |
| Instrumented window | 820 checks |
| Instrumented windowless document implementation | 2,968 checks |

All pass with ASan/UBSan/LSan enabled. The native DOM library builds. Twelve
isolated mutants are caught: unborrowed history length/update/fragment calls;
accepting a length/update/fragment result after retirement; late native moves
and events; taking obsolete navigation; accepting Location after conversion
retirement; and inactive history-state/restoration access. The cumulative
caught count is **109**. Full instrumented, guest and current pinned-WPT
acceptance remain outstanding.

## Storage service and final-settlement checkpoint

Storage identity/load/commit calls hold provider loans. Normal access refuses
inactive areas, including cached and memory-only maps; conversions, proxy
visibility checks, load retries and quota-driven commits recheck retirement.
Borrowed Storage methods use their receiver's binding rather than the method's
creation binding. A retained method from a retired realm can serve an active
receiver, while borrowing an active method cannot revive a retired receiver.

Ordinary settlement stops on retirement, including from either its scheduling
clock or the commit's clock. Reentrant settlement, forget, provider replacement
and drain cannot disturb borrowed maps. Retirement during a failed load does
not discard owed writes. The quiescent native cleanup entry attempts each
owed area's final commit once, even during closed root drain, then drops
pending changes and detaches the provider. A failed final serialization does
not leave a retry after detachment. Maps and exported objects are retained;
the public departure finish/barrier is still outstanding. Byte-format walk
and merge utilities are independent of document bindings; a provider's use
of them remains inside that provider's loan.

The fixture covers 64 native callback scenarios and 22 conversion/event
scenarios under both teardown policies, plus five final-cleanup cases under
each policy (success, full store, failed store, serialization allocation
failure and closed root drain). Pressure tests fill the change-list quota
with removed keys and retire during the early commit. The final fixture
reports **358 failures** against the preceding storage implementation.

| Coverage | Result |
|---|---:|
| Complete shipping-engine DOM suite | 54,791 checks; 3,991 native allocation sweeps |
| Provider retirement, shipping and instrumented | 9,224 checks each |
| Instrumented modern web suite, including storage and allocation sweeps | 20,236 checks |
| Instrumented storage quota | 10 checks |
| Instrumented cross-realm routing | 678 checks |

All pass with ASan/UBSan/LSan enabled. The native DOM library builds. Thirteen
isolated mutants are caught: unborrowed identity/load/commit calls; interpreting
a failed load after retirement; committing after clock retirement; using the
method creator instead of its receiver; inactive map/getter access; accepting
a new write after a retiring pressure commit; continuing names after a proxy
visibility mark; omitting the retired final commit; and reentrant forget/drain.
One mutation-runner anchor needed disambiguation before its remaining cases
ran. The cumulative caught count is **122**. Full instrumented, guest and
current pinned-WPT acceptance remain outstanding.

## Transport ownership and cancellation checkpoint

Fetch and XHR providers now hold binding/cluster loans. Each request captures
its producer table, so replacing or clearing the installed network cannot
redirect cancellation or body demand to another producer. A start callback
that accepts an id and retires the document cancels that id before returning;
synchronous XHR retains the original release callback through response
consumption, even if run clears the installed provider. Its stop predicate
also observes document retirement.

Native cleanup unlinks each request before calling cancel, removes fetch
signal algorithms and XHR timer/send bookkeeping, releases request holds,
and detaches the installed table. Cancellation can reenter delivery or drain
without double cancellation or script entry. Incoming whole/head/body/end
payloads remain host-owned; inactive or provider-reentrant delivery does not
read them into the engine. Retained XHR bytes, queued stream bytes and settled
Response clones survive. An explicit signal abort between mark and cleanup
cannot settle pending retired transport work.

A cross-realm signal source must route each dependent algorithm through the
dependent's binding. The audit found the previous path used the initiating
controller's binding: two documents' equal numeric request tokens could cancel
the wrong request. The collision regression produced **10 failures** before
the routing correction, then passed for active and retired dependents. Signal
timer cancellation also uses the signal's owner. Borrowed XHR methods use the
XHR receiver's binding, and send/open recheck argument conversion, loadstart
and clock retirement before publishing work.

The transport fixture covers 12 scenarios under both teardown policies,
including shared native request ids, colliding binding tokens, producer
replacement, retirement during start/more/run/release/clock callbacks, late
payloads, abort races, retained data and closed root cleanup. Four added XHR
conversion/event cases bring that provider fixture to 26 such cases. The final
transport fixture reports **240 failures** against the preceding transport
implementation.

The complete shipping-engine DOM suite passes **55,987 checks**, including
**3,991 native allocation sweeps**. Instrumented transport retirement passes **1,084 checks**, provider retirement
**9,336**, fetch/Streams **10,526**, and realm routing **678**, with
ASan/UBSan/LSan enabled. Fourteen isolated transport mutants are caught,
including wrong-producer cancel/more/release, missing provider loans, late
retired delivery, pending-promise settlement after retirement, retained abort
links, omitted XHR bookkeeping and source-binding abort routing. The cumulative
caught count is **136**. The initial missing-loan mutant also removed its
matching leave after an unbalanced version obscured the intended failure.

The native DOM library builds. This is a quiescent native-cleanup proof,
not the public departure finish protocol. HTTP/2 connection ownership was
inspected separately: the existing cancellation path releases the request's
stream; no HTTP/2 implementation changes were needed. The fixture does not
replace real-network, guest, P5 or final pinned-WPT acceptance.

## Remaining F.3b work

- Publish the allocation-free departure mark and idempotent safe-point
  finish protocol, with reentrant dispatch/reaction/provider-borrow proofs.
- Audit and gate the other host delivery paths, self-hosted Streams callbacks,
  report routing and provider
  entry points. Inventory and detach provider storage only after borrowers
  finish; preserve retained DOM/Canvas/WebAnimation identities.
- Integrate native request and scheduled-payload cleanup into safe-point
  finish, including the separate module-loading path.
- Install a child DOM transactionally with staged engine setup and native
  rollback, preserving siblings and accounting for retained failed shells.
- Measure installation peak and retained active/departed charges under both
  allocator profiles, set the production reserve/margin, and prove repeated
  admission refuses locally before sticky runtime failure.
- Complete the brief's integration, allocation, budget, mutant, guest and
  pinned-WPT evidence before marking F.3 done or requesting final review.

Frame enablement, independent realm reclamation and child-local overrun
recovery remain outside F.3b. No P5 handoff is required for this checkpoint.
