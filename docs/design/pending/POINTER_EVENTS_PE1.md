# PE1 — Pointer input implementation and acceptance

2026-11-01. Branch: `codex/canvas-pointer-events`, based on the Canvas
reconciliation branch. [Design](POINTER_EVENTS.md) and
[review response](POINTER_EVENTS_RESPONSE.md).

## Implemented behavior

`PointerEvent` inherits `MouseEvent`; coordinates retain fractions. The
constructor includes pointer fields and tilt/angle conversion. Element,
Document and Window expose pointer handlers under Modern Web, and Element
exposes explicit capture. Synthetic dispatch does not change native pointer
identity, buttons or capture.

A document-owned libdom controller holds capture and boundary targets. One
normalized native sample runs as one budgeted task; generated boundary,
pointer and compatibility events are not placed in a bounded DOM-event queue.
Capture, chord transitions, canceled compatibility events, release-time click
targeting and removal fallback use that same controller. Shared libdom
activation handles links, forms and checkbox/radio preactivation and rollback.

Yonder intercepts page input before libui, including sibling form widgets.
Allowed widget defaults replay under a scoped flag that suppresses duplicate
DOM click callbacks. The toolkit's optional cancel observer lives in private
window storage; existing application-owned `os64_ui_t` records retain their
size. Native loss and interruption cancel the gesture. Window-owned drain
state consumes a pressed gesture's tail across navigation or fatal script
retirement, even after its old document/controller has gone.

Published layout, scroll and zoom changes mark stationary hit testing due.
The shared ticker reconciles once per eligible opportunity, at least 16 ms
apart, after pending rendering work. It emits boundaries without fabricated
motion. A target moving itself from a boundary handler cannot recurse into
layout or monopolize a single turn.

Right/middle input participates in the pointer stream. An eligible native
right-click menu snapshots its address before callbacks, runs press then
cancel, and consumes the old release. The menu remains window-owned.

## Automated evidence

Leak detection stayed enabled. The existing target-engine probe and fully
sanitized engine probe both ran the DOM behavioral suite (`--mutants`, which
omits the unrelated exhaustive installer sweeps): **8,023** and **15,290**
checks respectively, zero failures. ASan/UBSan cover the binding and native
fixtures in both; the second also instruments the engine. The later focused
pointer probe passes **251 checks on each engine**, including a 32-move
pressed batch crossing two 20-level ancestor paths without cancellation.

The scripted-browser suite exercises real `window_event` ingress, native
control replay, compatibility/default actions, capture outside the page and
over the question bar, context-menu cancellation/address snapshots, removed
or disabled controls, fractional zoom coordinates, stationary fairness,
navigation, and failed-runtime teardown. It passes **16,320 checks**, zero failures.
The libui damage/cancellation suite passes **72 checks**.

Pinned WPT revision `a699fb16767be39ba3e61a8e8b6bef60383f08ee`:
`pointerevent_tiltX_tiltY_to_azimuth_altitude.html` passes **25/25**. All 25
are required in `tools/wpt/expected/pointerevents.txt`. Modern Web off exposes
no constructor and passes none of these required cases, as the off gate
expects. Native WPT testdriver actions remain unrun; the host ingress tests
are separate evidence, not a claim to have passed those upstream tests.
Coalesced/predicted APIs, touch/pen hardware and a page `contextmenu` interface
are outside this cut.

The runtime-exhaustion DOM fixture allows 2 MiB for interface installation
before intentionally exhausting it with retained nodes. Its former 1 MiB
setup bound was reached during installation by the fully sanitized engine;
the exhaustion and sticky-failure assertions still run.

Reproduction:

```sh
ASAN_OPTIONS=detect_leaks=1 tools/test_dom_host.sh --mutants
ASAN_OPTIONS=detect_leaks=1 tools/test_dom_host.sh --pointer
ASAN_OPTIONS=detect_leaks=1 tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 tools/test_ui_damage_host.sh
WPT_DIR=/path/to/pinned/wpt tools/test_yonder_wpt_host.sh pointerevents --only pointerevent_tiltX_tiltY_to_azimuth_altitude
WPT_DIR=/path/to/pinned/wpt tools/test_yonder_wpt_host.sh pointerevents --only pointerevent_tiltX_tiltY_to_azimuth_altitude --modern-off
```

## Interactive acceptance

Gallery: `tools/pages/canvas-pointer.html`, installed as
`/tests/pages/canvas-pointer.html` — **A Personal Touch**. Enable Scripts and
Modern Web. Its four cards are freehand ink, a captured draggable light,
three-button chords, and a moving target under a stationary pointer. An
ordinary checkbox checks native activation alongside the drawings.

QEMU native mouse input confirmed a captured stroke leaving and reentering
the drawing, release, light dragging, all three buttons held together and
released, and enter/leave while the target moves under a stationary cursor.
The guest uses an isolated copy of the disks and the matching runtime cohort.
No kernel change is required.

P5 acceptance passed on 2026-11-01 using the frozen `f1428b2d` delivery.
Chris reported that all four gallery samples worked perfectly and approved
the slice for review. This records the reported gallery pass; individual
interaction results were not separately logged. The accepted binary cohort
and its SHA-256 manifest remain unchanged.

C6 follows with the saved signature and scratch flows and their remaining
fidelity work. PE1's gallery acceptance does not claim those full applications
already pass.


## Frame-source regression correction

A post-acceptance report found that clicking an iframe placeholder no longer
opened its source. PE1 moved native click activation into libdom, whose
standard HTML activation rules do not include Yonder's frame-source shortcut.
The browser callback treated the click as complete and omitted that default.

The browser now retains the clicked FRAME/IFRAME as an optional held fallback
until the pointer task finishes. It rebuilds the model, gives a script's own
navigation priority, and otherwise follows the current frame source as a
user-initiated navigation. Canceled clicks, detached frames, absent/empty
sources and failed runtimes do not open it. No extra click is dispatched and
no navigation task is nested inside the pointer task.

The regression produced two failures before the fix: an ordinary source click
and a click whose listener changes the source. The expanded tests cover both
frame tags, Modern Web off, cancellation/removal, absent/empty sources, one
click delivery, navigation provenance, script-navigation precedence and a
same-document fragment source. The complete browser suite passes **16,376
checks**, zero failures, with ASan/UBSan/LeakSanitizer enabled. Native Yonder
build passed. This correction changes the Yonder executable; the previously
accepted gallery bundle is retained. Chris confirmed the corrected P5 cohort
opens the reported frame source on 2026-11-01.


## Sparse drawing follow-up

The external `draw-pointermove` demo logs “saving points” and “drawing points”
from its own script. On hardware it drew slowly at modest input rates and
stopped strokes at higher rates while the pointer stayed inside the canvas.
A saved reading contained a 2,518.7 ms script task, versus 23.4 ms for the
longest screen paint. It does not identify which callback consumed that time
or record whether native input overflowed.

The shared painter blended every bitmap pixel for each small dot, including
zero coverage, and reported the entire bitmap as damaged. It now skips zero
coverage for operators that leave those pixels unchanged and encloses the
processed pixels in the reported damage. COPY, SOURCE_IN, SOURCE_OUT,
DESTINATION_IN and DESTINATION_ATOP retain their effect outside the shape.
That initial correction retained full-mask allocation and scanning, clip
semantics and whole-bitmap work admission.

A focused 1,387 × 789 host benchmark, 32 radius-five solid dots with fresh
full-size masks, took 749.225 ms before and 45.431 ms after this change. The
pixel hashes matched. These are single host measurements of mask painting,
not hardware frame-rate measurements or the complete demo callback.

The new regression fails before the fix. It checks shader work, untouched
pixels and damage for all twelve operators in 8-bit and 16-bit surfaces,
including empty clips. Canvas core passes 166,774 assertions; the complete
production-geometry Canvas suite passes 415,042; the browser suite passes
16,378. ASan, UBSan and LeakSanitizer are enabled. Native build passed.

Native input loss during an active pointer stream now leaves a diagnostic
fact in the saved page record. The existing cancellation and tail-drain
behavior is unchanged. A hardware recheck is still needed to establish the
remaining drawing latency and whether queue loss explains the stopped strokes.


## Large-window drawing follow-up

The next hardware reading at 2,526 × 1,271 confirmed native input queue loss
and pointer cancellation, with a 729 ms maximum script task. It also showed
whole-view painting: Yonder's presentation path still expanded Canvas damage
to the viewport. Small-window drawing was accepted; large-window strokes
remained choppy and stopped at high input rates.

Solid shape rasterization now uses the geometry backend's conservative bounds
of the expanded outline, including the two-pixel fringe used by SVG. A small
shape allocates and scans a small coverage buffer; destination addressing keeps
the bitmap's stride. Existing clips are cropped into scratch before any write.
Operators that affect pixels outside the shape, clip creation, gradients,
images and backends without bounds retain the full-target path. Admission still
uses the full bitmap pixel limit.

A new DOM damage visitor exposes pending bitmap-local damage without script,
reset application or acknowledgement. Yonder maps plain canvases through the
painter's shared content rectangle and object-fit geometry, scroll offsets and
a source-texel filtering fringe. Resets and ancestor transforms, opacity and
animated groups keep conservative viewport invalidation. Successful painting
retains the existing acknowledgement rule. Input-loss cancellation is unchanged.

A small-shape scratch-budget test fails before the change. Full-buffer and
compact-buffer pixels match for transformed curves, dashed strokes, fractional
clips, offscreen shapes and the compositing operators. Allocation refusal covers
the added clip scratch without partial pixels, damage or leaks. Browser tests
compare partial repaints with full repaints for two dirty canvases, borders,
scaling, scrolling, object-fit, overlap and effect fallbacks.

A sanitizer-enabled host benchmark drawing 32 actual FreeType circles took
52.390 ms before / 2.687 ms after at 600 × 600, and 434.194 ms before / 1.441 ms
after at 2,526 × 1,271. Each size's pixel hash matched. These single host
measurements cover native circle drawing, not the demo's frame rate or P5
acceptance. Canvas passes 461,751 assertions and focused browser presentation
passes 39 checks. The complete browser suite passes 16,399 checks.
ASan/UBSan/LeakSanitizer are enabled; the native build also passes.

The demo's resize coordinate mismatch is also explained by its saved source:
CSS makes the canvas follow the window, but the script initializes its bitmap
and device-pixel-ratio transform only at load and installs no resize handler.
Reload after resizing for comparable hardware tests.

Large-window P5 acceptance passed on 2026-11-01 with the frozen `e3492691`
cohort. Chris reported that drawing in a maximized window had no perceptible
slowdown and explicitly accepted the result. This is qualitative hardware
acceptance, not a measured frame-rate claim; no additional diagnostic capture
was requested. The tested binaries and their SHA-256 manifest remain unchanged.
