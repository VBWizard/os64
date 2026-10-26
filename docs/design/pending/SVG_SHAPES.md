# SVG basic shapes in Yonder

2026-10-26. This 6b increment extends the [inline connection](SVG_YONDER.md)
with `rect`, `circle`, `ellipse`, `line`, `polyline` and `polygon`. It builds on the merged connection PR #368 and form-selection PR #367.
The visible checkpoint is
`tools/pages/svg-shapes.html`, installed as `/tests/pages/svg-shapes.html`.

## Geometry and ownership

The provider has an optional `shape` callback, required for `SVG_NODE_SHAPE`.
It runs after the element's displayed style has been resolved. The callback
returns a typed `svg_shape_t` with resolved user coordinates; point-list bytes
are borrowed until the next provider callback. Native `libsvg` constructs an
owned path directly, without formatting and reparsing decimal path data.
Scene-array growth refreshes the callback's borrowed parent-style pointer.

`svg_path_from_shape` exposes the same conversion to non-browser callers.
Rectangles become lines and, for rounded corners, endpoint arcs. Ellipses
(and circles with equal radii) become four clockwise quarter arcs. Ordinary
lines remain open; polygons close, while polylines keep their stroke open.
Fill implicitly closes an open outline through the existing painter. A line
has no fill, including when its inherited fill names an unsupported paint
server. Zero width, height or ellipse radius produces an empty path.

Rectangle radii clamp independently to half the width and height. If either
used radius is zero, corners are square. The adapter resolves `auto` by copying
the other used radius, or zero when both are auto; this applies to ellipses
as well as rectangles. Paths retain user coordinates until the existing arc
lowering, stroking, transforms and compositing stages consume them.

Point lists share the bounded native decimal scanner. Complete coordinate
pairs survive a malformed suffix; a polygon closes that complete prefix.
`SVG_MALFORMED` records its offset and keeps the owned prefix, like path `d`.
Allocation, work, coordinate-overflow and cancellation failures publish no
path. The existing 1 MiB input and path-command/work limits also apply to
point lists. The adapter's 4,096-node and shared 64 MiB render limits remain.

## Cascade and units

`libgarb` adds non-inherited `x`, `y`, `cx`, `cy`, `r`, `rx` and `ry` properties
and their SVG presentation attributes. Existing width/height declarations
provide rectangle extents. Stylesheets, inline styles, importance, custom
properties and explicit inheritance use the real cascade. Geometry is read
only for the element consuming it; irrelevant ancestor dimensions do not
force a conversion or a refusal. Mutation rebuilds the same cascade and scene.

Supported geometry values are user-unit numbers, px and percentages.
Width/height presentation attributes accept unitless numbers; their CSS
syntax remains the existing length grammar. Percentage positions, extents
and axis radii use the corresponding viewport dimension; circle `r` uses
`sqrt((width² + height²)/2)`, evaluated with `hypot`. Line x1/y1/x2/y2 and
polygon/polyline points remain attributes, not invented CSS properties.
Relative units and calculated geometry are explicitly outside the adapter's
profile. Invalid negative extents/radii are discarded by property parsing;
missing/initial extents become zero and rx/ry become auto.

The [SVG2 geometry chapter](https://www.w3.org/TR/SVG2/geometry.html) specifies
viewport-relative rx/ry percentages. The rectangle algorithm in the
[shapes chapter](https://www.w3.org/TR/SVG2/shapes.html#RectElement) instead says
to resolve them against the rectangle size. This implementation follows the
geometry chapter and Chromium: a 40-unit rectangle in a 200-unit-wide
viewport with rx=10% uses a 20-unit horizontal radius. This choice is tested.

## Evidence

The solid-painter suite passes **490,392 assertions**.
The native suite passes **148,938 checks**, including shape/path command
agreement, radius clamps, zero extents, malformed point prefixes, overflow,
allocation and cancellation cuts, missing provider callbacks, image-mode
preparation and the parent-style lifetime across scene-array growth.

The real HTML/cascade/layout adapter renders **19 shape/path pairs with exact
pixel equality**. Cases include percentages, rounded and zero-radius corners,
CSS overrides and custom properties, explicit inheritance versus ordinary
non-inheritance, open/closed strokes, malformed point prefixes, nonuniform
transforms and group opacity. Its three-shape fixture passes **60 allocation
cuts in both refusal modes**, with zero live SVG bytes after each failure,
and geometry mutation changes the next image. The connection's original
81-position failure sweep also passes. ASan, UBSan and LeakSanitizer are enabled.
The sandbox denied LeakSanitizer thread attachment with EPERM; leak-enabled
runs outside it pass.

The full window/script suite passes **8,243 checks**, including a script
changing a rectangle's width, x position and fill through the compositor.
After incorporating merged PR #367, the adapter and window suites pass
again with leak detection. The CSS parser/cascade regression suite and
freestanding Yonder/flowdump build pass. These are focused checks, not a broad WPT or corpus-completeness claim.

`tools/svg_reftests/shapes.py` compares those emitted fixtures with Chromium
140.0.7339.16 / Playwright 1.55.0. **57 comparisons behave as expected**:
38 shape/path references match their declared tolerances, 17 shifted controls
fail, and two empty fixtures remain empty after shifting. Integral straight
edges require exact agreement. Curved/diagonal edges use bounded channel,
changed-pixel and integrated-error envelopes. Radius-20 circles differ by up
to 65/255 at individual edge pixels but less than 8.2 integrated pixel
equivalents; their bound is 72/255, 160 pixels and 9 pixel equivalents.
Half-opacity compositing includes one-level interior rounding differences,
with a separate 32/255, 2,000-pixel, 11-pixel-equivalent cap. The report records
browser/build provenance and source/image hashes. These allowances document
existing backend coverage differences; native shapes still match native paths
exactly.

Windowed QEMU checks before and after incorporating PR #367 use private
copies of both disks and display all six cards. The button changes the rectangle from narrow blue to wide orange and
back to narrow blue. The [gallery](../../svg-evidence/yonder-shapes-gallery-qemu.png),
[shape cards](../../svg-evidence/yonder-shapes-qemu.png),
[expanded state](../../svg-evidence/yonder-shapes-expanded-qemu.png) and
[returned state](../../svg-evidence/yonder-shapes-returned-qemu.png) are guest
screenshots. The guest shuts down cleanly and both private filesystems pass
`e2fsck -fn`.

Chris verified the `0a5f559f` delivery on the P5: the gallery holds up at
400% page zoom, and he reported the checkpoint passed. Two green shapes
appeared blue through TigerVNC while appearing green on the P5's own screen.
That observation records a difference between the local and remote views;
its cause has not been diagnosed. No SVG color adjustment was made.

## Reproduction and next boundary

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-shapes-core
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-shapes-yonder
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
# Use the isolated Pillow/Playwright environment described in SVG_REFTESTS.md.
python3 tools/svg_reftests/shapes.py --native /tmp/svg-shapes-yonder --output /tmp/svg-shapes-browser
CCACHE_DISABLE=1 make -C userland -j4 "$PWD/userland/bin/yonder" "$PWD/userland/bin/tests/flowdump"
make stage-pages
```

Install Yonder, flowdump and the matching shared libraries together. Enable
Modern Web and open `/tests/pages/svg-shapes.html`; enable scripts for the
button. The button is a toggle. Scroll and change zoom to check repaint.

Dashes, use/symbol, nested viewports, clipping and the remaining static SVG
features stay in [DEBTS.md](../../../DEBTS.md). Relative/calc geometry, SVG
animated-length and point-list DOM objects, hit testing and pathLength-based
stroke calibration are not added here. The native painter's degenerate-stroke
and curved-edge coverage limits still apply. External SVG image decoding is
still a separate integration milestone.
