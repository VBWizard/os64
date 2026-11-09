# Pulse graphical views

Design proposal for adding Instruments and Compact to Pulse. Classic remains
available and is the default for existing configurations. The purpose is to
make activity easy to recognize at a glance while keeping Pulse small enough
to sit beside another application.

The graphical views use the same measurements, refresh interval, meter
visibility, network units, and Stay on top state as Classic. Switching views
does not restart sampling or erase the recent network peak.

## What the three views show

| View | Appearance | Information visible without hovering |
| --- | --- | --- |
| Classic | Existing text and bars | Existing labels, values, capacity details, and network scale |
| Instruments | Dark instrument faces, colored arcs, needles, and large numbers | Meter names, values, RAM/disk used and available, network directions, units, and scale |
| Compact | Five small dials with minimal surrounding decoration | Short meter labels, current values, network directions, units, and scale; fuller explanations on hover |

Instruments starts with four cards:

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

A small toolbar remains in each graphical view, with a View selector and
Settings. The selector offers Classic, Instruments, and Compact directly.
Settings also exposes the selected view. Classic keeps its existing Settings
button. There is a usable Settings control even when the frame has none or all
meters are hidden.

## Resizing that earns its place

The window is freely resizable above a measured readability minimum. The
following are fitting targets, in **content pixels**, excluding the frame:

| View | First fitting with all meters | Smaller fitting to prove |
| --- | --- | --- |
| Instruments | About 560–600 square | About 440 square, or a narrow column with more height |
| Compact | About 350 square | Around 320–360 square |
| Classic | Existing saved preference | Existing font and content rules |

These are prototype targets at approximately 24-pixel primary text, not hard
minimums. The first rendering slice must demonstrate Compact in the smaller
range before its layout is accepted. Increasing text size may require a larger
window. A 600-by-600 minimum is not the design.

Dials stay circular. Extra space enlarges the faces and improves spacing;
it does not stretch circles into ovals. Instruments can arrange cards in one
or two columns. Compact packs the visible dials into rows, keeping download
and upload adjacent and their shared scale with them. Hiding Network removes
both network dials. Other hidden meters leave no empty placeholder cards.

Layout selection uses available width and measured text. A shorter height
cannot be obtained by silently clipping a row: the native minimum is updated
for the chosen layout. Breakpoints need a small stable margin so dragging near
one does not continually alternate layouts. Changing the layout during a drag
must account for the existing minimum-size call cancelling an active gesture.

Compact is an explicit choice. Shrinking Instruments does not quietly change
it to Compact or hide its details. A user can enlarge Compact while retaining
its minimal presentation.

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
- **Disk:** selected mount, used space, available space, and total capacity.
- **Download or upload:** current rate, selected interface or aggregation, and
  the shared recent-peak scale.

Details use the latest displayed sample and configured units. They refresh
when that sample changes. Waiting and unavailable states are explained here
as well as represented on the dial.

The detail card is drawn inside Pulse's surface and clamped to its edges. It
can temporarily overlap a neighboring dial at small sizes. It does not create
a focus-taking popup, move the window, or grab input. Hit testing continues
against the underlying dial; the overlay must not trigger its own hover cycle.
Moving to another dial restarts the delay. Leaving the window, clicking,
pressing Escape, resizing, switching views, opening Settings, or losing input
cancels the card. Covering the window also cancels it. Hover must work while
Pulse is pinned but another app has keyboard focus.

When Pulse has keyboard focus, Tab can reach each visible dial; keyboard focus
shows the same details immediately. Controls retain normal keyboard operation.
This keeps useful information accessible without requiring precise mouse work.

There is no shared tooltip widget in the inspected toolkit headers. Start with
a small reusable userland overlay helper, exercised by Pulse, rather than a
new native window type. It can become the toolkit tooltip building block once
this behavior is proven.

## Honest meters

CPU, RAM, and disk use a fixed zero-to-100-percent sweep. Disk remains
**filesystem capacity**, not disk activity. The proposed `/sys/disk/stats`
work is a separate feature.

Download and upload share the existing trailing-minute, power-of-two ceiling.
The scale is shown in both graphical views. A full needle means the displayed
rate reaches that scale; it does not mean the link is saturated. If the peak
expires, the scale and both needle positions change together from one snapshot.
A unit change does not reset the byte-based peak history.

A valid zero shows zero. Missing data shows a neutral face and a dash, without
a needle that could be mistaken for a valid zero. The initial CPU/network
sampling interval shows a waiting state. Dial geometry clamps safely to its
range; labels retain the sampler's meaning.

The first version updates needles when samples arrive. Smooth interpolation
is optional later, after measuring the cost. No permanent animation loop is
needed to make the proposed layout useful.

## Remembering the user's arrangement

Keep a size for each view and one shared window position. Switching from a
large Instruments panel to Compact restores Compact's preferred size; returning
to Classic restores Classic's preferred size. Fitting a window onto a smaller
screen does not overwrite a larger saved preference.

Proposed `pulse.conf` additions:

| Key | Purpose |
| --- | --- |
| `view` | `classic`, `instruments`, or `compact`; absent/invalid means Classic |
| `instruments_size` | Preferred content width,height for Instruments |
| `compact_size` | Preferred content width,height for Compact |

The existing `geometry` key retains its format: version, frame x, frame y,
Classic content width, Classic content height. User moves update the shared
position; user resizes update the current view's size. A graphical resize must
not overwrite Classic's width and height. Old configurations need no migration
write just to launch.

View changes apply to the current window immediately. **Save as default**
persists the selected view along with other settings. Moving and resizing keep
the existing automatic geometry-saving behavior. Writes re-read the file and
merge their own keys, preserving unrelated settings and avoiding stale dialog
copies overwriting newer placement.

The current generic placement helper assumes a single size per configuration.
Pulse needs an explicit routing layer for per-view sizes; simply attaching
three ordinary instances to the same `geometry` key is incorrect. Programmatic
size changes must not be mistaken for a user resize of the outgoing view.

Preparing a view change includes its font, layout, and drawing resources.
Failure leaves the previous view usable and reports the reason. A failed
configuration save leaves the applied view visible and reports that it was not
saved. When no view can fit the screen at the requested text size, report the
constraint instead of silently switching views or reducing readability.

## Small supporting changes

The accepted Pulse source already has the important separation:
`pulse_data.c` collects measurements, while `pulse.c` owns widgets and windows.
Retain one sampler and one displayed snapshot. Add view/layout and dial drawing
code that consumes the snapshot; renderers do not read system reports.

Use the existing userland geometry backend for antialiased curves and strokes,
plus the existing text/font and surface drawing APIs. Fixed dial faces and tick
marks can be cached by dimensions, palette, and font. Dynamic numbers, needles,
and arcs redraw their affected areas. Bound the combined drawing-cache budget
(initial target: 8 MiB), release superseded resources after resize, and provide
a correct uncached drawing path when a cache cannot be retained. Report actual
memory and CPU costs against Classic rather than relying on a rounded 0% label.
There is no browser, SVG document engine, or JavaScript runtime inside Pulse.

The timer worker remains separate from widget ownership. It can check a
published hover deadline alongside the sampling deadline and ring a distinct
notification bit when details become due. Its existing sleep steps are at most
250 ms, so a nominal 500 ms hover can take about 500–750 ms plus scheduling
latency. A hover notification must not trigger an extra system sample.
Cancel pending hover state at teardown and join the worker before releasing
window or drawing resources.

### Restoring a size needs a kernel entry point

Manual window resizing already works. Automatic per-view size restoration has
one missing operation: applications cannot request an exact content size.
`os64_gui_window_set_min_size()` can grow a window, but lowering its limit does
not shrink it. Recreating Pulse's window on every view change would complicate
focus, its timer handle, and the open Settings dialog.

Propose a small `os64_gui_window_set_content_size()` syscall, with its number
assigned after checking the then-current syscall table. It should:

- Validate ownership and dimensions, reject desktop/popup targets, and honor
  the current minimum, reserved surface capacity, and frame insets.
- Resize a normal window without changing focus, pin state, or its handle.
  Preserve the maximized frame while updating its normal restore dimensions
  when the window is maximized.
- Notify the client of an actual surface-size change through the existing
  resize event. Keep programmatic changes distinct from completed user
  placement events used for preference saving.
- Handle an active native resize gesture through the same cancellation rules
  as the minimum-size setter, with coverage for its remaining button releases.
- Return an error without a partial geometry change on invalid requests.

The view transition must stage resources before changing native geometry and
restore the previous minimum if the requested size fails. On a kernel without
the new operation, allow switching views and manual resizing using the existing
minimum-size rules, but explain that automatic size restoration is unavailable.
This preserves a usable graphical view while making the kernel requirement
specific. The dials and hover details themselves require no new syscall.

## Implementation slices and acceptance

| Slice | Deliverable | Acceptance gate |
| --- | --- | --- |
| 1 — Draw and fit | Deterministic rendered Instruments and Compact previews using production drawing code, with example readings | Compact demonstrated around 320–360 pixels with readable values; inspect 24- and 26-pixel text, long rates, missing data, and hidden meters |
| 2 — Switch and resize | Both live views, Settings selection, per-view preferences, and the size-request syscall | Switch repeatedly among all views without losing position, sampling history, focus, pin state, or Classic size; verify older-kernel fallback |
| 3 — Details and finish | Mouse/keyboard detail cards, bounded caches, and lifecycle cleanup | Hover while Yonder has focus; resize with details visible; close with Settings open; compare CPU and memory against Classic |

Host tests should cover layout bounds at breakpoints, allocation/failure paths,
configuration preservation, scale/needle agreement, independent hover/sample
deadlines, and window ownership/resize behavior. Run meaningful regressions
under ASan, UBSan, and LeakSanitizer. Native QEMU tests cover the actual resize
syscall, focus, frame composition, pinning, and pointer delivery.

Hardware acceptance repeats the successful real workloads: compression and
file deletion, a network speed test, and a heavy Yonder page. Check both views
at the intended desktop resolution and confirm that Compact is comfortably
readable at its small fitting. Reopen Pulse after saving each view; verify the
saved choice, size, units, and meter selection. Exercise the existing
Apply/Save/restart, hide/show/resize, and close-with-Settings checks too.

Deliver each hardware slice as a small frozen cohort with matching libraries
and checksums. Include a kernel only for the slice that adds native size
restoration, with its requirement called out. Keep accepted cohorts intact.

Skins, graphs, per-core dials, disk activity, and animated needle interpolation
remain separate follow-ups. This design establishes the renderer separation
that can support them without committing to a skin file format.
