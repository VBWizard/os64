# SVG solid painting

Status: second 6b increment, 2026-10-26, based on merged PR #363 at
`ac734c6c`. Builds on [SVG_CORE.md](SVG_CORE.md). The separate
[Yonder connection](SVG_YONDER.md) consumes this prepared-scene painter.

## Delivered contract

`svg_scene_render` paints a prepared scene into caller-owned, aligned
`uint32_t` pixels containing straight-alpha `0xAARRGGBB`. The caller supplies
the device origin, dimensions, byte stride and capacity. Rendering replaces
the active rectangle, preserves row padding and exports zero RGB when alpha
rounds to zero. The last row needs its active bytes, not trailing padding.
On failure the caller discards the output; partial pixels must not be published.
The renderer never frees the target. Scenes remain reusable after success or
failure, and painting makes no document/style-provider calls.

The supported profile is solid fill followed by an ordinary user-space stroke,
nonzero/evenodd fill rules, resolved affine transforms, paint alpha and
fill/stroke opacity, and isolated element/group opacity. Strokes expand before
the device map: `scale(4,1)` transforms a round cap into an ellipse. Group
visibility does not suppress explicitly visible descendants. Empty paths and
malformed paths with an empty complete prefix paint nothing.

The provider must report active features it cannot represent, including dashes,
vector effects, clipping paths, resources, non-default paint order, non-normal
blending and non-sRGB color interpolation. Unsupported backend joins or
degenerate non-butt caps propagate `SVG_UNSUPPORTED`; the painter does not
silently omit their geometry. Target-rectangle cropping is implemented, but
that is not SVG `clipPath` support.

`libsvg.so` still imports only math functions from `libmath.so`. Rendering
receives the public geometry table explicitly. The table and its implementation
must outlive the render call. FreeType retains its two getters, no dynamic
dependencies and no undefined symbols. No application links libsvg yet.

## Coverage, color and isolation

Prepared path occurrences are lowered for their final map, then converted to
filled or expanded-stroke outlines. The geometry table's optional `bounds`
tail reports bounds of the actual transformed, quantized control points.
Callers check `struct_size` before reading it; this painter requires it and
returns `UNSUPPORTED` for an older table. The table prefix, revision and font
interface are unchanged.

These conservative allocation bounds are distinct from SVG geometric/object
bounds. Coverage masks and color surfaces use their intersection with the target,
plus a two-pixel raster fringe. Stroke extent is measured after expansion;
the centerline is not clipped or padded by a guessed miter radius. Group bounds
are unions of descendant ink. Even offscreen outlines undergo the backend's
whole-shape range and precision checks.

Color surfaces hold 16-bit premultiplied RGBA, eight bytes per pixel. Paints
composite source-over in sRGB. An element with opacity below one paints its
fill and stroke into a separate surface before applying opacity once. Groups
do the same for their children. Opacity-one groups share their parent's
surface. An iterative frame stack tracks ancestry, including empty groups.
The root surface is exported to straight-alpha 8-bit channels at the end.
Extra internal precision preserves a color such as `0x012091de` at alpha 1;
8-bit premultiplication would destroy much of its chroma.

The crop regression exposed a backend quantization bug: half-away-from-zero
rounding selected opposite sides of a half-grid tie when an integer clip
origin moved the coordinate across zero. Device-coordinate ties now round
toward positive infinity in both bounds and rasterization. The regression's
cropped pixels equal the corresponding full-image pixels exactly. This fixes
the tie-direction defect; it is not a promise that arbitrary floating-point
transform evaluation is invariant under all changes of origin.

## Memory, work and cancellation

SVG scratch allocations and geometry allocations go through the same owner
callbacks, each charged once. Geometry uses those callbacks directly; SVG
live/peak statistics exclude the backend subdivision. The owner ledger must
also charge the caller's output, retained snapshots and overlapping jobs.
Outlines remain live until the render finishes; masks and temporary opacity
layers are released after use. Allocation failures, cancellation, backend
errors and work refusals unwind those products without destroying the scene.

`svg_render_options_default` supplies these literal limits, including zero
when explicitly supplied by a caller:

| Quantity | Default |
|---|---:|
| Output pixels | 8,388,608 |
| Output dimension | 16,384 per axis |
| SVG render work | 134,217,728 |
| Raster pixel visits | 64,000,000 |
| Total reserved geometry work | 64,000,000 |
| Arc positional error | 1/64 device pixel per axis |
| Backend coordinate-conversion error | 1/64 device pixel per axis |

Dimensions also obey the backend's hard 65,535 per-axis limit. Raster work
charges mask footprints, layer clearing, blending, compositing and output
export. Render work covers SVG allocation/visits, lowering, raster work and
backend cancellation checkpoints; it replaces the context's preparation work
allowance for this call. The next public parse/prepare/lower operation restores
the context allowance.

The backend work limit resets per operation. To bound a whole render before
work starts, the painter counts active fill/stroke operations and gives each
backend call a quota of
`min(OS64_GEOMETRY_WORK_DEFAULT, max_geometry_work / (1 + 3 * paints))`.
This reserves creation plus build/bounds/raster for each paint. Unused quotas
are not redistributed, so uneven or large scenes can be refused even when
their actual total would fit. A dynamic remaining-budget backend API is a
possible later improvement, not an implied capability. Geometry statistics
sum available per-operation reports; failed creation without a context has
no report. Neither work counter measures CPU instructions or elapsed time.

Cancellation is cooperative, including row boundaries and backend checkpoints.
An executing FreeType call can finish before it reports cancellation. Owners
serialize context use; allocation/cancellation reentry returns `BUSY`.

## Verification and reproduction

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-paint
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --output /tmp/svg-core
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_geometry.py --backend --output /tmp/svg-geometry
CCACHE_DISABLE=1 make -C userland -j4 svg-library "$PWD/userland/bin/libfreetype.so"
PLAYWRIGHT_BROWSERS_PATH=/tmp/os64-svg-browser/browsers /tmp/os64-svg-browser/bin/python tools/svg_reftests/paint.py --native /tmp/svg-paint --output /tmp/svg-paint-browser
```

The browser environment setup is in [the comparison README](../../../tools/svg_reftests/README.md).
Use an empty browser output directory. Host tests compile production SVG,
FreeType and pinned libmath sources and record source/header/compiler/binary
provenance. ASan, UBSan and LeakSanitizer pass outside the sandbox; no
leak-disabled result is substituted for an attachment refusal.

The painting harness passes **490,392 assertions**, including exact opacity,
fill-rule, visibility, low-alpha and transparent-pixel checks, stride/capacity
and origin handling, offscreen/singular maps, allocation and work limits,
unsupported features, injected engine failure and callback reentry. It sweeps
**50 allocation failure positions in both refusal modes** and **1,240 cancellation
positions**, keeping two scenes alive and checking recovery and pin ownership.
The injected ledger peaks at **90,078 bytes** in that fixture, excluding the
stack-owned destination; this is not a page memory or performance estimate.

The original core suite passes **148,122 checks**. The backend suite passes,
including new allocation-free bounds/origin/failure tests and its existing
32 allocation/1,045 cancellation sweep. Its four saved coverage masks are
byte-identical to the prior comparison run. The freestanding libraries build
successfully with the dependency/export boundaries described above.

The [self-contained gallery](../../svg-evidence/solid-painting.html) and
[machine-readable results](../../svg-evidence/solid-painting-results.json)
record **24 accepted native/Chromium comparisons and 14 rejected negative
controls**, using Chromium 140.0.7339.16, Playwright 1.55.0, 64×64 pixels and
device scale 1. Each of twelve scenes is composited over light and dark
backgrounds. Wrong controls change group/element opacity, winding, scale,
stroke presence, shear or circle radius. No rendered text or external fetches
are involved. These are prepared-scene fixtures, not Yonder captures or WPT
coverage claims.

Rectangular opacity/low-alpha comparisons allow one channel unit; winding and
visible-child cases are exact. Curved/diagonal fixtures use separate edge
envelopes, without exempting edge pixels:

| Fixture | Maximum channel delta | Changed pixels | Sum of max-channel deltas / 255 |
|---|---:|---:|---:|
| Anisotropically scaled round cap | 40 | 200 | 12 |
| Miter join and square caps | 24 | 250 | 4 |
| Sheared/clipped filled and stroked rectangle | 24 | 400 | 6 |
| Arc circle | 64 | 200 | 12 |

The provisional miter/shear maximum of 8 and circle maximum of 40 failed.
Independent 64×64 subpixel oracles use a six-vertex miter polygon, inverse-map
rectangle/ring coverage and an analytic circle, rather than the painter's
outlines. Native alpha maximum/L1 differences are respectively **2/255,
1.031373**, **2/255, 0.458824**, and **32/255, 8.164706**. On the light
background, native/browser RGB maxima against those oracles are respectively
**2/23**, **2/23**, and **27/42**; native-versus-browser maxima are **24**, **21**
and **54**. The chosen envelopes retain these measured edge differences;
deliberately wrong geometry still fails. Existing backend/browser fixture
thresholds are unchanged. The circle's cubic-raster coverage limitation remains
recorded in SVG_CORE.md; tightening positional tolerance alone does not solve it.

## Following work

The [Yonder connection](SVG_YONDER.md) adds the real tree/style adapter, computed
paint properties, outer sizing and inline painting. Continue 6b with shapes, dashes,
degenerate-cap/join handling, use/symbol, nested viewports and SVG clipping.
Keep the Yonder connection in its own PR, inspect concurrent browser changes
before touching shared files, and stack dependent follow-ons while it is
unmerged, as recorded in SVG_CORE.md.

The first useful P5 checkpoint is a real inline fixture reaching Yonder's
paint path. SVG_YONDER.md records that checkpoint; this library increment
alone does not reach it. External-image policy,
gradients, caches, text, animation and broader conformance/performance gates
retain their places in [SVG.md](SVG.md) and DEBTS.md.
