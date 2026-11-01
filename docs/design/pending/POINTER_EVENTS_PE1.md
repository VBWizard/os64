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

P5 acceptance is pending: try drawing outside the canvas and window, release
and return, interrupt a held gesture, try the chord and gate, and repeat at
25%, 100%, 200% and 400% zoom. This is interaction acceptance; no alternating
performance campaign is required. C6 follows with the saved signature and
scratch flows and their remaining fidelity work.
