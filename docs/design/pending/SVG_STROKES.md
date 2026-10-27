# SVG dashes and zero-length caps

2026-10-26. This 6b increment follows [basic shapes](SVG_SHAPES.md).
Its branch is `codex/svg-strokes`; its review base is the persistent
`codex/svg-integration` branch, rooted at merged PR #369 (`78012540`).
Further SVG increments stack onto that integration branch. The checkpoint is
`/tests/pages/svg-strokes.html`: six cards with a reversible dash-offset button.

## Stroke construction

`libsvg` owns SVG dash semantics. `svg_style_t.dash` contains resolved,
nonnegative user-space lengths and a signed offset. A provider may lend the
array until its next callback; scene preparation validates and copies it.
There are at most 256 input entries. Empty/all-zero arrays produce a solid
stroke; odd arrays repeat to make an even cycle. Each subpath restarts its
phase. Signed offsets normalize over the full cycle. Positive-length dashes
clipped to nothing at an offset boundary do not invent a dot. An explicit
zero-length dash produces a cap, except at a final gap endpoint.

Dash cuts happen before the user-to-device map. Quadratic and cubic curves
are split with de Casteljau; the pieces stay curves. Chord/control-polygon
length bounds drive adaptive measurement. Each recursive child receives half
its parent's error allowance. The metric allowance is the render arc error
divided by eight times a conservative map norm, then divided among subpath
segments. Cut parameters use bounded bisection with separately bounded
prefix measurements. This bounds the measurement of the **lowered Bézier
path**; it does not claim a tangent or true source-arc length bound. The
existing source-arc approximation and FreeType curve coverage limits remain.

Subdivision depth is capped at 28 and parameter search at 64 iterations.
Measurement, scanning, output growth and dot construction charge the shared
work/allocation budgets. Patterns that exhaust these budgets, non-progressing
arithmetic, excessive expansion and non-finite intermediates refuse the render. Combined
centerline/cap commands must fit the backend's 4,096-command limit. A failed
render publishes no image through Yonder.

A dash that crosses a closed subpath's seam is emitted as one open piece
through that vertex, preserving the join. A single dash covering the entire
closed path retains CLOSE. Zero gaps between separate positive dashes retain
separate caps; replacing such a pattern with a solid line would change corners.

Exact zero-length subpaths containing a drawing command become round or
square cap contours; butt caps and MOVE-only paths paint nothing. Round dots
use the native ellipse/arc lowering with the current device error allowance.
Squares are aligned with the user axes before transformation. For explicit
zero-length dashes this follows Chromium's behavior, including on a diagonal
path, rather than claiming a tangent-oriented square interpretation.
Nonzero curves that collapse below the backend's working grid can still
return `SVG_UNSUPPORTED` for non-butt caps.

## One paint operation for overlapping pieces

The geometry v1 table adds an optional `stroke_with_caps` tail. Ordinary
`stroke` remains available; a consumer checks `struct_size` before reading
the tail. A backend without it can still render ordinary dashed lines, but
explicit cap contours request `SVG_UNSUPPORTED`.

FreeType expands the centerline in a frame planned jointly with the cap
contours. Clockwise cap contours are appended with the stroke's outer winding.
They are rasterized together with the nonzero fill rule, then colored and
composited once. This is essential for overlapping dots and for a dot inside
a translucent stroke: drawing each cap as a separate paint would darken the
overlap. Both streams are independently validated before concatenation;
allocation and cancellation failures release the combined outline.

## CSS and profile limits

`stroke-dasharray` uses the existing CSS grammar. `libgarb` adds inherited
`stroke-dashoffset`, including its presentation attribute and signed grammar.
The Yonder adapter resolves numbers, px and percentages; percentages use the
normalized viewport diagonal. The cascade owns ordinary inheritance,
initial/unset, custom properties and stylesheet/attribute precedence.
Dash lengths are resolved at the geometry consumer, not at an ancestor.
A script mutation rebuilds the scene through the existing repaint path.

Relative/calc dash values remain outside the adapter's profile. A nonempty
`pathLength` attribute with an active nonzero dash pattern is explicitly
refused: author-calibrated lengths need a separate metric contract. Solid or
all-zero patterns do not need that calibration. Non-scaling stroke, markers,
additional join modes, animation and the other static SVG feature debts are
unchanged. See [DEBTS.md](../../../DEBTS.md).

The semantics are grounded in SVG2's
[stroke dashing and shape algorithms](https://www.w3.org/TR/SVG2/painting.html#StrokeDashing)
and [pathLength definition](https://www.w3.org/TR/SVG2/paths.html#PathLengthAttribute).
The square-dot browser choice above and the calibration refusal are explicit
profile boundaries.

## Validation

The native core passes 148,938 checks. The painter passes 1,241,506 assertions,
including borrowed-array lifetime, older backend tables, malformed styles,
expansion refusal, overlapping cap/line coverage, 104 allocation positions
in both refusal modes and 2,666 cancellation positions. A separate
32-by-32 supersampled analytic dashed-circle reference measures circular
annular sectors and endpoint disks without using the native stroker:
maximum alpha difference 41/255 and integrated error 13.875 pixels, bounded
at 48/255 and 16 pixels. This is a fixture envelope, not a global guarantee.

The backend passes its ownership, malformed-stream, aggregate command-limit,
budget and reentrancy checks, including 33 allocation cuts and 1,109
cancellation cuts with cap contours present. The Yonder window/script suite
passes 8,253 checks, including a real script moving dash gaps and releasing
its render memory. CSS regressions pass. ASan, UBSan and LeakSanitizer remain
enabled; leak checks run outside the sandbox that denies thread attachment.

The real HTML/cascade/layout adapter passes 35 stroke fixtures, including
25 exact comparisons with independently specified solid paths or shapes.
They cover phase signs/wrapping, percentages, inheritance/reset, odd arrays,
zero patterns, zero-gap corners, closed seams, dot endpoints, retracing curves,
nonuniform transforms and overlap opacity. The zero-length subpath at an
empty initial gap failed before its focused walker correction and passes
with it. A mixed curve/dot fixture passes 72 allocation cuts in both refusal
modes; the existing 19 shape pairs and 60/81-position failure sweeps also pass.

Chromium 140.0.7339.16 / Playwright 1.55.0 comparisons pass **105/105 expected
outcomes**: 70 reference images match their declared envelopes, 33 shifted
controls fail and two empty controls remain empty. Integral straight edges
require exact agreement. Most other cases cap channel differences at 80/255,
changed pixels at 500 and integrated error at 20 pixel equivalents. Three
curve-specific envelopes are explicit in `tools/svg_reftests/strokes.py`:
quadratic 88/255, 240 pixels, 12 equivalents; dashed circle 160/255, 400 pixels,
34 equivalents; rounded rectangle 120/255, 220 pixels, 17 equivalents.
These bounded differences do not establish broad browser equivalence. The
independent analytic gate above separately checks circle placement/coverage.
Source/build/browser/image provenance is retained with each report.

Freestanding Yonder and flowdump build with their matching libraries. Windowed
QEMU uses private copies of both root and home disks. All six cards and both
toggle directions pass; the restored dash-card pixels exactly match the
initial phase. Magnified strokes repaint at 400% zoom, then return to normal
zoom. The guest shuts down cleanly and both filesystems pass `e2fsck -fn`.

Chris confirmed that the `32b65107` delivery passes on the P5. Repeated rapid
clicks on "Shift the dashes" produce apparent motion, including at 400% page
zoom. This exercises script-driven dash-offset mutation and repaint on real
hardware; it does not measure frame rate or validate timed SVG animation.

Guest captures: [gallery](../../svg-evidence/yonder-strokes-qemu.png),
[shifted phase](../../svg-evidence/yonder-strokes-shifted-qemu.png),
[curves and seams](../../svg-evidence/yonder-strokes-curves-qemu.png),
[overlapping dots](../../svg-evidence/yonder-strokes-overlap-qemu.png), and
[400% zoom](../../svg-evidence/yonder-strokes-zoom-qemu.png).
The [browser report](../../svg-evidence/stroke-results.json) records each
comparison and its provenance.

## Reproduction

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-strokes-core
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-strokes-paint
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-strokes-yonder
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_geometry.py --backend --output /tmp/svg-strokes-backend
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
# Pillow/Playwright environment: SVG_REFTESTS.md.
python3 tools/svg_reftests/strokes.py --native /tmp/svg-strokes-yonder --output /tmp/svg-strokes-browser
CCACHE_DISABLE=1 make -C userland -j4 "$PWD/userland/bin/yonder" "$PWD/userland/bin/tests/flowdump"
make stage-pages
```

Install Yonder, flowdump and their matching shared libraries as one cohort.
Enable Modern Web, open `/tests/pages/svg-strokes.html`, and enable scripts
for the toggle. Check both phases, scroll through all six cards, and zoom.
