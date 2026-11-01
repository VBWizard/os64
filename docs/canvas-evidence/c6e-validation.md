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
The animation scheduler, frame deadlines, rest policy and geometry limits are
unchanged. Large drawing operations no longer repeat coverage calculations
and source-color conversion for each fully covered interior pixel.

## Correctness

- Native Canvas/shared-paint suite: **230,101 assertions**, ASan, UBSan and
  LeakSanitizer enabled. The rectangle oracle calculates each pixel's area
  coverage independently of span selection. It covers opaque/translucent
  colors, opacity endpoints, clear, both surface formats, negative and
  fractional dimensions, clipping, extreme finite coordinates, row padding,
  exact final-row capacity, damage and atomic work-limit refusal.
- Complete scripted-browser suite: **15,931 checks, zero failures**, with
  ASan, UBSan and LeakSanitizer enabled.
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

Each row is the median drawing-plus-painting cost of eight subsequent frames
in the sanitized host harness. These exclude deliberate inter-frame rest and
OS window presentation; they are **not P5 frame-rate measurements**.

| Example | Viewport | Baseline | Candidate | Less frame work |
|---|---:|---:|---:|---:|
| Bouncing Balls | 600 × 600 | 10.36 ms | 8.29 ms | 20.0% |
| Bouncing Balls | 1920 × 1080 | 56.85 ms | 42.00 ms | 26.1% |
| 2D Wave Simulation | 600 × 600 | 59.66 ms | 38.97 ms | 34.7% |
| 2D Wave Simulation | 1920 × 1080 | 104.00 ms | 73.50 ms | 29.3% |

Both selected examples' hover-started animations run without script failures
and reuse layout during the measured frames. Starfield reaches a pre-existing `QuotaExceededError`
from Canvas geometry in both builds; matching its pixels is regression
evidence, not successful acceptance. Its extreme projected geometry requires
separate investigation under the existing bounds policy.

A standalone optimized host benchmark also retains identical output hashes.
At 1920 × 1080, opaque fills fall from 30.81 to 0.75 ms, clears from 16.66 to
0.58 ms, translucent fills from 30.36 to 13.48 ms, and a 50 × 50 fractional
rectangle grid from 35.39 to 5.48 ms. Those isolate this primitive and must not
be presented as whole-browser or hardware speedups.

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

P5 acceptance is pending. The delivery changes only `libdraw2d.so` in the
runtime cohort and reuses the other tested binaries. The earlier Danlegt
[checkpoint](c6-target-checkpoint.md) records its independent startup blockers.
