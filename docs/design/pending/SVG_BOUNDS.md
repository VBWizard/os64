# SVG geometric bounds and object-coordinate clips

2026-10-27. This increment follows merged clip-path PR #378 on
`codex/svg-integration`. Its branch is `codex/svg-object-bounds`.
The gallery is `/tests/pages/svg-bounds.html`, **SVG: made to measure**.

## Behavior

A single-silhouette clip resource can use `clipPathUnits="objectBoundingBox"`.
Numeric coordinates map the unit square to the referencing drawing's geometric
bounds. Changing a drawing's width changes its fitted clip without changing
the resource. Groups use the geometry of their retained descendants; use
instances and nested viewports participate in that calculation.

Bounds exclude strokes, paint, visibility, opacity and clipping. A hidden,
transparent or unpainted path still contributes; display:none subtrees and
zero-sized viewports have already been omitted during preparation. Curves
contribute their extrema, not off-curve control points. Descendant curves are
measured in the requested coordinate space, rather than transforming their
previously computed axis-aligned boxes. These rules follow
[SVG2 bounding boxes](https://www.w3.org/TR/SVG2/coords.html#BoundingBoxes).

The resource's transform acts in consumer user coordinates, outside the box
mapping; the child's transform acts inside it. The composition is:

```
consumer-to-device * resource-transform * box-map * child-transform
box-map = translate(min-x, min-y) * scale(max-x - min-x, max-y - min-y)
```

The consumer map includes its own viewport content map when applicable.
[Chromium's clip transform implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/layout/svg/layout_svg_resource_clipper.cc)
and a browser fixture exercise the resource/child distinction. The native
provider exposes the outer and inner maps separately for object-box clips.
With userSpaceOnUse, the adapter can still compose them directly.

Fractions outside [0,1] are supported. Resource geometry percentages remain
explicitly unsupported in both coordinate modes: percentage content follows
viewport rules, and is not shorthand for normalized fractions. For zero-width
or zero-height geometry, this profile ignores the object-box effect, following
[SVG2 object bounding box units](https://www.w3.org/TR/SVG2/coords.html#ObjectBoundingBoxUnits).
A nondegenerate drawing with an empty clip resource is fully clipped.

## Native interface and limits

`svg_path_bounds` measures a path under a caller-supplied affine map.
`svg_scene_object_bounds` measures retained paths in a subtree in that item's
content user space, excluding its own placement and viewBox map. The latter
recomposes descendant maps without inversion, so singular maps remain usable.
Empty geometry is distinct from a zero-extent point. Moves and zero-length
path segments contribute points; empty paths and containers add nothing.
This is a geometry query, not the DOM getBBox API: it does not recover omitted
DOM elements or the specified position of an empty shape/unresolved use.

Quadratic and cubic derivative roots and transformed ellipse-axis extrema are
evaluated in binary64. Endpoint arc conversion is shared with arc lowering.
These are geometric bounds, not certified outward-rounded raster allocation
bounds; the backend's conservative bounds remain responsible for allocations.
Floating-point precision and the endpoint conversion's numerical conditioning
still apply. No curve tessellation tolerance or control-point padding silently
changes the object coordinate system. Non-finite arithmetic fails with LIMIT.

Both queries allocate nothing and use the context's work/cancellation controls.
The scene query costs descendant visits, ancestor-map composition and path
commands; repeated clipped ancestors can revisit geometry, bounded by the
render work limit. Bounds are computed when an object-coordinate clip needs
them, without adding a preparation pass for scenes that do not use them.
Failure leaves public output empty. The renderer retains its existing
allocation, raster, cancellation and partial-output ownership contracts.
Matching Yonder/libsvg binaries are required because the provider/scene clip
layout changes. Multiple-child clip unions, resource percentages, use/text
clip children, nested resource clips and paint servers remain separate work.

## Shared consumers through 6c

The path/bounds interfaces require no DOM, stylesheet or markup serialization.
They support the geometry needed by gradient coordinates and a future Canvas
2D caller. Gradient evaluation should similarly consume resolved geometry,
stops, spread mode and transforms; SVG reference/units resolution stays in
its adapter. Canvas retains its own transform timing and drawing-state rules.

Canvas text should call shared font selection/shaping, without depending on
SVG text markup or waiting for 6d. SVG motion in 6e must use Yonder's frame
loop alongside canvas; it must not create a separate animation clock. Canvas
also needs persistent bitmap/state, resizing, pixel access and resource-origin
rules. Those browser semantics are not supplied merely by sharing painting.
No canvas API or scheduling change is implemented here.

The next external-image milestones remain standalone XML scene adaptation and
**the image route**, then gradients/caches and corpus/accounting gates within
6c. This increment does not enable website SVG files through `<img>`.

## Evidence

- Core analytic bounds and cancellation cases: 156,298 checks. Cases include
  both cubic extrema, sheared quadratics/ellipses, arc direction and radius
  correction, zero-length geometry, singular maps and arithmetic overflow.
- Native painter: 1,679,637 assertions. Normalized clips repeat the exact pixel,
  crop/padding, 12 preparation and 58 render allocation-cut sweeps in both
  refusal modes, and 1,547 cancellation cuts. Scene queries check coordinate
  spaces, subtree termination, paint-independent bounds and cancellation.
- Yonder adapter: 20 exact object-coordinate versus explicit user-coordinate
  references, 46 allocation cuts in both modes, and width-mutation repaint.
  Prior clip, viewport, use, stroke, shape and connection regressions pass.
- Window/script harness: 8,267 checks. Its source list now reads production
  libsvg membership, preventing new geometry modules from being omitted.
- Native suites run with ASan, UBSan and LeakSanitizer enabled. Freestanding
  Yonder/flowdump and their matching library cohort build successfully.
- Pinned Chromium 140.0.7339.16: 60 expected comparison outcomes: 38 matching
  positives, 20 rejected shifted controls, and two pinned semantic differences.
  Cases 3/4/18 have measured edge envelopes bounded by maximum channel error,
  differing pixels and total error, not an unrestricted fuzzy pass.

The two browser differences are explicit: Chromium transforms the sheared
quadratic's local box into a width of 30, whereas the curve's tight width is
22.5; and it hides the zero-height stroked line under an object-coordinate
clip, whereas SVG2 specifies ignoring that effect. The native results match
independent explicit-geometry references. These cases are not reported as
browser agreement. See [bounds-results.json](../../svg-evidence/bounds-results.json).

Windowed QEMU verified six cards, the width toggle in both directions, 25% and
400% zoom, and restoration to 100%. The frozen 28-file payload was installed
byte-for-byte into private guest disks. Screenshots are in `docs/svg-evidence/`
with the `yonder-bounds-` prefix. Orderly shutdown and read-only checks of both
filesystems passed. Chris built this worktree, installed it on the P5, and
confirmed that the gallery passed on 2026-10-27; approved for review.

Reproduce with `tools/test_svg_core.py` (core, `--paint`, `--yonder`),
`tools/test_yonder_scripts_host.sh`, and `tools/svg_reftests/bounds.py` pointed
at the native Yonder fixture output. Browser environments and provenance use
the existing SVG reftest setup. Keep leak checks enabled.
