# Pulse graphical views

Design proposal for adding an Instruments view with an optional Compact mode to
Pulse. Classic remains available and is the default for existing
configurations. The purpose is to make activity easy to recognize at a glance
while keeping Pulse small enough to sit beside another application.

Both views use the same measurements, refresh interval, meter visibility,
network units, and Stay on top state. Switching views or toggling Compact does
not restart sampling or erase recent peaks.

## Two views and a compact option

A **view** chooses how measurements are presented. **Compact** is an option
that a view may support. The initial choices are Classic and Instruments;
Instruments supports normal and compact layouts. A compact Classic layout is a
follow-up, as are other views and their compact options.

| View | Mode | Appearance and information visible without hovering |
| --- | --- | --- |
| Classic | Normal | Existing text and bars, labels, capacity details, and network scale |
| Instruments | Normal | Dark instrument faces, colored arcs, needles, large values, full labels, RAM/disk used and available, and network directions, units, and scale |
| Instruments | Compact | Five small dials, short labels, current values, network directions, units, and scale; fuller explanations on hover |

Instruments in normal mode starts with four cards:

```text
+-----------------------+-----------------------+
| CPU                   | RAM                   |
| needle + percentage   | ring + percentage     |
| overall utilization   | used / available      |
+-----------------------+-----------------------+
| DISK                  | NETWORK               |
| ring + percentage     | download / upload     |
| used / available      | two dials + rates     |
+-----------------------+-----------------------+
```

Compact gives each network direction its own space. With every meter enabled,
its first layout is three dials above two:

```text
+-----------------------------------------------+
|       CPU          RAM          DISK          |
|       dial         dial         dial          |
|       68%          38%          71%           |
|                                               |
|          DOWNLOAD          UPLOAD             |
|            dial             dial              |
|            rate             rate              |
|            units            units             |
|           Full scale: 16 MiB/s                 |
+-----------------------------------------------+
```

CPU is cyan, RAM violet, disk amber, download blue, and upload red. These
colors identify meters; amber disk usage does not by itself mean an alarm.
Short labels and arrows remain visible, so identifying a dial does not depend
on color or hovering. Needles suit CPU and network activity; capacity rings
suit RAM and disk. RAM and disk may retain a needle as a visual treatment, but
it must agree with the ring and number.

The generated concept picture sets the visual direction. Native frame controls
and decoration still come from the selected frame composition. Pulse draws its
instrument faces inside the window; it does not replace the window frame.
Static highlights can suggest a metal rim without animated glow or blur.

Instruments has a small toolbar with a View selector, a Compact toggle, and
Settings. The View selector offers Classic and Instruments. Settings exposes
both choices separately; Compact is disabled with an explanation for a view
that does not support it. Switching to Classic does not erase Instruments'
compact preference, and returning to Instruments restores that preference.
Classic keeps its existing Settings button. There is a usable Settings control
even when the frame has none or all meters are hidden.

View metadata declares whether Compact is supported. Keep stable view IDs and a
separate per-view compact preference; do not encode Compact as a third view or
as a skin. This permits adding a Classic compact layout or another view without
changing the meaning of existing preferences.

## Resizing that earns its place

The window is freely resizable above a measured readability minimum. The
following are fitting targets, in **content pixels**, excluding the frame:

| Presentation | First fitting with all meters | Smaller fitting to prove |
| --- | --- | --- |
| Instruments, normal | About 560–600 square | About 440 square, or a narrow column with more height |
| Instruments, compact | About 350 square | Around 320–360 square |
| Classic | Existing saved preference | Existing font and content rules |

These are prototype targets at approximately 24-pixel primary text, not hard
minimums. The first rendering slice must demonstrate Compact in the smaller
range before its layout is accepted. Increasing text size may require a larger
window. A 600-by-600 minimum is not the design.

Dials stay circular. Extra space enlarges the faces and improves spacing; it
does not stretch circles into ovals. Instruments can arrange cards in one or
two columns. Compact packs the visible dials into rows, keeping download and
upload adjacent and their shared scale with them. Hiding Network removes both
network dials. Other hidden meters leave no empty placeholder cards.

Layout selection uses available width and measured text. A shorter height
cannot be obtained by silently clipping a row: the native minimum is updated
for the chosen layout. Breakpoints need a small stable margin so dragging near
one does not continually alternate layouts. Measure that margin in content
pixels. Changing the layout during a drag
must account for the existing minimum-size call cancelling an active gesture.

Compact is an explicit option. Shrinking Instruments does not quietly enable
Compact or hide its details. A user can enlarge the compact layout while
retaining its minimal presentation.

The existing text-size setting controls primary readouts. Secondary labels use
a derived size with a 16-pixel floor. Resizing does not shrink primary numbers
below the chosen size. Rate values and their units can occupy separate lines;
long values use a bounded numeric representation with the full formatted value
in details. The selected rate unit must not silently change merely to fit.

## Hover details without chasing a tiny label

Resting the pointer over a dial for roughly half a second opens a small detail
card. Examples:

- **CPU:** overall utilization, normalized across cores.
- **RAM:** used, available, and total memory.
- **Disk:** selected mount, used space, available space, and total capacity;
  explicitly identify this as filesystem capacity, not disk activity or an alarm.
- **Download or upload:** current rate, selected interface or aggregation, and
  the shared recent-peak scale.

Details use the latest displayed sample and configured units. They refresh when
that sample changes. Waiting and unavailable states are explained here as well
as represented on the dial.

The detail card is drawn inside Pulse's surface and clamped to its edges. It
can temporarily overlap a neighboring dial at small sizes. It does not create a
focus-taking popup, move the window, or grab input. Hit testing continues
against the underlying dial; the overlay must not trigger its own hover cycle.
Moving to another dial restarts the delay. Leaving the window, clicking,
pressing Escape, resizing, changing view or mode, opening Settings, or losing
input cancels the card. Covering the window also cancels it. Hover must work
while Pulse is pinned but another app has keyboard focus.

When Pulse has keyboard focus, Tab can reach each visible dial; keyboard focus
shows the same details immediately. Controls retain normal keyboard operation.
This keeps useful information accessible without requiring precise mouse work.

There is no shared tooltip widget in the inspected toolkit headers. Start with
a small reusable userland overlay helper, exercised by Pulse, rather than a new
native window type. It can become the toolkit tooltip building block once this
behavior is proven.

## Honest meters

CPU, RAM, and disk use a fixed zero-to-100-percent sweep. Disk remains
**filesystem capacity**, not disk activity. The proposed `/sys/disk/stats` work
is a separate feature.

Download and upload share the existing trailing-minute, power-of-two ceiling.
The scale is shown in both Instruments modes. A full needle means the displayed
rate reaches that scale; it does not mean the link is saturated. If the peak
expires, the scale and both needle positions change together from one snapshot.
A unit change does not reset the byte-based peak history.

A valid zero shows zero. Missing data shows a neutral face and a dash, without
a needle that could be mistaken for a valid zero. The initial CPU/network
sampling interval shows a waiting state. Dial geometry clamps safely to its
range; labels retain the sampler's meaning.

The first version updates needles when samples arrive. Smooth interpolation is
optional later, after measuring the cost. No permanent animation loop is needed
to make the proposed layout useful.

## Recent peak markers

CPU, memory, download, and upload each gain a small contrasting radial line on
the dial rim showing their highest valid sampled value over the preceding **60
seconds**. The marker is visually distinct from the current needle or filled
arc and appears in both Instruments modes. Disk capacity has no peak marker in
this slice.

For example, CPU may read 23% while a small line remains at 91%, showing that
it was recently busy. Once the 91% reading leaves the window, the line moves to
the highest sample still inside it. This is a rolling peak, not a session
maximum or a marker that decays at an arbitrary speed. Each detail card names
the period and value: **Current: 23% · Peak, last 60 seconds: 91%**.

Use the same 60-second history interval as the network scale for the first
version. A configurable period is a follow-up; keep the interval named in the
history model and UI so it can be extended coherently. Peaks describe sampled
values, not unobserved spikes between refreshes.

CPU and memory peaks are percentages on their fixed scale. Network keeps
separate raw byte-per-second peaks for download and upload, even though their
dials share a ceiling. Project each marker onto the current shared scale; do
not retain an angle from an earlier scale. The ceiling, current readings, and
peak markers come from the same displayed snapshot. Changing display units
changes formatting, not the recorded byte rates.

Keep history with the sampler, using bounded timestamped per-second buckets
rather than a sample-count window. Read `os64_micros()` once at sampling time
and use its monotonic microseconds divided by 1,000,000 for every peak bucket
and the network ceiling. Wall-clock time and `set_time` do not participate.
Retain the existing second-boundary expiry rule: a bucket contributes while
`now_second - bucket_second < 60`. Boundary quantization is less than one
second. For multiple samples in a second, retain the maximum for each metric.

The existing network scale already uses monotonic uptime ticks, not wall time.
However, `pulse_sample()` reuses the previous second when its system sample
has no usable tick snapshot, which can freeze expiry while sampling fails.
Read the history clock independently of sampler success so old peaks expire
even when measurements are unavailable. CPU utilization still uses its tick
ledgers; this change concerns history age, not the CPU percentage calculation.
A failed clock read must not become an unsigned timestamp or a new sample in
a stale bucket: expose peak history as unavailable for that turn and use the
next valid monotonic time to expire retained buckets.

The existing network history stores the maximum across both directions;
extend it or share its timing with separate direction histories so one
direction's peak is not attributed to the other. CPU and memory have their
own validity and maxima. A decreasing injected timestamp is a clock-contract
violation to reject in tests, not a supported wall-clock adjustment or a reason
to reset history in normal operation.

Missing samples do not contribute zeroes or erase valid earlier peaks. If the
current value is unavailable, keep the neutral current-state display; a
remaining historical mark must be distinguishable and explained in details.
When no valid sample remains, remove that metric's marker. A valid zero peak
can appear at the zero endpoint. History starts empty on launch. An interface
change clears network peaks and scale together, preserving CPU and memory
history. View/mode, unit, refresh interval, and visibility changes do not
clear history; sampling and aging continue while a meter is hidden or Classic
is selected.

## Remembering the user's arrangement

Keep a size for each supported **view and mode pair**, plus one shared window
position. Enabling Compact restores Instruments' compact size; disabling it
restores Instruments' normal size. Returning to Classic restores Classic's
preferred size. Fitting a window onto a smaller screen does not overwrite a
larger saved preference.

Proposed `pulse.conf` additions:

| Key | Purpose |
| --- | --- |
| `view` | `classic` or `instruments`; absent/invalid means Classic |
| `instruments_compact` | `on` or `off`; absent/invalid means off, retained when another view is selected |
| `instruments_size` | Preferred content width,height for Instruments in normal mode |
| `instruments_compact_size` | Preferred content width,height for Instruments in compact mode |

The existing `geometry` key retains its format: version, frame x, frame y,
Classic content width, Classic content height. User moves update the shared
position; user resizes update the current view and mode's size. A graphical
resize must not overwrite Classic's width and height. Old configurations need
no migration write just to launch.

View and Compact changes apply to the current window immediately. **Save as
default** persists the selected view and per-view compact preference along with
other settings. Moving and resizing keep the existing automatic geometry-saving
behavior. Writes re-read the file and merge their own keys, preserving
unrelated settings and avoiding stale dialog copies overwriting newer
placement.

The current generic placement helper assumes a single size per configuration.
Pulse needs an explicit routing layer for sizes indexed by view and mode;
attaching ordinary placement instances to the same `geometry` key is incorrect.
Programmatic size changes must not be mistaken for a user resize of the
outgoing presentation. The shared placement helper can gain a role/mode key
when a second application needs it; that extraction is tracked in the GUI
section of `DEBTS.md`. Keep the Pulse adapter small enough to move there.

Preparing a view or mode change includes its font, layout, and drawing
resources. Failure leaves the previous view usable and reports the reason. A
failed configuration save leaves the applied view visible and reports that it
was not saved. When no view can fit the screen at the requested text size,
report the constraint instead of silently switching views or reducing
readability.

## Small supporting changes

The accepted Pulse source already has the important separation: `pulse_data.c`
collects measurements, while `pulse.c` owns widgets and windows. Retain one
sampler and one displayed snapshot. Add view/layout and dial drawing code that
consumes the snapshot; renderers do not read system reports.

Use the existing userland geometry backend for antialiased curves and strokes,
plus the existing text/font and surface drawing APIs. Fixed dial faces and tick
marks can be cached by dimensions, palette, and font. Dynamic numbers, needles,
peak markers, and arcs redraw their affected areas. Bound the combined
drawing-cache budget (initial target: 8 MiB), release superseded resources
after resize, and provide a correct uncached drawing path when a cache cannot
be retained. Report actual memory and CPU costs against Classic rather than
relying on a rounded 0% label. There is no browser, SVG document engine, or
JavaScript runtime inside Pulse.

The current `os64_gui_event_wait()` and `os64_gui_event_wait_any()` ABI has no
timeout argument. `ui_dispatcher.c` calls the latter through a wait callback
that also has no deadline parameter. Keep the worker-based hover deadline;
there is no timed wait to reuse and this slice does not add one.

The timer worker remains separate from widget ownership. It can check a
published monotonic hover deadline alongside the sampling deadline and ring
a distinct notification bit when details become due. Its existing sleep
steps are at most 250 ms, so a nominal 500 ms hover can take about 500–750 ms
plus scheduling latency. A hover notification must not trigger an extra
system sample. Cancel pending hover state at teardown and join the worker
before releasing window or drawing resources.

The existing compositor's `pointer_state_locked()` targets the topmost content
under the pointer, independently of keyboard focus, and libui consumes
`OS64_GUI_EVENT_POINTER_STATE` without discarding it on blur. Before building
the detail card, verify native delivery in QEMU with Pulse pinned and another
app focused. Treat a failure as a window-system regression, not a reason to
poll global pointer position or steal focus from the other application.

### Restoring a size needs a kernel entry point

The kernel already resizes windows through `wm_resize()`: a frame gesture or
maximize/restore action reaches the window manager directly inside the kernel.
An application can choose an initial size at creation and can call
`os64_gui_window_set_min_size()` afterward. That setter grows an undersized
window, which is how Pulse makes room for larger text, but lowering the limit
does not shrink the window.

The missing operation is a userland request to make an **existing** window a
specific content size, such as changing from 580 by 580 to a remembered 340 by
340. The proposed syscall exposes the existing resizing machinery with owner
and bounds checks; it does not invent a second resizing engine. It is needed
for automatic view/mode size restoration, not for manual dragging or drawing
dials. Recreating Pulse's window on each change would instead complicate focus,
its timer handle, and the open Settings dialog.

Propose a small `os64_gui_window_set_content_size()` syscall, with its number
assigned after checking the then-current syscall table. It is an exact request
on an owned window:

- Check ownership and ordinary-window policy, rejecting desktop/popup targets.
  Validate the full raw arguments before narrowing dimensions. Under the GUI
  lock, derive the frame using the current decoration insets and check the
  effective minimum, including the decoration's width floor, and reserved
  surface capacity before touching geometry or cancelling gestures.
- Keep the frame's top-left corner anchored when the new frame fits. Otherwise
  shift its position just enough to fit the complete frame on screen, using
  `x = clamp(old_x, 0, screen_width - frame_width)` and the equivalent for y.
  If the requested content plus chrome cannot fit on screen, refuse. Shifting
  position is permitted; silently reducing the requested size is not.
- Refuse dimensions below the effective minimum or above reserved capacity
  without change. Give these distinct size-error results, and a third for
  failure to fit the complete frame on screen. Existing ownership/handle and
  malformed-argument errors retain their meanings. Allocate the new error
  values with the ABI change; the caller cannot infer its capacity today.
- Resize without changing focus, pin state, minimization state, or handle.
  For a maximized window, anchor and fit the **normal restore frame**, updating
  its dimensions/position without changing the maximized frame or current
  surface. Document this hidden restore-state change in the public header.
- Return success for an already satisfied request; `wm_resize()` returning
  false for a no-op is not a syscall failure. Its surface-size invariant
  checks remain in place; reuse the engine without promising that its boolean
  result alone distinguishes no-op from an internal refusal.
- Use the existing `WINDOW_RESIZE` notification and readback after an actual
  visible geometry change. A restore-only change needs no surface resize
  notification. Add no programmatic-origin flag, tag, or new event type.
- Retain the existing `WINDOW_PLACEMENT` contract: it describes completed user
  gestures, not automatic fitting, minimum growth, or this syscall. Userland
  refreshes layout on resize notifications and saves preferences from placement
  notifications. It records its requested target for transition bookkeeping;
  matching resize dimensions are not a reliable event-origin identifier when
  changes coalesce. Reconcile current state after a notification or input loss.
- After validation, handle active resize gestures through the existing
  cancellation rules, including their remaining button releases. An invalid
  request must leave both geometry and the active gesture untouched.

The frame-fitting policy parallels content-window creation, whose fit step
lives in `gui_client.c`; it does not reuse the creation path's size-clamping
semantics. Pulse may explicitly calculate a smaller live fitting target when
restoring a preference on a smaller screen, preserving the stored preference.
The kernel must still honor that explicit target exactly or refuse it.

The view/mode transition stages fonts, layout, and drawing resources before
changing native state. Snapshot the old minimum and live geometry. If raising
a new minimum grows the old window and the subsequent exact-size request
fails, restore both the old minimum and old geometry before retiring the old
view; restoring the floor alone does not undo that growth. Exercise this
failure ordering in tests. Re-read native state after any failed rollback and
keep the still-active view laid out to the actual surface, reporting the
failure rather than claiming the transition was atomic.

Keep a bounded older-kernel behavior: a view/mode change that fits the current
surface can proceed, while automatic size restoration reports that it is
unavailable. If the candidate cannot fit, retain the current view and ask for
manual resizing or a kernel update before retrying. Do not grow the window
through a minimum-size side effect to emulate the missing operation. Manual
resizing remains available. Staged delivery can install userland before the corresponding
kernel is booted. A successful rebuild does not establish which kernel is
running. Test this path on a kernel without the new handler rather than
assuming the mixed deployment cannot occur.

Do not call every rejected request an old kernel. The existing unimplemented
syscall result is -1, also used for an invalid GUI handle. Revalidate the live
owned window with readback before presenting the unavailable-operation hint;
report known bounds/ownership failures as their actual cause. Preserve the
user's preferred sizes on failure and avoid repeated retry/error messages.
The fallback does not emulate the syscall by recreating windows or launch a
second sizing engine. The dials and hover details themselves require no new
syscall.

## Implementation slices and acceptance

| Slice | Deliverable | Acceptance gate |
| --- | --- | --- |
| 1 — Draw and fit | Deterministic normal/compact Instruments previews using production drawing code, including recent-peak markers | Compact demonstrated around 320–360 pixels with readable values; inspect 24- and 26-pixel text, long rates, missing data, and hidden meters |
| 2 — Switch and resize | Classic/Instruments selection, Compact toggle, per-view/mode preferences, live peak histories, and the size-request syscall | Switch views and toggle Compact without losing position, peaks, focus, pin state, or saved sizes; Classic ignores but retains the Instruments compact preference; verify older-kernel fallback |
| 3 — Details and finish | Mouse/keyboard detail cards, bounded caches, and lifecycle cleanup | Verify unfocused hover delivery before building the overlay; resize with details visible; close with Settings open; measure idle/loaded CPU and peak cache bytes against Classic, including the 8 MiB cache target |

Host tests should cover layout bounds at breakpoints, allocation/failure paths,
configuration preservation, scale/needle/peak agreement, peak expiry and
per-metric validity and distinct direction peaks, multiple samples per second,
long gaps, wall-clock changes, history expiry during sampler failure,
clock-contract violations in injected tests, interface changes, independent
hover/sample deadlines, and window ownership/resize behavior. Run meaningful regressions
under ASan, UBSan, and LeakSanitizer. Native QEMU tests cover the actual resize
syscall, focus, frame composition, pinning, and pointer delivery. Include
requests near each screen edge/corner, a frame too large to fit, distinct
minimum/capacity refusals, maximized restore-only changes, no-op success,
active-gesture refusal/cancellation, and absence of user-placement writes for
programmatic fitting. Slice 2 takes the kernel review route for the syscall
and its native fixture; slices 1 and 3 retain application review scope.

Hardware acceptance repeats the successful real workloads: compression and file
deletion, a network speed test, and a heavy Yonder page. Check both Instruments
modes at the intended desktop resolution and confirm that Compact is
comfortably readable at its small fitting. Create a short CPU/network burst and
verify that its peak mark remains after the current value falls, then expires.
Reopen Pulse after saving each view/mode; verify the saved choice, size, units,
and meter selection. Exercise the existing Apply/Save/restart,
hide/show/resize, and close-with-Settings checks too.

Deliver each hardware slice as a small frozen cohort with matching libraries
and checksums. Include a kernel only for the slice that adds native size
restoration, with its requirement called out. Keep accepted cohorts intact.

## Follow-up for a floating instrument cluster

Keep the five-dial Instruments compact layout in the current scope. A later
presentation can visually join the instrument rims while removing the
rectangular panel, so the desktop and other applications show through the
spaces between them. The instruments remain visible; the containing window
still exists to own their pixels, input, position, and lifetime.

Prefer **click-through gaps with interactive instruments**. Hovering over a
dial should still show its details, and an instrument rim or small handle can
provide a place to drag the cluster. A settings/context action must remain
reachable without a titlebar. Fully passing mouse input through the entire
cluster would prevent its ordinary hover and click interactions; that would be
a distinct locked-overlay mode with an external way to unlock it.

The inspected window system needs additional support for this presentation:

| Capability | Current behavior | Follow-up requirement |
| --- | --- | --- |
| Frame | No-decorations removes the titlebar but retains a one-pixel border | A genuinely frameless ordinary window, retaining normal lifetime and focus rules |
| Visible shape | Client content is copied as a rectangle | A published shape mask or per-pixel alpha, allowing empty space around visible dials |
| Drawing underneath | Occlusion assumes the window's whole frame is opaque | Transparent gaps must not hide or suppress updates from underlying windows |
| Pointer targeting | `wm_topmost_at()` uses the frame rectangle | An input region that passes gap clicks and hover to the window below while retaining dial input |

The existing creation-time combination of no decorations and pinned also
classifies a window as a popup. A persistent instrument cluster must not
accidentally inherit menu lifecycle or owner-focus restrictions from that
combination. Give the window type and decoration policy an explicit contract.

Drawing a translucent color inside Pulse does not make the desktop visible: the
current compositor still copies the window surface opaquely. Likewise, merely
skipping a pixel would leave stale backbuffer content because the opaque window
has already prevented painting underneath it. Visual shape, occlusion, damage,
and input routing must agree.

A binary cutout shape could establish the first version: solid instruments and
transparent gaps. Antialiased outer edges and translucent shadows require
partial-alpha composition. Keep the input region independent of decorative
shadows, and publish validated shape/input state consistently with the drawn
content. The follow-up should cover movement, resizing, shape changes, partial
overlap, pointer capture, and hover transitions over the gaps.

This is window-system work with Pulse as its first consumer, rather than an
extra dial-rendering option. It does not block Instruments, its resizable
Compact mode, or hover details in a normal window.

Skins, graphs, per-core dials, disk activity, and animated needle interpolation
also remain separate follow-ups. This design establishes the renderer
separation that can support them without committing to a skin file format.
