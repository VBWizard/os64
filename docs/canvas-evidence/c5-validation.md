# Canvas C5 validation

C5 adds live linear/radial/conic gradients, filled and stroked outline text,
font/alignment/baseline state, and numeric TextMetrics. The gallery is
`tools/pages/canvas-paints.html`, “A Few Choice Words”; enable Scripts and
Modern Web. P5 acceptance is complete: all four samples passed at all four
tested zoom levels. This is the bounded C5 profile, not complete Canvas or
text conformance.

## Shared implementation

The numeric gradient programs moved from SVG to libdraw2d, with SVG's public
adapters and its tangent-cone policy retained. All 27 recorded SVG images are
byte-identical to the C4 baseline. Canvas uses the shared stop interpolation,
root solving, coverage and compositor, with Canvas transform timing and cone
rules.

The font backend exposes an optional, size-guarded outline visitor. The shared
text layer supplies positioned contours and fallback through
`os64_text_run_outline`; Canvas consumes them without replacing its current
path. The original raster-glyph contract and Western text profile remain.
An actual prefix-sized old backend table is tested under ASan, including
bitmap outline fallback and explicit refusal of unavailable face outlines.

## Automated checks

- Native Canvas: 414,512 assertions, zero failures. New cases cover live stops,
  references held by saved states and other contexts, duplicate stops, empty
  gradients, radial cone transparency, conic angles, independent contours,
  text state, and allocation refusal without partial painting.
- SVG painter: 1,835,108 assertions, zero failures, plus the 27 exact fixture
  comparisons after the completed shared-code changes.
- Shared text: 10,300 checks, zero failures. Real TrueType and CFF fixtures
  cover contour closure, tight bounds, retained-run lifetime, command limits,
  every visitor refusal boundary, and bitmap/old-table fallback.
- Font backend: 1,089 checks, zero failures. Font-to-PSF2 consumer: 567,092
  checks, zero failures with real font fixtures.
- Focused Canvas bindings: 129 checks in each JavaScript engine profile,
  zero failures. Real outlines exercise metric alignment, transforms,
  whitespace, maxWidth, path preservation, reset and reentrant conversions.
- Full DOM: 20,542 target-engine checks and 133,673 sanitized-engine checks,
  zero failures. Binding allocation sweeps cover 2,180 and 20,554 points.
- Final Yonder script suite: 11,747 checks, zero failures, including existing
  SVG worker allocation and cancellation sweeps.
- Final gallery and all four handlers execute through Yonder's host with the
  real font backend. Every handler changes its intended bitmap; teardown
  releases the native allocations. Captures preserve premultiplied channels.
- The target Yonder and its matching libraries cross-build with warnings as
  errors. Sanitizer runs retain ASan, UBSan and leak detection.

The browser checks exposed missing gradient-owner initialization, detached
canvas relative-size defaults, and escaped font-family serialization. The
full DOM check also caught the new globals missing from the Modern Web
registration table. These were corrected before delivery.

Reproduction: `tools/test_svg_core.py --canvas --output <dir>`,
`tools/test_svg_core.py --paint --output <dir>`, `tools/test_dom_host.sh`,
`python3 tools/test_text_host.py --real --output <dir>`,
`python3 tools/test_freetype_host.py`,
`python3 tools/test_font_psf2_host.py --real --output <dir>`,
`tools/test_yonder_scripts_host.sh`, and
`tools/test_canvas_gallery.sh <dir> tools/pages/canvas-paints.html`.

## Upstream and browser evidence

WPT pin: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`. The exercised prior
expectations lose no passes. Required gradient and text-path cases fail with
Modern Web off, as the gate requires.

| Family | Pass | Fail | Timeout |
| --- | ---: | ---: | ---: |
| fill-and-stroke-styles | 188 | 68 | 1 |
| text | 35 | 153 | 0 |
| the-canvas-state | 59 | 9 | 0 |
| reset | 17 | 12 | 0 |

Fill/stroke styles gain 52 passes. The remaining gradient failures require
unsupported color-mix or relative-color parsing. Most other style failures
require patterns or additional color profiles. Text results include state,
font parsing, relative sizes, direction/alignment measurements and independent
path behavior. Many text drawing tests cannot reach their assertions because
CSS Font Loading (`document.fonts.ready`/FontFace) is absent; the real-font native
and binding tests provide separate drawing evidence. Additional styling,
complex shaping and extended metrics methods remain outside the profile.

Chromium 140.0.7339.16 executes the gallery without script errors. The shared
linear panels differ by at most one premultiplied channel unit. The sky/curve
sample's maximum difference is 16, mean 0.185 channel units across its pixels,
including different rasterizer edge coverage. Font appearance is not compared
byte-for-byte across platform font selection and hinting.

Visible QEMU runs exercise all four buttons, text fill/stroke/fitting, and
25%, 33%, 75%, 100% and 400% presentation. The final delivery cohort receives
an additional 25% smoke check. Canvas retains bitmap pixels when browser zoom
changes; this does not promise fresh vector rasterization at each zoom.

## Review regressions

The first review identified two reproduced defects: explicit positive infinity
suppressed text drawing, and Canvas font lookup outside layout omitted loaded
page faces. Both new regression cases fail against the submitted code.

Text preparation now rejects NaN and nonpositive maximum widths while allowing
positive infinity, following the [HTML text preparation algorithm](https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm).
Pixel comparisons cover `fillText` and `strokeText` against an omitted width;
invalid widths and non-finite coordinates remain no-ops.
The focused Canvas DOM suite passes 129 checks in each JavaScript engine
profile with ASan, UBSan and leak detection enabled.

Font resolution uses the owning page's retained cascade and stylesheet mapping
between layouts. The production-resolver regression compares the Canvas run's
font identity to ordinary DOM text after a page face loads, checks isolation
from an unrelated document, and checks that layout diagnostics are preserved.
The full Yonder suite passes 11,752 checks with ASan, UBSan and leak detection;
the target Yonder and updated libdom cross-build also pass.

## Limits

The shared Western text profile supplies pair positioning and fallback, not
full bidi or complex-script shaping. Direction affects start/end alignment;
HTML dir inheritance is supported. Non-alphabetic baselines are synthesized
from metrics. Viewport font-size units, font loading promises, small caps,
variation/stretch, letter/word spacing and newer color syntax remain absent.

TextMetrics uses collectible engine storage. Gradient wrappers remain in the
binding registry until teardown, bounded by its record quota and the Canvas
owner ledger. Repeated construction can reach that quota; collectible gradient
wrappers are booked before high-churn animation workloads. Native saved-state
references release on restore/reset/destruction. Text input is limited to
16 KiB, font size to 1280px, outlines to 4096 commands per draw, and gradients
to 256 stops. These limitations and the remaining Canvas features are tracked
in DEBTS.md.
