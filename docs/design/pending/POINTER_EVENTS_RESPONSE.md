# PE1 — response to Fable's design review

Quinn, 2026-11-01. Review baseline: `1af3d38c`.
The original review is preserved in `POINTER_EVENTS_REVIEW.md`; changes are
in [POINTER_EVENTS.md](POINTER_EVENTS.md). This is a design revision, not an
implementation or a claim of passing pointer tests.

## 1. Toolkit routing — accepted; choose browser ingress

Confirmed in `userland/libos64/ui.c`: right presses take the context door,
left presses establish a grab, other presses are dropped, and releases are
filtered against the grab button. The kernel guarantee alone was insufficient.

I choose the proposed browser-bypass alternative, with an explicit page/native
ownership classifier and default-action replay. An all-buttons flag on the
view is insufficient because `forms_sync` attaches page form widgets directly
to `g.root`, beside `g.view`. The design now names that topology, duplicate
callback suppression, safe widget lookup after script, and an optional toolkit
cancellation notification from `cancel_widget`. Cancellation callbacks only
mark debt; script runs outside the widget stack. Native defaults remain
button-specific. This does not change other applications' button policy.

## 2. Secondary/middle policy — accepted, with eligibility made explicit

The window-owned context menu is an existing product decision and stays.
Middle input and eligible `auxclick` reach the page, without adding a new
middle-click navigation default. Secondary input also reaches the page when
no window menu is eligible. When a link/picture menu is eligible, the press
is delivered, the page stream is canceled, and the window menu opens. A
chorded secondary press is not another pointerdown. Its release is consumed
as abandoned input after cancellation, not delivered as a normal auxclick.
No page `contextmenu` is introduced; the design names that conformance
exception. Keyboard menu opening also interrupts an active page gesture.

## 3. Activation target — accepted, with two corrections

Replace the equality check for uncaptured activation with the common-ancestor
rule and derive navigation from that target's enclosing link. Captured
release is the exception: activation retains the release target rather than
recomputing the ancestor after capture has been cleared.

The proposed text-versus-padding example does not establish the present bug:
`element_hit` already normalizes a text hit through `element_of`. The new
regression uses distinct nested spans inside the same link, plus release
over an unrelated link. Native control activation must follow the same
resolved target and cannot silently commit an old widget callback instead.

## 4. Queue capacity — accept the invariant, correct the failure mechanism

`window_event` calls `inputs_run` after each native event. The 32-event native
batch is therefore not presently accumulated as 32 expanded DOM packets.
Furthermore, expansion was intended inside dispatch, not as one queue entry
per generated PointerEvent. The original design needed to say that explicitly.

PE1 now queues one sample per packet and expands it directly during its task.
Admission drains pending work before polling again, with a reserved ingress
slot separate from the native batch's look-ahead slot. Cancellation is not
the response to ordinary drawing throughput. A 32-record pressed-motion
stress case with deep boundary paths is an acceptance gate. Multiplying a
fixed queue by four boundary events would also miss ancestor enter/leave
walks; sample queuing avoids that dependency.

## 5. Stationary boundary events — agree about missing mechanism; retain scope

This is my substantive disagreement with the recommended deferral. PE1
exposes a page-wide interface, not an interface private to the two Canvas
apps. Layout-driven boundaries are required by
[PE3 §4.1.4](https://www.w3.org/TR/pointerevents3/#boundary-events-caused-by-layout-changes),
which also permits delay for performance. Avoiding them because our first
two captured gestures do not need them would bake in a known hover defect.

The revised design supplies the missing mechanism: a document-generation-tagged
re-hit debt, published hit-test revision, last physical sample and deadline;
one boundary-only dispatch after geometry publication per eligible rendering
opportunity; a minimum 16 ms interval through the existing ticker, including
when no rAF callback exists. Clear the flag before callbacks so their edits
owe a later turn. Covered-window suspension and uncover recovery follow 5d.
Navigation/close clears it. No fabricated move and no layout-to-script call
from the painter. The lifecycle tables and a self-moving-target fairness
regression make the loop hazard reviewable.

I would like Fable to challenge this mechanism if a particular transition is
still unsound; I do not propose booking the feature as debt merely because
it is new machinery.

## 6. Coordinates — accepted; doubles now

Widen the host event coordinate record and queued CSS snapshot to doubles,
then audit host constructors/getters/call sites. Keep the native kernel event
ABI integral. Snapshot viewport/scroll/zoom before callbacks and test fractional
zoom positions numerically. The browser and matching library binaries move
together. The design also calls out the existing misuse of content-local
coordinates as screen coordinates, so widening does not bless that error.

## 7. Lifecycle audit — accepted

Added two state-by-transition tables, covering ordinary input/capture/removal/
geometry and navigation/close/loss/host cancellation/overflow/allocation
failure. They distinguish compatibility suppression from draining an abandoned
native gesture, and failed API calls from failed native dispatch. The new
stationary-hit debt is included. Node replacement, document fallback,
cancellation reentrancy, partial allocation and silent teardown are explicit.

## 8. Sequence oracles — accepted, with release/cancel qualifications

Added a consolidated list. Lost capture after up/cancel describes implicit
release; explicit release and transfer can report lost before a later move.
Also, cancellation does not imply that no mouseup of any kind exists: the
hover compatibility mapping can send a mouseup to Window when mouse events
were not suppressed. That cleanup must never enter ordinary click activation.
The revised design distinguishes a canceled pointerdown from a canceled
stream, and capture transitions from boundary reconciliation.

## 9. Surviving kernel grab — accepted

Confirmed the two independent state machines. `event_queue.c` discards queued
gesture history while the compositor retains `s_pointer_window` until the
physical final release. The design now explains why that release is expected
and why browser drain-until-neutral is necessary. No kernel change is called
for. Navigation preserves host drain state too, so an old page's release
cannot activate its replacement.

## 10. Identity and WPT — accepted

Pin the mouse ID to 1 for the window lifetime, with document-local availability
and capture. Add `pointerevents` to `tools/wpt_fetch.sh` without changing the
WPT commit. The tests have been added to the local sparse checkout using an
additive operation, retaining its other directories. Manifest regeneration
and supported test selection precede implementation validation. Action-driven
WPT tests require a bridge to the GUI ingress; synthetic DOM dispatch cannot
stand in for that bridge. No WPT pointer pass is claimed by this revision.

## Revision checks

Re-read the named kernel, toolkit, browser and DOM paths; checked pointer
ordering, targeting and stationary boundaries against the primary specification.
Checked local Markdown links, whitespace, the retirement-reference report and
shell syntax of the changed WPT fetch script. Runtime code is unchanged.


## Implementation follow-through

The implemented ingress uses a synchronous packet on the `window_event`
call stack. It completes before polling another native record, so the
reserved admission slot needs no separate queue allocation. Ancestor paths
expand inside the packet's task. The deep-path 32-move regression passes on
both engine builds. Window-level drain state survives document replacement
and script-runtime retirement; document-owned capture does not.

The stationary-boundary mechanism is implemented and exercised by a
self-moving target that would loop without the rendering-opportunity bound.
The gallery includes the same behavior visibly. See
[PE1 evidence](POINTER_EVENTS_PE1.md) for the implementation checks and
hardware acceptance status; the earlier revision checks above describe the
design-only commit.
