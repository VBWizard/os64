# PE1 — Mouse-backed pointer events and capture

Draft, 2026-11-01. Base: Canvas reconciliation `c1f354a4` (PR #447),
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
pointer identities from it. Use one stable positive mouse ID per live
browser input session. Track pointer availability separately from held
buttons: hovering is not the same as a button-active gesture.

Pen/touch hardware, gesture arbitration, pointer lock, raw updates and motion
prediction are outside PE1. Do not advertise working raw/predicted input.
Any coalesced-event interface exposed must report the samples actually kept;
constructor/IDL compatibility and trusted hardware support are separate
claims. `touch-action` does not control this mouse-only stream.

Normative reference: [Pointer Events Level 3](https://www.w3.org/TR/pointerevents3/),
especially sections 4.1–4.2, 9 and 11. Use its constructor defaults, button
transition rules, capture algorithms, event flags and compatibility mapping
as test oracles rather than copying mouse events under different names.

## Evidence from the current tree

| Existing path | Consequence for PE1 |
| --- | --- |
| `kernel/src/gui/compositor.c`, implicit pointer grab | The pressed window receives movement and releases outside its content. DOM capture needs no new kernel syscall. Verify this route in the guest and on hardware. |
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
default actions. A proposed host pointer-dispatch entry accepts a normalized
sample plus a held physical hit target and returns the default-action verdict
and any activation target needed by the host. Final C names belong to the
implementation; the ownership boundary is the contract.

Do not make JavaScript's synthetic `dispatchEvent` a source of physical
pointer state. Synthetic PointerEvents use normal dispatch; their chosen IDs
cannot activate a pointer or take over a native drag.

The native queue records a document generation, sample coordinates, button
mask, changed button and modifiers. Convert content-local positions using
the viewport origin, scroll position and zoom captured with the sample;
retain fractional CSS coordinates where the event interface supports them.
Do not reconstruct `buttons` from a subsequently changed pressed node.
Keep the physical hit and related nodes alive through queued delivery.

Capture is resolved at delivery, after the preceding callback can change it.
A down listener can capture before the next queued move/up is dispatched.
Dispatch a sample's pointer events and permitted mouse events within a
single browser input task, then use the existing rendering opportunity.
Do not call today's complete `page_event` task once per generated event:
that would permit layout/navigation between halves of one input operation.
Revalidate document generation and target connectivity after callbacks;
navigation stops delivery to the replaced document.

Keep browser widget dispatch safe: scripts run after libui returns, as they
do now. However, cancellation decisions must reach the associated deferred
default action before it commits. Audit the primary control/link doors and
the right/middle-button context-menu/activation doors together. A native
sample must not generate duplicate DOM mouse or click events through both
the new controller and an old widget callback.

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
including keyboard activation, in the same slice. Track any unsupported
auxiliary activation behavior explicitly rather than claiming full support.

Build boundary events from old/new ancestor paths and `relatedTarget`, not
just two leaf notifications. A capture overrides event targeting, not the
stored physical cursor location. On release, reconcile to that physical
location. Re-hit-test a stationary pointer after published layout changes,
including scroll/zoom, through the shared rendering path. Schedule resulting
boundary work as bounded input work; it must not recurse into layout from
painting. Scripts that repeatedly move targets under the cursor must yield
to the browser's existing task/frame limits.

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
then accept a fresh press.

Retain bounded native event batches. Queue overflow requests cancellation
through reserved state rather than silently losing the release or trying
to append cancellation to a full queue. No allocation is required to remember
that cancellation is owed. Construction failure clears native ownership and
prevents activation even if JavaScript cannot receive the cancellation event.

For PE1, do not merge pressed movement in the DOM queue. Idle motion may be
merged only with the immediately adjacent compatible sample, without crossing
button, boundary, modifier or generation changes. This fixes the current
whole-queue move search while retaining the native fairness limit.

## Implementation and acceptance

One implementation slice, reviewed before C6, with these internal steps:

1. Extend event storage, PointerEvent inheritance and construction, handlers
   and capture bindings; add controller and host API. Gate exposure until the
   native route is connected. Cover constructor coercion, errors and synthetic
   event isolation in the DOM host suite.
2. Normalize native samples, connect queued dispatch and deferred defaults,
   then add capture/loss/teardown and layout-boundary integration. Correct
   comments that describe primary-only fields or absent PointerEvent support.
3. Exercise the complete path in the scripted-browser host, using GUI records
   through `window_event`, not just direct JavaScript dispatch. Run focused
   WPT pointer cases supported by the mouse profile and keep unsupported
   hardware/testdriver cases separate from passing evidence.
4. Deliver a small frozen gallery bundle after QEMU verification. The gallery
   offers freehand drawing, captured travel across neighboring boxes, explicit
   release/target removal, and an event-order log with ordinary form controls.
   On P5, draw outside the canvas and window, release, return, interrupt a
   drag, and repeat at the usual zooms. This is interaction acceptance, not
   another control/candidate performance campaign.

Host gates include pointer-only listeners, first/final button transitions and
chording, capture transfer from listeners, removal while pending/current,
up/cancel release, no-button capture, unknown IDs, boundaries across nested
nodes, stationary layout changes, cancellation versus click, keyboard clicks,
navigation during handlers, Modern Web off, full input queues and allocation
refusal. Assert event order, targets, fields and default actions; retain
ASan/UBSan/LeakSanitizer. Existing mouse/form/selection tests must stay green.

C6 then replays the saved signature/scratch scripts against these interfaces,
adds the remaining Canvas fidelity features, and tests the full applications.
PE1's gallery alone is not a claim that the live site already works.
