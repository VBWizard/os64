# Pulse graphical design review disposition

Response to Fable's review of design commit `65127113`. These are design
changes; no syscall, renderer, or tooltip implementation is included.
The [updated design](PULSE_GRAPHICAL.md) remains the implementation contract.

## Accepted changes

| Review point | Disposition |
| --- | --- |
| Resize anchor and screen edges | Anchor at the frame's current top-left. Shift to fit the complete frame, including current chrome. Refuse if the exact content size cannot fit even after shifting. Maximized requests apply this policy to the normal restore frame. Add edge/corner native cases. |
| Refuse rather than clamp exact sizes | Prevalidate raw arguments, effective minimum including decoration width, reserved capacity, and screen fit under the GUI lock. Return distinct size-bound errors without geometry or gesture mutation. Keep manual drag clamping unchanged. |
| Maximized hidden state | Explicitly specify and document that only restore geometry changes while maximized. No current-surface resize event is required for that hidden change. |
| No programmatic resize-event flag | No new flag or event. Preserve the existing distinction between layout notifications and completed user-placement notifications, as explained below. |
| Monotonic history | Use one independent `os64_micros()` reading for the history clock on each sample turn. Remove wall-clock/backward-reset language; test wall-clock independence and measurement failures. |
| Check for a timed event wait | Checked: neither wait API nor the dispatcher wait callback accepts a timeout. Retain the bounded worker/doorbell design and its stated hover latency. |
| Unfocused hover delivery | Source supports it through `POINTER_STATE`. Require the native QEMU check before implementing the overlay, not just as a final acceptance test. |
| Placement helper reuse | Booked a GUI debt for role/mode-aware shared placement when a second application needs it. Keep Pulse's first adapter local and preserve its saved-size semantics. |
| Covered tooltip, content-pixel breakpoints, disk meaning | Specify covered-state reconciliation, content-pixel breakpoint margins, and explicit filesystem-capacity wording in the disk details. |
| Performance and review gates | Make measured cache bytes and CPU comparison explicit in slice 3. Slice 2 takes kernel review for its syscall and fixture. |

## Qualified by the existing implementation

**Resize versus placement is already an ABI distinction.**
`WINDOW_RESIZE` updates surfaces/layouts after both automatic changes and user
gestures. `WINDOW_PLACEMENT` records completed user moves/resizes and excludes
automatic fitting and maximize/restore. `placement_complete_locked()` in
`kernel/src/gui/compositor.c` creates that report; `window_placement.c` saves
from it. Preserve those contracts. Do not add an origin flag, but also do not
replace placement events with a guess based on matching resize dimensions.
Coalescing and later gestures make dimension matching insufficient for origin
attribution. The caller records its requested target and reconciles readback
for the transition itself.

**The current peak clock is already monotonic, but its availability is coupled
to another sampler.** `pulse_sample()` uses `sys->ticks` when present and the
previous scale timestamp otherwise. The network peak can therefore outlive
its intended interval during repeated system-sample failures. An independent
monotonic read removes that coupling. CPU percentage calculation continues to
use its existing tick ledgers. Wall-time changes must not alter peak age.

**The resize engine has no normal allocation failure, but its boolean is not
an unconditional success result.** `wm_resize()` returns false for a no-op
and also retains defensive surface-capacity refusal/rollback checks. The new
wrapper must accept an already satisfied request and preserve those guards.
Validate before cancelling gestures; do not interpret every false as a failed
request or remove invariant checks to simplify the new call.

The shared placement helper prepares content dimensions; the final
frame-position fit at creation is in `gui_window_create()` under the GUI
lock. The new setter shares that positional policy, while intentionally
refusing size violations instead of using creation's clamping behavior.

## Retained older-kernel behavior

The request to remove the older-kernel path is not adopted. Kernel and
userland may be built from the same source revision yet become active at
different times: a staged userland update takes effect before the installed
kernel is booted. The accepted Pulse pin update already exercised that case.
Build consistency does not establish the running kernel's capabilities.

Keep a small failure path: report that automatic size restoration is
unavailable and preserve preferred geometry. A mode that fits the current
surface can be displayed; otherwise retain the current view and ask for manual
resizing or a kernel update. Test the new app on a kernel without the handler.
Do not emulate exact resizing or recreate windows. This is a bounded
compatibility behavior with a native test, not an untested alternate sizing
implementation.

An unimplemented syscall currently returns -1, the same value as an invalid
GUI handle. A negative result alone cannot prove that a kernel is old. Check
the live owned window before presenting the compatibility hint and report
distinct minimum/capacity/screen-fit errors as those errors. A refusal must
not silently claim the preferred size was restored.

## Additional transition check

Changing a minimum can already grow the live window before the new exact-size
request executes. If that later request fails, restoring only the minimum
does not shrink the window back. The design now requires old minimum/geometry
snapshots, resource staging, and rollback of both, with readback and a truthful
error if rollback fails. Add a failure-ordering test for this sequence.

No implementation tests were rerun for this documentation revision. Code
inspection resolved the wait, pointer, clock, resize, and placement questions;
the native cases above are gates for the corresponding implementation slices.
