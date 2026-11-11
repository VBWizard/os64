# F.2b — binding ownership and cross-document adoption

Implementation [#539](https://github.com/VBWizard/os64/pull/539), on the merged
native foundation [#537](https://github.com/VBWizard/os64/pull/537). The accepted
scope is [IFRAME_F2_OWNERSHIP.md](IFRAME_F2_OWNERSHIP.md); native ownership costs
and proofs are in [IFRAME_F2_NATIVE_VALIDATION.md](IFRAME_F2_NATIVE_VALIDATION.md).
Implementation acceptance still requires code review before F.2's completion
flip. This record supersedes F.2b's initial realm-routing checkpoint.

## Implemented contract

An explicit DOM cluster admits independently parsed document/state pairs into
realms of one runtime. Its public admission result distinguishes a refusal
that preserves caller ownership from a consumed pair. Installation after
consumption runs inside a guarded realm task; failure drains and poisons the
whole cluster. Duplicate realms, foreign runtimes, external native borrowers
and aggregate-cap failures are rejected before ownership transfer. Runtime
classes are registered once, with prototypes and anchors installed per realm.
The legacy constructor keeps its borrowed, single-document contract.

A cluster identity cache returns the same node wrapper across members. Native
creation-realm metadata selects the prototype, including when the first wrap
occurs after several adoptions. Current-document metadata routes mutations,
control state, geometry, style, image and navigation services. Neither a
borrowed method's defining binding nor a wrapper's allocation owner selects
those services. Reentrant conversions re-resolve the operation's owner.

Explicit adoption and implicit insertion use the native state transaction.
Dirty controls, live ranges, cached collections, Attr/style views, Canvas
contexts and pixels survive moves. Imports create destination-realm objects.
Additional and inert Document roots remain arena-owned; unwrapped detached
nodes retain their true ownerDocument without a permanent snapshot pin.
Removing ordinary children still permits reclamation.

Custom registries remain per realm. An upgraded element retains its selected
definition; the cluster shares one reaction stack and backup queue, preserving
source disconnection, adoption and destination connection order. Nested
re-adoption and throwing callbacks are exercised. Mutation observers share
creation order and notification state, while each callback and MutationRecord
uses its observer's binding. Listeners retain their callback binding; event
paths use the target's current document. Timers, reporting, rejection taking,
rAF and idle entry use explicit libjs realm APIs and share the enclosing
runtime budget.

Native allocations keep their physical owner. DOM block headers now retain
that owner, so a sibling's release debits the correct account. Removing the
obsolete wrapper-local Document field offsets the larger block header for
ordinary wrappers; the existing 128-byte charged-wrapper bound still passes.
Aggregate HTML, page-state, DOM, kept-body and Canvas ceilings supplement local
limits. Harness root allowances are 64/16/8/256/128 MiB respectively; deliberate
small-cap tests prove refusal, charge recovery and continued sibling use.
No existing product limit was increased.

Web Animation effect backing stays with its allocation owner. Cluster-wide
generations and the oldest borrowing host protect old effect data after a
target moves; destination layout can find the effect. Hosts release layout
borrows before cluster free. This is a lifetime proof, not integrated frame
animation scheduling.

Teardown closes native delivery doors, drains member JS references, destroys
the shared runtime once, releases layout borrowers, then sweeps bindings and
native groups. Binding descriptors remain until sibling releases finish.
Individual drain/free is not a child-retirement API. Provider opaque storage
must survive cluster drain. Both libjs FATAL/finalizer and RECLAIM policies
are covered with zero surviving native fixture allocations.

## Corrections found during integration

Pinned WPT exposed two errors in the native fragment assumptions: template
contents can be adopted independently, whereas a ShadowRoot cannot; insertion
moves a fragment's children and preserves the fragment's own Document, even
when empty. Native insertion, source notifications and assertions now follow
that distinction. Four named WPT cases improve below.

Installing the native template-document association must not advance a tree's
rendering revision: it changes ownership metadata, not rendered tree, style
or control state. The unconditional revision bump invalidated Yonder models
built before DOM installation. Removing it restores the scripted-page suite.

A focused regression also covers native changes to a compiled event-handler
attribute after adoption: refreshing the slot must resolve its current
Document before deciding whether to compile. The test failed under both
teardown policies before that correction; ordinary compiled callbacks still
retain the realm where they were compiled. A second reproducer creates an
animation before admitting a sibling, then moves its target into that sibling
without another animation update. New bindings start with an invalidated effect
index so their first layout discovers effects published before admission.

## Review round 1

All four findings on `347f0665` were accepted. Filler options now allocate
in the select's current binding after numeric conversion, including retained
options collections and indexed gaps. Live class queries use their root's
current document mode while their storage stays charged to their creator.
Script ALREADY STARTED marks live with the node's immutable creation binding;
adoption and cross-document cloning consult that same identity.

Function listeners and handler properties resolve the callback's engine realm,
independently of the method binding and mutable prototypes. The hash-checked
QuickJS patch exposes its existing function-realm query; bound functions and
proxies follow their target. Script attribution is recorded in that binding too.
Compiled attribute handlers retain their existing creation-document behavior.

The focused matrix passes 280 checks in each engine profile, under both
teardown policies, with ASan/UBSan and leak detection enabled. It covers
reentrant select adoption, quirks/standards moves, started and inert script
adoption/import, ordinary and named handlers, bound/proxy callbacks, changed
function prototypes and exception delivery to the callback's window. Five
review mutants compile and are caught by those assertions, including both
option filler loops. The validation table below records the original F2b
campaign; these results are the review delta.

The branch also merges userland through `c1150130`, including Web Animations C
(#541). Its animation-member conflict preserves both the animation owner's
binding and the new CSS play-state override. The integrated shipping-engine
DOM suite passes 43,781 checks and 3,879 allocation sweep points. Instrumented
checks pass: events 1,020; reporting 4,523; cloning 999; Web Animations 292;
and admission failures 16,778 checks over 1,048 cuts per teardown policy.
The final realm probe adds host-dispatched callback coverage after the full
shipping run; it passes the 280 checks above in both profiles. No implementation
changed between those runs. The libjs realm suite passes 1,152 checks, engine
preparation passes six tests, and `make -C userland dom-realms-test` links the
updated target consumer and libraries. The original frozen QEMU cohort and
its results below remain evidence for the earlier implementation; this review
delta does not claim another guest run.

## Review round 2

Both findings on `699702d2` were reproduced and accepted. Window `onerror`
uses the target entry's binding to select its five-argument calling convention
and true-to-cancel rule; execution, current event and exception attribution
remain in the function callback's realm. The regression covers synthetic and
host-delivered errors, both boolean return values, and a Document handler that
must retain ordinary one-argument/false-to-cancel semantics.

`getAnimations` resolves the receiver after reading its options, searches the
cluster's animation lists in global creation order, and limits Document queries
to connected targets whose current document is that receiver. Borrowed Element
and Document methods, subtree queries, detached elements and adoption during
option access preserve animation identity. Queries and layout share the same
ordered traversal. Query iteration pins queued records across result-array
allocation and releases them on refusal as well as success.

Leak-enabled checks pass in both engine profiles: realm matrices 290 each;
events 1,020 each; reporting 1,089 shipping / 4,523 instrumented; Web Animations
303 shipping / 424 instrumented. The instrumented animation query fails each
of its 11 allocation points in turn and tears down cleanly. Four new mutants
compile and are caught: callback-window error semantics, binding-local animation
queries, missing current-document filtering and reversed composite order.
`make -C userland dom-realms-test` links the updated target libraries/consumer.
These are focused review-delta checks; the broader campaign and frozen guest
results below retain their earlier source qualifications.

## Review round 3

All three findings on `b91c5e74` were reproduced under both teardown policies.
The shared base-URI helper resolves the node's current binding before selecting
its URL and document, including retained and detached Attr callers. The
rendered-target check for `commitStyles` uses the target's current binding;
animation ownership is unchanged. Handler properties choose their target from
the receiver's current document before resolving the callback's own realm.
The attribute-handler entry point applies the same ownership rule so a handler
slot takes its place when an adopted body's attribute is set.

Regressions cover destination URLs and relative bases, borrowed baseURI getters,
detached Attrs and windowless documents; style commits across B/C providers,
including detached/display:none/display:contents refusals; and adopted body and
frameset properties, callback realm, windowless refusal, and handler order when value conversion adopts a body before borrowed listener
registration.

Both final realm matrices pass 318 checks. The shipping DOM suite passes 43,834
checks and 3,879 allocation sweep points. Instrumented suites pass: events
1,020; reporting 4,523; Web Animations 424 (including 11 query-allocation cuts);
and attributes 3,426. ASan/UBSan and LeakSanitizer remain enabled. The final
focused rerun tightens the handler-attribute test with reentrant adoption and
borrowed listener registration; no implementation changed after the full shipping
run. Four review mutants
compile and are caught, including the sibling handler-attribute entry point.
The updated target consumer and libraries link. No new guest/hardware run is
claimed; the earlier campaign's source qualifications still apply below.

## Review round 4

All three findings on `61ebe6fd` were reproduced under both teardown policies.
Canvas fill/stroke getters search the cluster's retained registries for the
native gradient's wrapper. Strict identity survives cross-realm assignment,
borrowed accessors, save/restore and adoption. This preserves the existing
retention model; the linear lookup performance debt remains open.

Click-in-progress flags use the node's immutable creation binding, so adoption
inside a listener cannot bypass the active flag. The sibling form-submission
flag follows the same rule in script submission, host submit delivery and host
constraint validation. Tests cover A-to-B-to-C moves, borrowed methods, another
element's nested click, already-adopted forms entering through the host, and
subsequent calls after the guards clear. Document services still use the
current binding.

EventTarget methods select the receiver's binding before classifying window
versus plain targets, and refresh node ownership after listener argument
conversion. Attribute lookup, registration order and compilation use the
target's binding; function callbacks retain their own realm and object
listeners retain their registration binding. A native attribute replacing an
older foreign callback also enters the target realm before compilation. Tests
cover untouched body attributes, existing window slots, borrowed dispatch,
add/remove methods and handler getters, callback current-event state and
attribute-before-listener order. The old slot-refresh comment was incomplete:
it accounted for node adoption but omitted window handlers and foreign contexts.

Both final realm matrices pass 350 checks. The complete shipping-engine DOM
suite passes 43,866 checks and 3,879 allocation sweep points. Instrumented
suites pass: events 1,020; reporting 4,523; Canvas 548; window/activation 820.
ASan, UBSan and LeakSanitizer remain enabled. Eight focused mutants compile and
fail the intended assertions: gradient lookup, click flag, the three submission
entry paths, borrowed dispatch, window registration order and handler
compilation context. The target consumer and libraries link. No new guest or
P5 run is claimed; the earlier campaign's source qualifications remain below.

## Review round 5

All three findings on `65b6faf5` were reproduced under both teardown policies.
They expose remaining single-binding assumptions in current-document services,
post-listener activation and animation replacement.

Element scroll methods and position setters resolve the current binding after
argument conversion. Regressions cover scroll/scrollTo/scrollBy, both position
setters, borrowed methods, adoption in dictionary getters and numeric conversion,
read/write agreement at destination providers, and a windowless target.

Post-click activation resolves the target again after listeners. The sibling
path audit also found and fixed post-submit navigation and post-reset host
notification: their listeners can adopt the form too. A submit listener that
disconnects its form cannot produce a navigation request. Tests cover link and
button adoption, script-dispatched MouseEvent activation, cancellation,
windowless refusal, form adoption during submit/reset and disconnected refusal.
The creation-owned reentry guards remain intact.

Automatic animation replacement merges the cluster's creation-ordered lists
using the same serial order as discovery and layout. Eligibility checks the
target's current window document while animation timelines and event queues
remain with their creation bindings. The candidate snapshot retains JS values
across removal and event queuing and rebuilds traversal heads after allocation.
The eligibility comment now distinguishes target connectivity from timeline
ownership. Tests cover C/A/B creation order, property coverage split across two
later effects, removal-event realm, persisted and detached exclusions, and
layout/discovery dropping the removed effect. Remaining list walks were checked:
CSS-host identity, timeline updates, hold release and teardown are local-owner
operations; queries, layout, effect-owner publication and replacement traverse
the cluster where needed.

Both final realm matrices pass 476 checks. The complete shipping-engine DOM
suite passes 44,043 checks and 3,883 allocation sweep points. Instrumented
suites pass: scrolling 305; window/activation 820; events 1,020; Web Animations
675. Animation replacement's update step fails each of 4 shipping / 24
instrumented allocation points in turn, with clean teardown; the earlier
11 instrumented animation-query cuts still pass. Eight review mutants compile
and fail assertions, covering scroll ownership, post-click activation,
post-submit ownership/connectivity, reset notification, replacement scope,
eligibility and composite order. ASan/UBSan and LeakSanitizer remain enabled.
The target consumer and libraries link. No new guest/P5 run is claimed; the
earlier campaign's source qualifications remain below.

## Review round 6

Both findings on `e5adab49` were reproduced under both teardown policies.
Pointer capture selects the element's current binding after converting the
pointer ID. Tests use native pointer packets in B/C with an A-created element,
verify got/lost capture and callback current-event state, borrow capture methods,
and adopt during each capture method's ID conversion. Unknown-pointer and
unconnected-element refusals remain covered.

Attr cloning selects its destination document binding; the shared Attr
allocator selects that binding's realm before creating its record and wrapper.
The related allocation audit also corrects borrowed createAttribute/NS factories
and first-created cached views: attached Attr and NamedNodeMap views use the
element's creation binding, while an Attr's empty child list uses the Attr's
own creation binding. Existing cached objects retain identity. Tests cover
import, detached/attached clone after adoption, namespace/name/value preservation,
windowless destinations, borrowed factories and borrowed cached-view accessors.
The allocator comment no longer incorrectly promises QuotaExceededError for
engine allocation failure or an unavailable realm.

Both final realm matrices pass 498 checks. The complete shipping-engine DOM
suite passes 44,065 checks and 3,883 allocation sweep points. Instrumented
suites pass: pointer 603; attributes 3,426; document operations 2,968. Seven
mutants compile and fail assertions, separately covering capture ownership,
clone ownership, Attr allocation context, factory ownership, attached Attr
views, NamedNodeMap views and Attr child lists. ASan/UBSan and LeakSanitizer
remain enabled. The target consumer and libraries link. No new guest/P5 run
is claimed; earlier campaign source qualifications remain below.

## Validation

Host checks use ASan/UBSan and LeakSanitizer with `detect_leaks=1`, outside the
sandbox for thread attachment. Retained host objects are rebuilt for changed
units; both shipping and instrumented engine profiles link the same native
fixture. Logs are streamed to bounded files and read in bounded excerpts.
No log is loaded wholesale for validation.

The full instrumented sweep precedes the final handler-refresh and late-admission
effect-index corrections. After those corrections the instrumented realm/cap
probe passes 258 checks, the event probe passes 1,020, and Web Animations pass
278; admission allocation cuts are also rerun. The full shipping-engine run
includes both corrections. The retained full-sweep evidence is not relabeled
as an exact final-source instrumented run.

| Check | Result |
|---|---|
| Three-realm DOM and small-cap probes, both engine profiles | 258 checks per engine profile; zero failures |
| Complete DOM, shipping engine | 43,749 checks; zero failures; 3,879 allocation sweep points |
| Complete DOM, instrumented engine | 308,981 checks; zero failures; 36,607 sweep points; final-delta qualification above |
| Second-binding installation allocation failures | 1,048 cuts per teardown policy; 16,778 checks; zero failures |
| Native HTML ownership/DOM | 800,487 checks; zero failures |
| HTML parser/reference suite | 1,277 native checks; 10,531/10,531 reference cases; 376 roundtrips |
| HTML fragment fixture | 333 checks; zero failures |
| HTML reclamation | 310,833 checks; zero failures; 216,000 churn steps flat at 39,279 bytes / 104 nodes |
| HTML parser fuzz | 113,920 mutations; four chunkings each; 30 seconds |
| Page state | 254,708 checks; zero failures; 415 allocation cuts and lifetime-fatal proof |
| libjs focused realms/runtime | 1,152 checks; zero failures |
| Yonder scripted pages | 24,831 checks; zero failures; 114 completed refusal cases |
| Yonder painting regression | 2,255 checks; zero failures; six corpus paints match |
| Binding ownership mutants | Passing baseline; 8/8 compiled and caught |
| Existing mutation probes affected by the refactor | 11/11 compiled and caught against focused passing baselines |
| Native ownership mutants | Passing HTML/page baselines; 9/9 compiled and caught |
| QEMU DOM consumer | Three actual realms, both teardown policies; parent observes exit `0x446f0100` |

Focused entry points are `tools/test_dom_host.sh --realms` and
`tools/test_dom_host.sh --cluster-failures`; omitting the selector runs the
complete matrix. Retain objects with `DOM_HOST_KEEP=1`, then run
`python3 tools/test_dom_mutants.py <objects> --realms` for the binding
mutants. The native campaign remains `tools/test_iframe_ownership_mutants.py`.
`make -C userland dom-realms-test` builds the guest consumer. The four pinned
WPT groups use `tools/test_yonder_wpt_host.sh <group>` against the pinned
checkout; neither `--update` nor expectation edits are part of this evidence.

The shared host/guest oracle creates a container in A, adopts it into B,
creates descendants there, then adopts into C before first wrapping them.
It proves A's container prototype, B's descendant prototypes, C's
ownerDocument and preserved native/JS identity. The host extends this with
distinct providers (geometry widths 100/101/102), shadow/template/XML creation,
borrowed methods, controls, listeners, observer order, custom definitions,
reentrant adoption, Canvas damage and retained effect data.

The mutant campaign breaks creation-realm selection, receiver routing, shared
wrapper lookup, physical release accounting, preserved custom definitions,
child task/report routing and aggregate DOM limits. Native mutants separately
break document migration, physical node ownership, ranges/templates, sibling
pin protection, state migration and retained snapshot storage. A compilation
failure, timeout or arbitrary exit is not a successful catch.

QEMU uses a frozen SHA-256-manifested cohort of 19 consumer/library files
(5,709,296 bytes), an existing kernel and a disposable disk. The consumer
executes the shared adoption oracle under both teardown policies and verifies
its heap; the parent checks `os64_wait`'s actual status. The network-disabled
boot also reports the unrelated `net_arp_pending_order` test failure. The
acceptance claim is for the DOM consumer, not a wholly passing kernel suite.

The native corpus capacities match F.2a's measured table. The long churn
plateau remains unchanged. F.2a's stalled-pin and old-model recovery costs
remain applicable: HTML B 5,616 → 513,972 → 5,616 bytes, aggregate peak 529,764;
page state 1,909 → 55,586 → 6,000 bytes, peak 63,922. F.8 must budget these
retention costs alongside its integrated rendering accounts.

## Pinned named-WPT comparison

Checkout `a699fb16767be39ba3e61a8e8b6bef60383f08ee`; baseline `03a18579` in an
isolated worktree. Named `(page, test)` verdicts and REQUIRED/LOST sets were
compared, with no expectation or exclusion edits. Both sides have empty
REQUIRED/LOST sets in all four groups.

| Group | Baseline pass/fail/timeout/notrun/precondition | F.2b | Pages |
|---|---|---|---:|
| `dom/nodes` | 7902 / 1279 / 82 / 676 / 6 | 7905 / 1279 / 79 / 676 / 6 | 361 |
| `dom/ranges` | 37377 / 274 / 1 / 4176 / 0 | unchanged | 69 |
| `dom/events` | 389 / 154 / 10 / 11 / 0 | 389 / 156 / 8 / 11 / 0 | 90 |
| `custom-elements` | 196 / 1028 / 34 / 250 / 2 | 197 / 1027 / 34 / 250 / 2 | 189 |

Named FAIL → PASS changes:

- `dom/nodes/adoption.window.html`: `appendChild() and DocumentFragment with host`.
- The same page: `adoptNode() and DocumentFragment with host`.
- `dom/nodes/Document-adoptNode-DocumentFragment-with-host.window.html`:
  `adoptNode() <template> element: content follows via template adopting steps`.
- `custom-elements/registries/upgrade.html`:
  `Elements inside a template contents DocumentFragment node`.

Five other named verdicts change TIMEOUT → FAIL, with no lost passing test:
`moveBefore/continue-css-animation-left.html`,
`moveBefore/continue-css-animation-transform.html`,
`moveBefore/css-animation-commit-styles.html`, and the `Listener of animationend`
and `Listener of animationstart` cases in `EventListener-invoke-legacy.html`.
The three moveBefore cases now reach the missing-method TypeError. The two
animation listeners pass their subtests but the runner downgrades them because
the enclosing page still times out. These remain failures, not new conformance
claims. Range verdicts are identical.

Exclusions are unchanged: nodes omit 47 XML pages; events omit 115 expectation
exclusions and 10 worker/shadow-realm variants; custom elements omit seven XML
pages. The brief's iframe-dependent realm/adoption assertions are adapted in
the native host/guest setup, not reported as unmodified iframe WPT passes.
Frame-removal tests still require F.3 departure behavior.

## Review boundary

Yonder retains its single-binding path. F.2b exposes no frame loading,
WindowProxy, same-origin gate, independent realm retirement, child-only
installation rollback or integrated frame scheduler. Shared-runtime membership
is a native lifetime/brand check, not origin authorization. Raw object exchange
is confined to trusted fixture setup. F.3 and F.4 remain prerequisites for
host frame integration. No P5 acceptance is required for this foundation.
