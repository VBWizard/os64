# Native geometry backend

Implemented 2026-10-26 as an increment of SVG slice 6a, above the
[geometry experiment](SVG_GEOMETRY.md). The versioned production interface
is in `userland/libos64/include/os64/geometry_backend.h`; the adapter is
`userland/libfreetype/port/geometry.c`. The [scene foundation](SVG_CORE.md)
and [solid painter](SVG_PAINT.md) consume it; browser integration and broader
reference gates remain separate work.

## Interface and ownership

`os64_freetype_geometry_v1()` returns a table with `revision` and `struct_size`.
Check both before using its operations. This is a second exported getter in
`libfreetype.so`; the font v1 table and getter are unchanged. No FreeType types
cross either interface. The library remains freestanding, with no DT_NEEDED,
undefined imports or libm dependency.

A context owns its FreeType library and its outlines. The caller supplies
allocation/free callbacks, a literal per-operation work allowance, and an
optional cancellation callback. Allocations include adapter headers and
FreeType storage. The callback returns LIMIT for admission refusal and
NO_MEMORY for physical exhaustion; free receives the original pointer and
size. Returned storage must be aligned for `max_align_t`. The SVG owner's
shared ledger charges these allocations directly. The backend's
live/peak counters report those charges; they do not impose another cap.
Callbacks and their state must outlive the context.

The caller serializes operations on a context and its outlines. Re-entry
through allocation, free or cancellation callbacks is rejected with BUSY
for otherwise valid operations. This guard is not a cross-thread lock.
Outlines cannot move between contexts. Destroy returns BUSY while any
outline lives. Release and destroy do not allocate or poll cancellation,
so an operation's cancellation or exhausted work allowance cannot prevent
cleanup. An operation failure leaves the context reusable. Handles must be
live; the API does not validate stale or arbitrary pointers.

`path` copies a command stream into an owned filled outline. `stroke`
expands each subpath with its own open/closed flag before any device map.
The accepted stream starts with MOVE and contains MOVE, LINE, QUADRATIC,
CUBIC and CLOSE commands. A segment after CLOSE needs another MOVE. Empty
streams are invalid. Fill implicitly closes contours; stroke closes only
where CLOSE appears. Inputs are borrowed for the call, and an output handle
is NULL on failure. `outline_info` reports points/contours without exposing
upstream storage.

`raster` maps the complete outline into a caller-owned 8-bit coverage plane,
using a device origin, dimensions, stride, capacity and nonzero/evenodd rule.
Coordinates point right and down. It clears the active rectangle and preserves
row padding. Clipping happens during coverage generation, not by cutting the
path into pieces that would acquire new caps. Any non-OK result invalidates
the destination; cancellation or backend failure can leave partial coverage.
Singular maps require no inverse and can produce empty coverage normally.

The optional v1 table tail `bounds` encloses transformed, 26.6-quantized
control points relative to a supplied origin. It allocates nothing, charges
point visits, supports cancellation and leaves `empty` true on failure. It
uses the same precision/range checks as rasterization. Check `struct_size`
before accessing the tail; the existing table prefix and revision remain
unchanged. The solid painter uses actual expanded-stroke bounds for scratch
allocation. These are conservative raster bounds, not SVG object bounds.

## Precision and refusal

The planned map determines a translated, power-of-two working frame. Scale
starts at 256, is at least 256 times the linear map's maximum absolute row
sum, and increases further when the requested coordinate precision requires
it. Scale above 2^30 is refused. The translated control extent plus the
conservative stroke expansion must fit 1,048,576 working units. No coordinate
is independently clamped, and no filled outline is split to evade a limit.

The requested coordinate-conversion error is per device axis, greater than
1/128 and at most one pixel; 1/64 is the ordinary caller choice. The estimate
includes working-grid rounding, final 26.6 rounding, and a conservative double
arithmetic allowance that accounts for a large world origin and stroke
excursions. Rasterization checks this estimate again against the actual map.
Device half-grid ties round toward positive infinity. Half-away-from-zero
rounding was wrong for translated clips: crossing zero changed the selected
side of a tie. The solid-painting crop regression now matches full-image
pixels exactly; see SVG_PAINT.md for its evidence.
An outline prepared for one scale cannot silently lose precision when reused
at a larger scale. Large finite values can therefore return LIMIT even when
translation would place the picture near the viewport.

This is a coordinate-conversion allowance, not an end-to-end bound on the
stroked boundary or pixel coverage. Stroke radius rounds to the working grid;
positive radii below half a grid unit are refused. Miter ratio rounds to
16.16 without changing the SVG ratio, and values below one are invalid.
The measured right-angle transition remains 92681/65536 (bevel) versus
92682/65536 (miter). Curve approximation and coverage approximation retain
their separately measured fixture tolerances.

Input is limited to 4096 commands. Expanded point and contour counts must fit
the imported unsigned-short outline representation before allocation/export.
Raster coordinates must fit ±0x1000000 in 26.6 units; each coverage axis is
at most 65535. Checked stride × height must fit both size_t and capacity.
Whole-shape range refusal still applies when the requested clip is small.

Width zero returns an empty outline. ARCS joins return UNSUPPORTED. Move-only
subpaths paint nothing. Non-butt degenerate subpaths containing segments also
return UNSUPPORTED, including those that collapse during grid conversion or
are discarded by the pinned stroker's curve epsilon. Curve control-polygon
edges below two working units on both axes are ignored; line segments are
ignored only at zero length. The guard follows successive edges and the
updated center, including discarded curves, separately for each subpath.
This prevents the safe empty output from FreeType from masquerading as SVG's
round/square zero-length cap. SVG must construct those caps, and resolve its
join fallback policy, above this boundary. Dashing and non-scaling-stroke
coordinate selection also belong above it.

The optional `stroke_with_caps` table tail accepts a second command stream of
clockwise filled cap contours. It plans the frame jointly, combines their
winding with the expanded stroke, and returns one outline for nonzero-rule
rasterization. [SVG stroke construction](SVG_STROKES.md) uses this to avoid
double compositing overlapping dots. The aggregate command limit applies to
both streams; ordinary `stroke` retains the degeneracy contract above.

## Work and cancellation

Work charges command/point visits, allocation and copied bytes, output bytes
and spans. `max_work` is literal, including zero, and resets for creation and
each path/stroke/bounds/raster operation. The suggested default is four million
(4 × 1024 × 1024) units. Statistics retain the last entered operation's cost.
Admission limits, work refusal and coordinate/count refusal return LIMIT;
physical allocation failure returns NO_MEMORY; cancellation returns CANCELLED.

Cancellation is cooperative at adapter, allocation and span checkpoints. A
FreeType call already executing may finish before the adapter returns. Once
cancellation is observed, span callbacks stop writing and allocation callbacks
stop admitting storage. Command, coordinate and expanded-output limits bound
the accepted geometry, but work accounting is not an upstream instruction
counter or a wall-clock deadline. The SVG job scheduler must retain its own
scheduling/cancellation policy.

## Verification

`python3 tools/test_svg_geometry.py --backend --output /tmp/geometry-backend`
compiles the production source list with ASan and UBSan, retaining leak checks.
Its harness includes the public header and calls the exported table; it does
not reach through FreeType structs. The independent 64 × 64 sampled capsule
oracle gives the experiment's same whole-image differences: 3.243137 pixel
equivalents / maximum 13/255 for scale(4,1), and 2.901961 / maximum 19/255
for reflection and shear (acceptance ≤8 and ≤24/255).

The suite covers mixed closure, fill rules, input lifetime, exact crop/padding,
16,384× zoom, billion-unit translation, fixed-point miter transitions, working
range, oversized expanded output, precision refusal, wrong-owner operations,
live-outline destruction refusal and context recovery. Short quadratic/cubic
subpaths exercise both round/square caps, open/closed forms and placement
before/after a visible subpath; threshold cases retain drawable curves and
one-unit lines. A large raster exhausts
work and a subsequent smaller raster succeeds. Cleanup works with cancellation
still requested. A representative curve stroke/render sweeps 32 allocation
failures and 1,045 cancellation checkpoints, with callback re-entry probes and
zero live blocks after teardown. Peak caller-charged memory is 6649 bytes in
that fixture; this is not a document memory estimate. The oversized cubic
fixture uses an unlimited work allowance to exercise the expanded-count guard
separately, reaching 4,356,236 peak bytes before refusal.

The existing font backend passes 1,089 checks under ASan/UBSan/LeakSanitizer.
The font contract suite passes its ownership/error/allocation checks and 17
hand-authored layout vectors, plus strict freestanding header compilation.
The import verifier reports 334 files and 12 patches unchanged. The cross-built
library has exactly the two getter exports, no undefined symbols or DT_NEEDED,
relative relocations and no writable/executable load segment. The private
runtime's target-compiled memory aliases and nonlocal-return test pass using a
Linux entry/exit launcher. The geometry adapter itself is cross-compiled and
linked; its execution evidence is hosted, not an Os64 guest run.

Sanitizer runs complete outside the sandbox with leak detection enabled;
attachment refusal in the sandbox is not counted as a pass. No P5, QEMU,
performance, WPT or browser-conformance result is claimed. The private proof
still generates its own gallery; its images are not expected-output files for
the production test. Independent cold review and Fable's architecture read
remain review steps, not claimed checks.

The solid-painting increment adds allocation-free bounds, translated-origin,
cancellation and failure-reset tests to this suite. The suite passes with
leak checks enabled, and its four saved masks are byte-identical to the prior
comparison run. The font/import verification counts above describe the original
backend delivery; they are not claims of a new font/import run for this tail.
