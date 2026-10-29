# SVG clip-path silhouettes

2026-10-27. This increment follows merged viewport PR #376 on
`codex/svg-clip-paths`. Its review target is `codex/svg-integration`. The gallery
is `/tests/pages/svg-clips.html`, **SVG: cut to shape**.

## The supported cut

A fragment `clip-path: url(#id)` resolves a same-document SVG clipPath resource.
This profile admits one visible direct path or basic-shape child, in
userSpaceOnUse coordinates; [object-coordinate clips](SVG_BOUNDS.md) add
objectBoundingBox units and geometric bounds. A path can have multiple contours, curves and
holes. The resource and child transforms compose; the child's inherited
clip-rule selects nonzero or even-odd filling. Resource styling follows its
own DOM ancestry, not the referencing element's paint inheritance. CSS
clip-path and clip-rule participate in the cascade, including presentation
attributes, CSS-wide keywords and CSS precedence.

Clip resources are not drawn directly. Display on the resource or its
ancestors does not prevent referencing it; display/visibility on a child
controls whether that child contributes. Fill, stroke, opacity and color-space
paint values on clip geometry do not affect the silhouette, including values
unsupported for ordinary painting. An empty resource clips everything away,
except for ignored zero-extent object-box effects described in [SVG_BOUNDS.md](SVG_BOUNDS.md).
A missing or wrong-kind target leaves the drawing unclipped. The source
lookup shares use's bounded document-order ID lookup, percent-decoded
fragments, first-ID behavior and conservative base-URL restriction.

Clips can apply to groups, shapes, paths and use instances. Independent clips
on ordinary ancestors intersect through their isolated layers. A nested SVG
can have both its rectangular overflow clip and a silhouette clip; the latter
uses its descendant viewBox coordinate system. Editing shared clip geometry
repaints consumers, including consumers in another inline SVG root.

The source rules follow [CSS Masking's clipPath definition](https://www.w3.org/TR/css-masking-1/#the-clippath-element)
and [clip-rule](https://www.w3.org/TR/css-masking-1/#the-clip-rule).

## Native ownership and painting

The optional provider clip callback returns a borrowed path/shape descriptor,
transform and winding rule. Preparation invokes it after copying the ordinary
geometry, copies the silhouette into scene ownership, and retains it with the
scene's pinned snapshot. Release frees both ordinary and clip paths. Provider
failures, invalid maps, parse/allocation failures and cancellation unwind the
same ownership path. An active empty silhouette is distinct from no clip.

The painter builds a separate outline and bounds for each silhouette. Frame
and silhouette masks have separate maps and rules. Clip bounds intersect the
bottom-up ink bounds, then propagate down before descendant writes. A clipped
shape isolates fill and stroke together; a clipped group isolates its children.
The mask multiplies premultiplied channels before element/group opacity.
Nested masks multiply at their respective layer boundaries. Geometry quotas,
coverage allocations, raster work and cancellation include the masks.
The native provider/scene layout changes require matching consumers/libraries.

## Deliberate limits

Multiple visible clip children need a geometric union, not alpha addition or
maximum coverage at shared edges. They return SVG_UNSUPPORTED in this cut.
So do use/text/group children, clip-path references
on the resource or its child, and resource geometry percentages. Percentage
bases need a resource-viewport contract distinct from ordinary instance
percentages; an initial consumer-viewport implementation disagreed with
Chromium for a source outside the consumer's nested viewport and was removed.
Numeric and pixel geometry remain supported; relative/calc geometry is outside
the existing adapter profile. Unsupported CSS basic-shape clip syntax follows
the parser's skipped-style diagnostics. HTML box clipping is not implemented
by this SVG adapter. Masks, paint servers and resource fetching remain separate.

This is the first arbitrary-silhouette increment, not completion of general
SVG clipping. The union and resource-coordinate work stays in
DEBTS.md. It does not advance the external-image milestone: website SVG files
through `<img>` remain stage 6c, the image-route checkpoint.

## Verification

Native checks compare a holed group clip and a clipped child against exact
pixel bounds and a nonzero-origin crop, including untouched target padding.
They fail every preparation/render allocation in both refusal modes and every
render cancellation position. Adapter cases compare clipped output with
independently flattened colored geometry: circle, polygon, path holes, rule
precedence, ignored paint, transforms, CSS precedence, ancestor intersections,
opacity, empty/missing/wrong targets, hidden resources/children, nested viewBox
maps, use instances, clipped strokes and source mutation. Explicit refusals
cover multi-child union, coordinate units, percentages, resource-reference
cycles, use children, external URLs and base-changing documents.

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-clips-core
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-clips-paint
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-clips-yonder
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_libpage_host.sh
python3 tools/svg_reftests/clips.py --native /tmp/svg-clips-yonder --output /tmp/svg-clips-browser
```

Browser comparisons are exact except a radius-32 circle envelope (64 channel
steps, 256 pixels, 18 integrated pixel-equivalents) and a one-step half-opacity
group difference over 1,600 pixels (seven pixel-equivalents). Native clipped
and flattened references remain exact. Shifted controls use the corresponding
gates and must fail. A separate pinned semantic discrepancy records Chromium
140 ignoring a display:none clip resource and painting the full rectangle:
255 maximum, 16,000 pixels, 4,080,000 summed channel difference. The native
result follows the explicit resource-availability rule above. That discrepancy
is retained as a failed exact comparison, not reported as browser agreement.

## P5 checkpoint

Install the matching Yonder/library cohort and open `/tests/pages/svg-clips.html`
with Modern Web and scripts enabled. **Move the window** shifts the round
opening in cards 1 and 6, then restores it. Their artwork stays stationary.
Inspect the star, transparent rectangular hole, intersected circle/band and
rotated rounded silhouette. Scroll and try 25% and 400% zoom.
Chris confirmed the gallery works correctly on the P5 on 2026-10-27.


## Recorded evidence — 2026-10-27

- Native core: 155,998 checks. Painter: 1,514,235 assertions. The new clip
  fixture cuts 12 preparation and 58 render allocations in both refusal
  modes, plus 1,526 render cancellation positions; exact crop/padding pass.
- Adapter: 22 exact clip/reference pairs, 46 allocation cuts in both modes
  and source mutation pass. Existing viewport, use, stroke and shape cases pass.
- Scripted window: 8,267 checks, including one clip edit repainting two roots.
  Full libgarb and libpage suites pass; libpage reports 254,176 checks and 399
  allocation cuts. ASan, UBSan and LeakSanitizer remain enabled.
- [Pinned Chromium evidence](../../svg-evidence/clip-results.json): 66 expected
  outcomes: 43 positive comparisons, 22 rejected shifted controls and the
  explicitly retained display:none resource discrepancy described above.
- Freestanding Yonder and flowdump build successfully with the matching
  library cohort. General SVG conformance and page-scale throughput are not
  established by these selected fixtures.

- Private windowed QEMU: six cards inspected, shared opening moved and restored,
  25% and 400% zoom and restoration checked. Shutdown completed normally;
  read-only root/home filesystem checks both returned zero.
- P5: Chris confirmed the page works correctly and approved the slice for review.

Screenshots: [initial](../../svg-evidence/yonder-clips-top.png),
[moved](../../svg-evidence/yonder-clips-changed.png),
[holes and intersections](../../svg-evidence/yonder-clips-middle.png),
[rotation and shared source](../../svg-evidence/yonder-clips-bottom.png),
[400%](../../svg-evidence/yonder-clips-zoom.png),
[25%](../../svg-evidence/yonder-clips-small.png), and
[restored](../../svg-evidence/yonder-clips-restored.png).
