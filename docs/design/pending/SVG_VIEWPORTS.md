# SVG referenced and nested viewports

2026-10-27. This 6b increment follows [same-document instances](SVG_INSTANCES.md).
Its branch is `codex/svg-viewports`, based on integration merge `f7e6ef78`
(PR #373), and its review target is `codex/svg-integration`. The hardware
checkpoint is `/tests/pages/svg-viewports.html`, **SVG: a room of its own**.

## The frame and its contents

`svg_style_t.viewport` describes an optional container viewport: finite,
nonnegative width/height, a descendant content transform, and a rectangular
clip flag. Zero dimensions suppress the subtree. Scene preparation rejects
viewport fields on path/shape nodes. `user_to_viewport` positions the frame;
the content transform is composed into the map inherited by descendants.
Keeping those maps distinct prevents viewBox scaling from moving the clip
rectangle. The public style structure grows; consumers and shared libraries
must be rebuilt and installed together.

The painter builds a FreeType outline for the frame rectangle with the frame
map, independently of the children's maps. Rectangular bounds reduce layer
allocation, but the rasterized rectangle supplies the actual clipping coverage.
A rotated or sheared viewport therefore clips to its transformed rectangle,
not to its axis-aligned bounding box. Clip bounds are intersected bottom-up;
a subsequent top-down pass restricts descendant layer and ink bounds before
pixel writes. This also makes cropped render targets safe.

A clipped container gets an isolated layer even at opacity one. Its children
paint into that layer, then the clip coverage multiplies its premultiplied
channels once, before group opacity and parent compositing. Nested viewports
apply their masks as their layers close. Geometry creation/build/bounds/raster
quotas include the masks; outline, layer and temporary coverage allocations
share the caller's ledger. Raster work and cancellation checks cover mask
application and cleanup. These are rectangular viewport clips, not a general
clipPath or mask implementation.

## Yonder geometry and inheritance

Nested SVG elements and directly referenced SVG/symbol elements become
viewport containers. An ordinary symbol remains non-rendered, including
inside a referenced group. A use's non-auto width/height override its target's
dimensions. Otherwise the target's dimensions apply, with auto falling back
to 100% of the enclosing viewport. Use x/y and the target's own x/y retain
their respective placement transforms. Width/height on a use of a plain
shape or group still do not resize it.

The shared viewBox mapper handles nonzero origins, the nine alignments,
meet/slice and none. A zero viewBox dimension suppresses drawing. Instance
records retain each container's descendant percentage basis: viewBox width
and height when present, viewport user dimensions otherwise. Shapes, use
placement, stroke widths and dash lengths resolve against that basis; sibling
instances do not inherit another instance's viewport dimensions. Root box
sizing, zoom and the flow-owned root transform remain at the existing boundary.

`libgarb` includes the overflow presentation attribute in its SVG cascade.
Nested SVG and symbol viewports default to hidden overflow. Visible removes
the clip; hidden and clip retain it. CSS overrides the presentation attribute,
and explicit inheritance uses instance ancestry. The visible/hidden axis pair
normalizes to clipping on both axes. Explicit auto/scroll and a mixed
clip/visible pair are refused: scrolling and an unbounded one-axis strip mask
are outside this static profile.

[Clip silhouettes](SVG_CLIPS.md) add one direct path/shape clip resource.
Symbol refX/refY, broader clip combinations/masks, relative/calc viewport dimensions,
per-instance custom-property substitution and the other resource/paint-server
features remain in [DEBTS.md](../../../DEBTS.md). The existing same-document
reference, base-URL, selector-matching, work and ownership boundaries apply.

The design follows SVG2's [viewport transformation](https://www.w3.org/TR/SVG2/coords.html#ComputingAViewportsTransform),
[percentage units](https://www.w3.org/TR/SVG2/coords.html#Units),
[use layout](https://www.w3.org/TR/SVG2/struct.html#UseLayout), and
[overflow rules](https://www.w3.org/TR/SVG2/render.html#OverflowAndClipProperties).

## Verification

The native mask fixture checks nested clips against exact pixel bounds,
cropped origins against the corresponding full render, zero viewports,
invalid dimensions/maps, and each allocation/cancellation failure. Adapter
fixtures compare referenced and nested viewports with explicit flattened
references. They cover sizing overrides, meet/slice/none, viewBox origins,
percentages, transformed clips, isolated opacity, overflow precedence and
inheritance, hidden symbols, zero dimensions and live use-width mutation.
The window harness checks that resizing a symbol moves its clipping edge too.

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-viewports-core
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-viewports-paint
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-viewports-yonder
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
python3 tools/svg_reftests/viewports.py --native /tmp/svg-viewports-yonder --output /tmp/svg-viewports-browser
```

Native viewport/reference pairs require exact bytes. Browser comparisons use
exact pixels except three recorded raster envelopes: one coverage step on
32 diagonal-edge pixels; a fractional-width stroke bounded by 32 channel steps,
80 affected pixels and eight pixel-equivalents of integrated error; and the
rotated overlapping-child opacity fixture. In that last fixture Chromium
shows a red contribution on the clip edge and one-step interior rounding,
while the native result exactly matches the flattened blue reference. The
actual/shifted comparison is bounded by 40 channel steps, 1,100 affected pixels
and 18 pixel-equivalents; the flattened reference still requires exact pixels.
Shifted negative controls must fail the corresponding gates. These selected
envelopes do not establish general browser equivalence or page-scale speed.

Case 27 records a separate semantic disagreement: Chromium 140 reports a
computed use width of 40px but renders the attribute width of 20. SVG2 use
layout specifies the computed property. Native pixels exactly match the
40-unit reference. The browser runner retains the failing exact comparison
and pins its observed discrepancy (255 maximum, 400 pixels, 102,000 summed
channel difference); it is not counted as browser agreement.

## P5 checkpoint

Install Yonder, flowdump and the matching libraries together. Open
`/tests/pages/svg-viewports.html` with Modern Web enabled; enable scripts for
the button. **Make room** widens and restores the first symbol's frame. Its
circle stays round and centered, and its pale background stops at the border.
The other cards demonstrate fit/fill cropping, deliberate stretching, nested
windows, a rotated clipping frame and visible overflow. Scroll, change page
theme and zoom to 400% to check repaint.


## Recorded results — 2026-10-27

- Core: 155,998 checks; painter: 1,348,897 assertions. The viewport fixture
  cuts 63 allocations in both refusal modes and 922 cancellation positions,
  with exact cropped-target and padding checks.
- Adapter: 28 exact viewport references and 42 allocation cuts in both modes;
  preceding use, shape and stroke regressions also pass. Real window: 8,263
  checks. Full libgarb suite and libpage's 254,176 checks / 399 allocation cuts pass.
- Host runs retain ASan, UBSan and LeakSanitizer outside the restricted sandbox.
  Freestanding Yonder and flowdump builds pass.
- [Chromium evidence](../../svg-evidence/viewport-results.json): 84 expected
  outcomes, consisting of 55 positive comparisons, 28 rejected shifted controls
  and the explicitly recorded CSS use-width disagreement. The three raster
  envelopes above remain bounded; the 28 native reference pairs are exact.
- Private windowed QEMU: six cards inspected, width toggled both ways, 400%
  zoom and normal-size restoration checked. Shutdown completed normally;
  read-only root and home filesystem checks both returned zero.
- P5: Chris confirmed the gallery passes, including 400% and 25% zoom.
  Card 4 intentionally offsets the inner window to the right: its 88-unit
  width starts at x=18 inside the 112-unit pale frame, leaving margins of
  18 units left and 6 right. The reported placement matches the fixture.
  The frozen delivery contains the guest-tested Yonder, flowdump, 19 matching
  libraries and five gallery pages.

Screenshots: [initial](../../svg-evidence/yonder-viewports-top.png),
[widened](../../svg-evidence/yonder-viewports-changed.png),
[nested windows](../../svg-evidence/yonder-viewports-middle.png),
[rotation and overflow](../../svg-evidence/yonder-viewports-bottom.png),
[400%](../../svg-evidence/yonder-viewports-zoom.png), and
[restored](../../svg-evidence/yonder-viewports-restored.png).
