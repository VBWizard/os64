# PE1 — Mouse-backed pointer events and capture

Implemented; P5 gallery accepted 2026-11-01. Base: Canvas reconciliation `c1f354a4` (PR #447),
stacked on `canvas-integration`. PE1 follows Modern Web 5d and precedes
Canvas C6. It supplies the input path for drawing and scratching; C6 owns
end-to-end acceptance of the captured Danlegt applications.

## Outcome and scope

A page can receive real pointer input, capture a drag to an element, and
finish or cancel it reliably. Existing links, forms, text selection and
mouse listeners continue to work. Modern Web enables the constructor,
handler properties, capture methods and native dispatch together. With
Modern Web disabled, keep the existing mouse profile.

The first hardware profile is Os64's combined mouse stream. USB, PS/2 and
remote input already feed that stream; PE1 does not invent separate physical
pointer identities from it. Use `pointerId = 1` for the browser window
lifetime, across document navigation; availability and capture remain
document-local. Track pointer availability separately from held
buttons: hovering is not the same as a button-active gesture.

Pen/touch hardware, gesture arbitration, pointer lock, raw updates and motion
prediction are outside PE1. Do not advertise working raw/predicted input.
PE1 does not expose `getCoalescedEvents` or `getPredictedEvents`;
constructor compatibility and trusted hardware support are separate claims. `touch-action` does not control this mouse-only stream.

Normative reference: [Pointer Events Level 3](https://www.w3.org/TR/pointerevents3/),
especially sections 4.1–4.2, 9 and 11. Use its constructor defaults, button
transition rules, capture algorithms, event flags and compatibility mapping
as test oracles rather than copying mouse events under different names.

## Evidence from the current tree

| Existing path | Consequence for PE1 |
| --- | --- |
| `kernel/src/gui/compositor.c`, implicit pointer grab | The pressed window receives movement and releases outside its content. DOM capture needs no new kernel syscall. Verify this route in the guest and on hardware. |
| `userland/libos64/ui.c`, `os64/ui.h` | The toolkit is a third router: left-button grab, right-button context callback, no middle/extra-button delivery. `cancel_widget` ends interaction on focus/visibility/loss/resize and widget invalidation. PE1 uses the browser ingress below, with explicit cancellation feedback. |
| `abi/include/os64/gui.h` | Ordered mouse records carry button masks, changed button, modifiers and content-local coordinates. `POINTER_STATE` is a coalesced hover snapshot, not a button or drag record. `INPUT_LOST` cancels incomplete input. |
| `userland/apps/yonder/events.h` | The bounded native batch already preserves drag samples and merges idle movement. Keep its scheduling bound. |
| `yonder.c`, `input_queue`, `inputs_run`, `mouse_event`, `hover` | Queued input holds nodes, but its mouse payload models a primary-button boolean and calculates CSS coordinates later. Its move merging can cross intervening queue entries. These assumptions must change for pointer input. |
| `libdom/event.c`, `internal.h`, `include/dom/dom.h` | Extend the existing event record, MouseEvent inheritance, host dispatch and handler registry. PointerEvent is deliberately absent because exposing it changes site feature detection. |
| `libdom/mutation.c` | Capture lifetime joins the existing mutation dispatcher; it must not install another document listener. |

The saved target inventory is linked from [CANVAS.md](CANVAS.md). Its signature
editor captures on down and ends drawing on up/cancel/leave. The shared
scratch handler captures its container and does its own per-cell hit testing.
An erroneous leave during capture therefore stops a legitimate stroke; a
missed up leaves scratching active. Tests must reproduce both arrangements.

## Ownership and dispatch boundary

Put pointer identity, button state, pending/current capture and compatibility
suppression in a small document-owned controller in libdom (`pointer.c`).
Yonder owns native input acquisition, hit testing, task scheduling and browser
default actions. `os64_dom_pointer_dispatch` in `dom/pointer.h` accepts a
normalized sample and a borrowed physical hit target, with a callback for
native control defaults. The controller holds targets across listener calls.
Shared libdom activation handles clicks and records navigation requests;
Yonder takes those requests after the packet. The optional `ran` result
separates script tasks from quiet hover updates.

Do not make JavaScript's synthetic `dispatchEvent` a source of physical
pointer state. Synthetic PointerEvents use normal dispatch; their chosen IDs
cannot activate a pointer or take over a native drag.

The browser ingress records a document generation, sample coordinates, button
mask, changed button and modifiers. Convert content-local positions using
the viewport origin, scroll position and zoom captured with the sample;
widen the coordinate members of `os64_dom_event_t` to `double` in PE1
(client/page/screen x and y), and preserve fractional values through host
construction and property getters. Widen the legacy queued CSS snapshot as well;
keep native GUI positions and hit-test arithmetic integral. Update the C
callers, layouts of shared records, and format strings together; distribute
matching Yonder/libdom libraries. No kernel event ABI change is needed.
Audit true screen coordinates separately from content-local GUI coordinates:
include the window's placement, rather than relabeling content coordinates.
MouseEvent also receives doubles, so this is a deliberate shared-interface
change, not a fractional PointerEvent feeding an integer mouse adapter.
Do not reconstruct `buttons` from a subsequently changed pressed node.
Keep the physical hit and related nodes alive through packet delivery.

Capture is resolved at delivery, after the preceding callback can change it.
A down listener can capture before the next queued move/up is dispatched.
Dispatch a sample's pointer events and permitted mouse events within a
single browser input task, then use the existing rendering opportunity.
Do not call today's complete `page_event` task once per generated event:
that would permit layout/navigation between halves of one input operation.
Revalidate document generation and target connectivity after callbacks;
navigation stops delivery to the replaced document.

### Browser ingress and native defaults

Choose a browser-owned page-input route before libui's mouse routing, rather
than an all-buttons widget flag. Form widgets are siblings of `g.view` under
`g.root`, so a flag on the view would still miss page controls. The ingress
classifies page content, native chrome, popup/modal ownership and an existing
page gesture explicitly. It handles page samples over form widgets too, and
continues a page-owned gesture outside the viewport. Native chrome and
non-page gestures keep their existing libui route. Modern Web off uses the
old route unchanged.

The route dispatches one synchronous normalized packet outside libui. Pointer
and permitted compatibility events run before their corresponding native
defaults. Invoke libui for an allowed native control default at that phase,
with a scoped flag preventing the old `view_event`/widget callback path from
also emitting the DOM mouse/click event. Widget activation callbacks are
suppressed during replay; checkbox widgets are restored from the model.
They cannot dispatch script while libui is on the stack. The packet emits
one DOM click and uses shared libdom preactivation, cancellation rollback
and activation for controls and links.
Resolve the widget/control again by node after script; do not replay to a
saved widget address freed by a rebuild. Native focus/selection defaults
must respect a suppressed mousedown. Navigation or document-generation
change ends the packet before another default or generated event is run.

The browser route observes all three GUI buttons, including extra presses
and releases during an existing grab. The controller determines DOM pointer
transitions from the complete button mask; native widget replay still uses
that widget's supported buttons. Do not feed middle/right presses to a
left-only checkbox or button as synthetic primary presses.

Add an optional libui cancellation notification for an actually owned native
gesture, issued by `cancel_widget` before its owner is discarded. The browser
listener sets cancellation owed; it runs no script. This covers toolkit
cancellation caused by widget invalidation as well as explicit host events.
Yonder also feeds focus loss, window coverage, resize, input loss and modal
opening through the same idempotent cancellation door, including page
captures with no toolkit grab. Initial focus movement before a page gesture
exists must not cancel the new gesture. Duplicate notifications produce one
cancel. `os64_ui_on_gesture_cancel` stores the observer in the toolkit's
private per-window binding, released by `os64_ui_font_release`; it does not
enlarge the public `os64_ui_t` record allocated by existing applications.
Other applications retain their existing routing and cancel behavior.

### Button and menu policy

| Physical button | Page input and activation | Browser default |
| --- | --- | --- |
| Primary | Pointer transitions, permitted mouse events, and a normal click if the sequence remains eligible. | Existing link/control/selection behavior, using the resolved activation target. |
| Middle | Pointer transitions, permitted mouse events and `auxclick` on eligible release, with DOM button 1. | No new middle-click navigation/tab action in PE1. |
| Secondary, no menu eligible | Pointer transitions, permitted mouse events and `auxclick` on eligible release, with DOM button 2. | No popup, matching today's menu eligibility. |
| Secondary, window menu eligible | Deliver the press packet, then cancel the live page stream and open the menu; consume its abandoned tail. | Preserve the existing window-owned menu. No page `contextmenu` and no page veto of this menu. |

Snapshot the eligible menu action's addresses at ingress, before callbacks,
as the existing menu snapshots addresses rather than retaining live nodes.
Defer menu opening until the press/cancel sequence has run outside libui.
If navigation occurs meanwhile, discard that page's pending menu action.
A secondary press during another held button is a chord transition, not a
second pointerdown. If it opens a menu, it cancels the existing stream.
Keyboard menu invocation uses the same interruption door if a drag exists.
This window-owned menu is an explicit product exception to the web
`contextmenu` interface; it is not a PE3 conformance claim. A window-directed
compatibility mouseup on cancel is not a normal page release or auxclick.

## State, capture and compatibility

The controller stores the last sample, physical/effective boundary targets,
button state, gesture down target, capture target used for release, and
pending/current capture references. Capture methods consult this controller;
`hasPointerCapture` observes pending state. Process capture transitions before
the next applicable pointer event. Up/cancel releases capture and reconciles
boundaries. Preserve release-time targeting for the later activation event.

Use the specification's errors for unknown IDs and disconnected elements;
a known hovering mouse with no pressed button does not start capture. Native
mouse capture is explicit at the DOM level, even though the window system
implicitly grabs the containing window. Repeated set/release calls must not
produce spurious transition events. Changes made from capture listeners are
handled by the defined pending-state algorithm, without recursive draining.

Node references are held against their document. Removal of a capture target
marks the transition through the shared C mutation consumer; dispatch at the
next safe event boundary, using document fallback where required. Mere DOM
ownership is insufficient to prove connectivity. Do not run script from the
mutation callback. Navigation/runtime close releases state without calling
into a dying runtime. Include pending targets and saved activation targets
in teardown and allocation-failure coverage.

Button transitions use the full native mask with explicit GUI-to-DOM mapping.
An additional button pressed during a drag is not another pointerdown.
Pointer cancellation of compatibility mouse events must not accidentally
cancel click activation; that has its own event/default-action contract.
Hover boundary events remain distinct from cancellable button events.
Update the existing click path's event interface and pointer provenance,
including keyboard activation, in the same slice.

For uncaptured activation, replace `at == g.pressed_node` with the nearest
common inclusive ancestor of the down/up targets, evaluated against the
current DOM at click dispatch. If release was captured, retain its target
for activation even after lost capture. Revalidate connected targets after
callbacks; do not let a removed target or new document inherit an activation.
Derive a link's default from the activation target's enclosing link, not
`link_at` at release or the old `in.related` value. Native controls must also
match this activation target before committing their callback. Regression:
down on one nested span in a link, up on a sibling span, clicks the link;
release over an unrelated link does not navigate that unrelated link.

Build boundary events from old/new ancestor paths and `relatedTarget`, not
just two leaf notifications. A capture overrides event targeting, not the
stored physical cursor location. On release, reconcile to that physical
location.

### Sequencing oracles

Use these alongside the normative event-flag and button tables; they are
observable tests, not an invitation to dispatch each event as a new task.

1. Pending capture is processed before the next applicable pointer event;
   transfer reports lost on the old target before got on the new one.
2. Implicit release follows up/cancel. Explicit release or transfer can
   report lost before a later move, without waiting for up. Preserve the
   capture-used-at-release snapshot for click/auxclick target selection.
3. Reconcile released capture with the physical cursor. Cancellation also
   ends boundary presence; recovering hover is not a new pressed gesture.
4. Canceling pointerdown suppresses subsequent compatibility down/move/up
   for that gesture, not click or the boundary mouse events. Unsuppressed
   pointercancel has the specification's window-targeted compatibility
   mouseup, which must not enter the normal activation path.
5. A canceled stream emits no pointerup and cannot generate click/auxclick;
   its later physical releases are consumed until neutral.

### Stationary boundary scheduling

Keep this in PE1: the new event interface is page-wide, not limited to the
captured Canvas targets. The obligation and permitted delay are in
[PE3 §4.1.4](https://www.w3.org/TR/pointerevents3/#boundary-events-caused-by-layout-changes).

Yonder owns `rehit_owed`, the document generation, a last published hit-test
revision and `next_rehit_ms`. Publishing layout, CSS hit-test/transform
changes, scroll or zoom sets owed when a physical cursor location is known.
An ordinary sample that hit-tests that same published revision can discharge
it. Presence snapshots update the physical-location validity separately.
Keep the original screen/content sample: recompute CSS positions against the
new viewport for this boundary-only operation, rather than reusing stale
page coordinates from a real motion event.

At the next eligible 5d rendering opportunity, publish pending geometry,
clear owed before delivery, and perform at most one stationary hit-test and
boundary dispatch for that document. This is a post-layout input task, not
a callback from the painter. It emits no synthetic pointermove/mousemove.
Capture keeps its effective target while physical hit state is refreshed.
Callback edits may set owed again; they cannot cause another re-hit in the
same opportunity. Fold `next_rehit_ms` into the existing ticker deadline with
a minimum 16 ms interval. This work wakes the ticker even if there are no
rAF callbacks, and uses the existing covered-window suspension policy.
Uncover schedules one reconciliation. Navigation/close clears the debt.

Thus a boundary listener that moves its own target can cause another
bounded browser turn, but cannot form an immediate layout/event recursion.
Use the normal script budget; no unbounded ancestor event queue is built.
Existing scroll/zoom calls to `hover()` become marks/physical updates into
this same path under Modern Web, preventing duplicate boundary dispatch.
The host test must drive the production publication hook and ticker, with
a listener that moves a target every time; assert at most one re-hit per
eligible opportunity and progress of other queued input.

## Loss, batching and bounded work

A press originated in page content keeps delivering captured samples even
outside the viewport, where ordinary `element_hit` returns NULL. Native
window-grab routing carries the drag outside the window. Toolbar interaction
or a modal must not steal a live page gesture silently; end it through the
same cancellation path when host policy interrupts it.

`INPUT_LOST` and a host interruption that abandons the gesture clear queued
stale samples, dispatch cancellation while the document is live, release
capture and clear mouse suppression. A lost queue must not fabricate a
successful up/click. A passive pointer-presence snapshot only repairs hover;
it cannot resurrect a cancelled drag or infer an unobserved press. After
loss, suppress the abandoned gesture until button state reaches neutral,
then accept a fresh press. The tail is expected: `event_queue.c` compacts
lost queued gestures, but the compositor's `s_pointer_window` grab survives
until the physical final release. Do not change the kernel to hide that up;
the browser's drain-until-neutral state is what prevents phantom activation.

Retain bounded native event batches. Queue **samples**, not their expanded
DOM events. `window_event` drains `inputs_run` after each native event today;
it does not accumulate 32 native records and then expand them into the DOM
queue. Preserve that drain boundary in PE1. A normalized sample consumes one
packet slot; its ancestor boundary walks and generated events are dispatched
directly within the packet's task and use the normal script budget.

The call stack is the ingress slot: `window_event` completes its pointer
packet and deferred control work before the next native record is polled.
There is no pointer DOM-event queue to overflow as boundary depth grows.
The native batch's look-ahead record remains separate. Boundary paths hold
nodes and emit directly under the packet's task budget. A 32-record pressed
sequence must reach its release without artificial cancellation.

An impossible internal overflow or actual native loss requests cancellation
through reserved state rather than appending to a full queue. No allocation
is required to remember it. Construction failure clears native ownership and
prevents activation even if JavaScript cannot receive the cancellation event.

PE1 does not merge pressed movement at the DOM boundary. Idle motion uses
the native batch's adjacent-sample coalescing; the pointer route bypasses the
legacy DOM queue's whole-queue move search. The native fairness limit stays
in place. A hover with no relevant listeners updates native state and its
widget default without opening a script task.

## Lifecycle audit

Tables describe native/controller state; event objects use the existing DOM
lifetime. Held node assignments acquire the replacement before releasing the
previous hold. `Keep` means unchanged. A successful explicit API call updates
pending state without running script. Invalid API calls leave state unchanged.
The current/pending capture processing algorithm revalidates after listeners;
it is not implemented by recursively replaying this table.

`End` means: set cancel owed before entering callbacks, prevent activation,
deliver one cancellation sequence at the next safe dispatch boundary if the
runtime is alive, then release the gesture's holds. If already canceling,
merge the reason and do not recurse. Record the last packet for that sequence
before clearing live state. `Silent clear` releases holds without script.
`Drain` consumes the abandoned native tail until an observed zero-button
sample, and remains separate from compatibility-mouse suppression.

Down and release targets include bounded per-button activation candidates
for the three supported buttons; chord releases cannot borrow a different
button's down target. The ID is always 1 for this window, but it is not an
active pointer in a replacement document until that document receives input.

### Ordinary transitions

| State | Native sample | Capture set | Explicit release | Target removed | Geometry published |
| --- | --- | --- | --- | --- | --- |
| Last sample / availability | Snapshot normalized fields; availability follows real input/presence, not synthetic dispatch. | Keep. | Keep. | Keep. | Preserve physical sample; derive fresh boundary coordinates when due. |
| Physical/effective/legacy boundary targets | Re-hit physical position; reconcile effective and legacy paths under capture. | Keep until pending processing. | Reconcile physical position when pending processing releases capture. | Hold old path information until safe boundary reconciliation; mark re-hit owed. | Mark re-hit owed; do not dispatch from layout. |
| Buttons / drain-until-neutral | Update mask; while draining consume tail, clearing drain at neutral without activation. | Require an active pointer and pressed-button state. | Keep. | Keep. | Keep. |
| Down targets | Hold per-button eligible press targets; retire on release/cancel. | Keep. | Keep. | Invalidate disconnected activation candidates and release their holds. | Keep, with connectivity checked before activation. |
| Release/activation target | Snapshot delivered release target and whether captured; consume once for activation. | Keep any already selected release target. | Keep the release snapshot despite capture loss. | Invalidate disconnected candidate; do not activate its replacement. | Keep, with current-DOM targeting rules at activation. |
| Pending capture | Process before applicable input; clear after up/cancel. | Replace with the connected requested node. | Clear only if this element has pending capture. | Clear disconnected pending node. | Keep. |
| Current capture | Process pending transitions; clear on implicit release. | Keep until pending processing. | Keep until pending processing. | Replace disconnected current target with held document fallback; owe lost capture. | Keep effective capture despite changed physical hit. |
| Compatibility suppression | Set from canceled pointerdown; reset after up/cancel mapping. | Keep. | Keep. | Keep; removal alone is not cancellation. | Keep. |
| Cancel owed | Service before later packets; do not dispatch abandoned tail. | Do not permit rearming while End/drain is in progress. | Keep; clearing capture cannot retract cancellation. | Keep unless host also abandons the stream. | Keep. |
| Re-hit owed / revision / deadline | Clear only for a hit-test of the current published revision. | Preserve owed work; capture affects effective targeting. | Reconcile from current physical hit or keep owed if stale. | Set owed for the live document. | Set owed/revision; share the ticker with a 16 ms minimum interval. |

### Ending and failure transitions

Navigation and runtime close have separate columns deliberately: neither may
call a dying document, while loss and host cancellation may notify a live one.
Overflow here means an invariant violation or native loss, not a full queue
that can be drained through admission back-pressure.

| State | Navigation | Runtime close | INPUT_LOST | Host cancel | Internal overflow | Allocation failure |
| --- | --- | --- | --- | --- | --- | --- |
| Last sample / availability | Silent clear; new document starts unavailable; host preserves drain state for held buttons. | Silent clear. | Save cancel snapshot; require fresh state before rearming. | Save cancel snapshot; retain physical location only if known. | Follow INPUT_LOST. | Failed event construction uses last valid snapshot for End if possible. |
| Boundary targets | Release all old-document holds. | Release all holds. | End, then clear paths. | End, then clear paths. | End, then clear paths. | Release partial paths; End on dispatch construction failure. |
| Buttons / drain | Clear document buttons; host drains an in-flight old gesture. | Clear document buttons; host drains if window survives. | Clear logical buttons and enter drain even if the final mask is unknown. | Clear logical buttons and drain any abandoned held gesture. | Follow INPUT_LOST. | Dispatch failure ends gesture and enters drain; refused API allocation preserves state. |
| Down targets | Release. | Release. | End and release. | End and release. | End and release. | Dispatch failure invalidates/release candidates; API refusal keeps them. |
| Release/activation target | Release; forbid cross-document activation. | Release. | Invalidate and release; no click. | Invalidate and release; no click. | Invalidate and release; no click. | Refused event/default preparation cannot commit activation. |
| Pending capture | Release. | Release. | End and release. | End and release. | End and release. | Capture API refusal preserves prior pending hold; failed event delivery ends and releases it. |
| Current capture | Release silently. | Release silently. | End then release, with lost capture if dispatch can succeed. | End then release, with lost capture if dispatch can succeed. | Follow INPUT_LOST. | Failed cancellation allocation still releases native holds; no recursive retry. |
| Compatibility suppression | Clear with old controller. | Clear. | Apply cancel mapping if possible, then clear; drain remains set. | Apply cancel mapping if possible, then clear; drain remains set. | Follow INPUT_LOST. | Clear after failed End; do not undo drain. |
| Cancel owed | Clear old debt after silent release; never deliver it to new document. | Clear after silent release. | Set reserved flag; duplicates coalesce. | Set reserved flag only for a live gesture; duplicates coalesce. | Set reserved flag without allocating. | Set for failed native dispatch; clear after End even if notification allocation fails. |
| Re-hit owed / revision / deadline | Clear old generation/deadline. | Clear. | Discard stale re-hit; fresh presence may schedule hover recovery. | Clear pending work; valid presence/uncover may schedule recovery. | Follow INPUT_LOST. | Retain one bounded re-hit retry for recoverable idle failure; failed active dispatch goes through End. |

A boundary-only task clears owed before callbacks; changes it causes can set
owed for a later eligible opportunity. A failed native packet cannot be
retried as a second press. An ordinary failed capture API call is different:
it leaves the prior native state intact and reports failure to its caller.
The cancellation table also applies when the script budget aborts dispatch.
A fatal packet abandons its remaining defaults, including activation. A
fresh later click can use the existing native fallback after the failed
script runtime is retired; it does not replay the failed packet's click.

## Implementation and acceptance

One implementation slice, reviewed before C6, with these internal steps:

1. Extend event storage, PointerEvent inheritance and construction, handlers
   and capture bindings; add controller and host API. Gate exposure until the
   native route is connected. Cover constructor coercion, errors and synthetic
   event isolation in the DOM host suite.
2. Normalize native samples, connect packet dispatch and deferred defaults,
   then add capture/loss/teardown and layout-boundary integration. Correct
   comments that describe primary-only fields or absent PointerEvent support.
3. Exercise the complete path in the scripted-browser host, using GUI records
   through `window_event`, not just direct JavaScript dispatch. Run focused
   WPT pointer cases supported by the mouse profile and keep unsupported
   hardware/testdriver cases separate from passing evidence. Add
   `pointerevents` to `tools/wpt_fetch.sh` at the existing pin; refresh its
   manifest before selecting tests. The baseline sparse checkout omitted it.
   Constructor tests can use the existing harness; action-driven tests need
   a testdriver bridge into the native GUI-record path. Until that bridge
   exists, record those as unrun, not as passes from synthetic dispatch.
4. Deliver a small frozen gallery bundle after QEMU verification. The gallery
   offers freehand drawing, a captured draggable light, a three-button chord
   display, a moving target under a stationary cursor, and an ordinary
   checkbox. Transfer/removal and exact ordering have deterministic host tests.
   On P5, draw outside the canvas and window, release, return, interrupt a
   drag, and repeat at the usual zooms. This is interaction acceptance, not
   another control/candidate performance campaign.

Host gates include pointer-only listeners, first/final button transitions and
chording, capture transfer from listeners, removal while pending/current,
up/cancel release, no-button capture, unknown IDs, boundaries across nested
nodes, stationary layout changes, cancellation versus click, keyboard clicks,
navigation during handlers, Modern Web off, full native input batches and allocation
refusal. Assert event order, targets, fields and default actions; retain
ASan/UBSan/LeakSanitizer. Existing mouse/form/selection tests must stay green.

C6 then replays the saved signature/scratch scripts against these interfaces,
adds the remaining Canvas fidelity features, and tests the full applications.
PE1's gallery alone is not a claim that the live site already works.


Implementation evidence and remaining hardware acceptance are recorded in
[POINTER_EVENTS_PE1.md](POINTER_EVENTS_PE1.md).
