# C6e: rectangle painting and real-page acceptance

2026-11-01. Baseline: `684f8665`, after C6d.

## Change

The shared rectangle painter handles fully covered interior spans separately
from fractional edges. Opaque fills and clears write their replacement pixels
directly. Translucent fills reuse the span's premultiplied source color while
retaining the existing blend and rounding operations. Both 8-bit and 16-bit
premultiplied surfaces retain their stride, capacity and work-limit contracts.
Fractional edges retain the original area-coverage calculation.

This changes `libdraw2d`, so Canvas callers benefit without another renderer.
Large drawing operations avoid repeated coverage calculations and source-color
conversion for fully covered interior pixels. Large translucent 8-bit fills also
tabulate the wide blend and packing result for each possible destination byte;
the four channel tables occupy 4 KiB on the call's stack, with no retained cache.
Small spans and 16-bit surfaces keep their arithmetic paths. Native-size browser
presentation walks source rows directly while preserving alpha and clipping.

Script frames retain their 20 ms DOM deadline, measured after callbacks finish,
and leave a fixed 10 ms pause after painting. The pause admits input and eligible
idle work without repeating an expensive frame's elapsed time as a mandatory
rest. This is not a CPU percentage cap. Covered-window handling, batch ownership,
missed-frame coalescing and geometry limits retain their contracts.

## Correctness

- Native Canvas/shared-paint suite: **240,341 assertions**, ASan, UBSan and
  LeakSanitizer enabled. The rectangle oracle calculates each pixel's area
  coverage independently of span selection. It covers opaque/translucent
  colors, opacity endpoints, clear, both surface formats, negative and
  fractional dimensions, clipping, extreme finite coordinates, row padding,
  exact final-row capacity, damage and atomic work-limit refusal. Large-span
  tests cover every source alpha and destination byte, opacity endpoints and
  intermediate values, and rectangles above and below the lookup threshold.
- Complete scripted-browser suite: **15,942 checks, zero failures**, with
  ASan, UBSan and LeakSanitizer enabled.
- Painter suite: **2,077 checks, zero failures**, with ASan, UBSan and
  LeakSanitizer enabled. Native-size presentation cases cover translated clips,
  negative placement, padded rows and straight/premultiplied transparency.
- Frame-loop tests cover the post-paint pause, fast-frame nominal deadline,
  clicks, timers and idle work during repeated animation, one-batch resumption
  after a long absence, covered windows and navigation teardown.
- Captured example runs release their tracked allocations. Final Canvas pixel
  hashes match the baseline at both tested sizes for Bouncing Balls, 2D Wave
  Simulation, and Starfield.

## Host measurements

The unchanged public HTML/scripts from
[Curran's examples](https://curran.github.io/HTML5Examples/) run through the
browser's parser, script queue, Canvas binding and painter. The host supplies
captured resources, the production FreeType backend with a fixture font, and
a controlled clock for scheduling. Native mouse ingress starts each animation.
Wall-clock measurements separate script/drawing from browser painting.

The initial rectangle-span comparison below is the median drawing-plus-painting cost of eight subsequent frames
in the sanitized host harness. These exclude deliberate inter-frame rest and
OS window presentation; they are **not P5 frame-rate measurements**.

| Example | Viewport | Baseline | Candidate | Less frame work |
|---|---:|---:|---:|---:|
| Bouncing Balls | 600 × 600 | 10.36 ms | 8.29 ms | 20.0% |
| Bouncing Balls | 1920 × 1080 | 56.85 ms | 42.00 ms | 26.1% |
| 2D Wave Simulation | 600 × 600 | 59.66 ms | 38.97 ms | 34.7% |
| 2D Wave Simulation | 1920 × 1080 | 104.00 ms | 73.50 ms | 29.3% |

Both selected examples' hover-started animations run without script failures
and reuse layout during the measured frames. The initial Starfield replay
reached a pre-existing geometry `QuotaExceededError` in both builds; matching
those incomplete frames was regression evidence, not successful acceptance.
The offscreen-fill follow-up below diagnoses and addresses that failure.

A standalone optimized host benchmark also retains identical output hashes.
At 1920 × 1080, opaque fills fall from 30.81 to 0.75 ms, clears from 16.66 to
0.58 ms, translucent fills from 30.36 to 13.48 ms, and a 50 × 50 fractional
rectangle grid from 35.39 to 5.48 ms. Those isolate this primitive and must not
be presented as whole-browser or hardware speedups.

## Fullscreen pacing follow-up

The first P5 browsing pass confirmed the examples draw correctly, but fullscreen
Bouncing Balls remained visibly slow. The example advances physics once per
frame, rather than by elapsed time, so fewer frames also slow its simulation.
The equal-cost rest in Yonder contributed to the reported roughly 50% thread
utilization; it was an animation scheduling policy, not a Canvas CPU quota.

A matched eight-frame sanitized host replay compares the first delivered build
(`479efaeb`) with the byte tables and native-size presentation changes:

| Bouncing Balls viewport | Prior frame work | Revised frame work | Reduction |
|---|---:|---:|---:|
| 600 × 600 | 8.11 ms | 6.72 ms | 17.1% |
| 1920 × 1080 | 41.52 ms | 33.03 ms | 20.5% |

Both final bitmap hashes match. These measurements exclude the host's frame
waiting and native window presentation, so they do not quantify the pacing
change or predict P5 frame rates. A separate optimized primitive benchmark
reduces a 1920 × 1080 translucent fill from 13.59 to 2.10 ms with matching pixels;
that is the fill alone. The P5 browsing pass confirmed a substantial improvement
at small and maximized sizes. The revised pacing is accepted, subject to later
integration regressions; the user observed utilization above 50% fullscreen and
about 30% in a smaller window.

## Starfield: offscreen fills must not interrupt visible stars

The next P5 report found smooth motion but repeated blinking without pointer
movement. The captured replay on `f383a88d` reproduces geometry exceptions that
predate the pacing change. A debugger stop identifies `bounds()` returning
`OS64_GEOMETRY_LIMIT` while converting a projected star near (286313, -148494)
to the raster grid. The page has already filled the canvas black; throwing from
that star's `fill()` aborts the loop and omits subsequent stars from the frame.

Before backend preparation, Canvas now checks whether the device-space control
points of a fill lie in one common half-plane outside the bitmap, with the same
two-pixel fringe used for coverage bounds. Lines, Bezier curves and implicit
closing edges remain inside that hull. Such a fill is a no-op for operators
that preserve destination pixels under zero coverage. The check does not cull
stroke expansion, shifted/blurred shadows, clip replacement or operators such
as copy that can affect the destination outside the shape. It does not enlarge
backend limits or swallow a backend error; intersecting extreme geometry and
path-construction limits remain explicit debts.

Validation:

- Production-geometry Canvas suite: **1,070,072 assertions**, including far-away
  and oversized offscreen circles, curves and enclosing paths that enter the
  bitmap, copy, clipping, visible shadows, wide strokes and device-space path
  lifetime across transform changes. ASan, UBSan and LeakSanitizer enabled.
- The original captured Starfield scripts run for 100 measured frames at
  600 × 600 and 1920 × 1080. The prior replays record 24 and 63 repeated geometry
  errors respectively, including startup; the new replays record none.
- Each of the 100 bitmap hashes at each size matches a diagnostic reference
  that adds a projected-circle bounding-box rejection to the page's script.
  This reference is a private fixture variant, not a modification to the site
  or to the delivered captured scripts. Runs release tracked allocations with
  leak checking enabled outside the debugger.

The P5 browsing pass approved Starfield after the fix: smooth motion without
the repeated blinking. The native QEMU run also animated and recorded a clean
diagnostic. Its first boot stalled at GUI startup with both CPUs waiting on a
kernel block-cache lock; a restart of the unchanged guest passed. That startup
hang is unresolved, with kernel traces retained separately from Canvas evidence.

## Game of Life: deferred Window named access

The completed P5 collection pass identified Game of Life's startup error:
`automata.js: ReferenceError: 'automata' is not defined, at 5:12`. The page
contains `<canvas id="automata">`, then assigns `canvas = automata` without a
variable declaration for that name. It relies on
[Window named access](https://html.spec.whatwg.org/multipage/nav-history-apis.html#named-access-on-the-window-object),
which is already planned in [WINDOW_DOCUMENT P3c](../design/pending/WINDOW_DOCUMENT.md#p3c--legacy-named-access-3-members).

The unchanged captured page reproduces the error. A private diagnostic copy
changes that one assignment to `document.getElementById('automata')`; it
initializes and runs eight animated frames without script errors, changes its
512 × 128 bitmap, and performs no frame layouts. The replay passes its ten
harness checks with ASan, UBSan and LeakSanitizer enabled and no tracked memory
left. Library feature probes still produce missing-global diagnostics; this is
not a claim of full browser-interface coverage.

The diagnostic edit is not shipped. Window named access stays a separate
browser compatibility slice; the accepted Canvas fixes proceed to review.
The wave mouse-coordinate limitation below remains recorded independently.

## Native handoff

`/tests/pages/canvas-field-trip.html` links to live versions of the two
acceptance examples. Captured source bytes remain unchanged; the
private delivery manifest records their URLs and hashes. Scripts and Modern
Web must be enabled. Begin at 100% zoom with Dark pages off, move the pointer
onto each drawing, and try grabbing a bouncing ball. Reload after changing window size so
the example's own sizing code starts from the new dimensions.

The bundle also contains the captured pages at
`/tests/pages/curran/canvas/bouncingBalls/index.html` and
`/tests/pages/curran/canvas/waveSimulation2D/index.html`. Open these explicitly
through the address bar or program arguments: page links do not authorize
local-file navigation.

Visible QEMU validation shows the native bouncing-balls drawing and animation.
The wave's click/drag interaction is not accepted: its script reads the absent
`MouseEvent.offsetX`/`offsetY`, then indexes its cell array with invalid
coordinates and throws when assigning `height`. This is the existing mouse
coordinate debt, separate from rectangle painting; the hover-started frame
measurements do not exercise it. The guide names this limitation rather than
promising that the grid can be edited.

The pacing/performance delivery changed `libdraw2d.so` and `yonder` and passed
its P5 browsing check. The Starfield follow-up changes `libcanvas.so`, retaining
that accepted pacing and the remaining tested runtime files. The earlier Danlegt
[checkpoint](c6-target-checkpoint.md) records its independent startup blockers.
