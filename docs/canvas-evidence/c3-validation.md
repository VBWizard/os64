# Canvas C3 validation

C3 adds current paths, transforms, strokes/dashes, immutable saved clips and
Porter–Duff compositing. It extracts SVG's dash cutter into libdraw2d and
uses the production FreeType geometry backend from both callers. The gallery
is `tools/pages/canvas-paths.html`; enable Scripts and Modern Web.
P5 validation is pending. `c3-bundle.sha256` identifies the delivery cohort.

## Automated checks

- Native Canvas: 321,611 assertions, zero failures under ASan/UBSan/LSan.
  This includes the 166,244 existing core assertions, independent floating-point
  composite factors, mixed-transform paths, clipping/save/restore, zero-size
  stroke rectangles, arc direction, empty curves and refusal preservation.
  Stroke allocation sweeps exercise 36 cut points in each refusal mode; clip
  failures preserve both the active mask and a saved reference.
- Full DOM: 20,590 target-engine checks and 132,263 sanitized-engine checks,
  zero failures. Binding allocation sweeps: 2,192 and 20,322 respectively.
  New cases cover synchronous resize during conversions, transform dictionary
  aliases, per-item dash conversion order, iterator closing, state and pixels.
- Scripted Yonder: 11,736 checks, zero failures, including retained presentation
  and allocation/cancellation sweeps.
- The actual gallery HTML and all four click handlers run through the Yonder
  host with zero failures and no remaining live Canvas allocation.
- SVG painter: 1,835,108 assertions, zero failures, including allocation and
  cancellation sweeps. All 27 before/after PPM/RGBA fixture files are
  byte-identical to the C2 baseline; sharing the dash cutter changes no fixture.
- Cross build of Yonder and its shared libraries succeeds with warnings as errors.
- Leak detection remains enabled in sanitizer runs outside the restricted sandbox.

Reproduction entry points are `tools/test_svg_core.py --canvas --output <dir>`,
`tools/test_dom_host.sh`, `tools/test_yonder_scripts_host.sh`,
`tools/test_canvas_gallery.sh <dir>` and
`tools/test_svg_core.py --paint --output <dir>`.

## Upstream census

WPT pin: `a699fb16767be39ba3e61a8e8b6bef60383f08ee`.
Expectation files retain every C2 passing subtest and add four C3 families.
These files record passes, not an assertion that the broader families conform.

| Family | Pass | Fail | Timeout |
| --- | ---: | ---: | ---: |
| transformations | 21 | 1 | 0 |
| path-objects | 117 | 86 | 0 |
| line-styles | 31 | 2 | 0 |
| compositing | 51 | 47 | 0 |
| drawing-rectangles-to-the-canvas | 30 | 2 | 0 |
| pixel-manipulation | 65 | 6 | 0 |
| the-canvas-state | 53 | 15 | 0 |
| reset | 11 | 18 | 0 |
| canvas-context | 11 | 0 | 0 |
| canvas-host | 57 | 9 | 0 |
| fill-and-stroke-styles | 135 | 121 | 1 |
| **Total** | **582** | **307** | **1** |

Eleven required cases in the four new families also fail with Modern Web off,
establishing that they depend on the implementation. The remaining broad
failures include image/gradient/text/shadow APIs, Path2D, hit testing, roundRect,
DOMMatrix/getTransform, advanced blend modes and creation-profile differences.
The pattern animation timeout awaits the image/pattern path. The final census
preceded a narrow correction to ignore an invalid odd dash list before applying
its normalized-length limit; the final native and binding suites include that
source version.

Two implemented-path limitations are distinct from missing APIs:

- Wide offset contours can intersect and fill incorrectly in the FreeType
  stroker. `2d.path.rect.selfintersect`, `2d.path.arc.selfintersect.2` and
  `2d.line.cross` expose the defect.
- Extreme coordinates/transforms reach the backend's bounded precision/work
  envelope and refuse without changing retained pixels. The large-scale,
  large-curve and some skew/miter cases expose this limit.

Both are tracked in DEBTS.md; C3 does not claim complete path conformance.

## Independent and interactive checks

Chromium 140.0.7339.16 renders the same gallery without script errors. Comparing
its initial 400×180 RGBA bitmaps to the native gallery gives these per-channel
absolute differences after conversion to premultiplied 8-bit channels with
nearest-integer rounding (ties to even). Raw measurements are in
`c3-gallery-comparison.json`:

| Card | Mean difference | Maximum difference |
| --- | ---: | ---: |
| Dashed route | 0.252 | 97 |
| Transformed petals | 0.261 | 71 |
| Even-odd stencil | 0.127 | 68 |
| Scratch coating | 0.158 | 3 |

The images agree visually; the path-edge rasterization differs. These are
measured comparisons, not exact browser-equivalence assertions. Native literal
pixel and compositing-oracle tests provide separate correctness checks.

The delivery contains Yonder, 21 matching shared libraries and the gallery:
23 files, with 16 unchanged binaries reused by allocated ELF-section comparison
against C2. Guest-installed files were read back and compared byte for byte.
QEMU checks exercise all four buttons, six scratch strokes revealing HTML below
the canvas, and the seventh click recoating it. The displayed coating after
recoat matches its original pixels exactly. The complete page remains visible
at 25%; the enlarged route also paints in the scrolled 400% viewport.
