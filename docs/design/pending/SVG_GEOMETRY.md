# FreeType geometry proof

Status: private geometry experiment at commit `de20cd81`, 2026-10-26. Quinn.
The [production backend](SVG_GEOMETRY_BACKEND.md) builds on these findings;
the source-list and API statements below describe that experiment's commit.
Part of
[SVG slice 6a](SVG.md#implementation-stages); this does not complete that slice
or freeze the public geometry ABI.

The pinned FreeType port can construct and rasterize the selected SVG geometry
fixtures without a font face. Stroke expansion in user space followed by an
affine transform produces the required stretched round caps. The experiment
also identifies an upstream empty-subpath defect, a fixed-point miter boundary,
and limits the production adapter must expose rather than silently clamp.

[View the generated gallery](../../svg-evidence/geometry-proof.html).
The gallery shows coverage composited over light and dark backgrounds. It is
native geometry output, not an SVG document rendered by Yonder.

## Experiment and independent checks

`tools/test_svg_geometry.py` builds the pinned production FreeType source list
and port configuration, adding `ftstroke.c`, `ftglyph.c` and `ftbitmap.c` for
this experiment. The glyph and bitmap units satisfy the stroker's glyph-helper
link dependencies. No new operation table, public header, production build
membership or font backend ABI is introduced. `tools/svg_geometry/proof.c`
contains the private adapter and fixtures; it uses hosted libm for double
conversion and analytic reference calculations.

The path stream has move, line, quadratic, cubic and close commands. Each
stroke subpath is passed to the stroker with its own closure flag. Fill uses
implicit contour closure. The adapter expands strokes in a translated/scaled
user coordinate system, then maps the resulting outline into device space
relative to the requested clip origin. Direct grayscale spans write into
caller-supplied coverage with a checked stride and capacity. A backend error
requires discarding that destination, since rendering can have partly written
it. Validation errors are detected before clearing or writing the destination.

Selected round-cap fixtures are compared against an independently evaluated
capsule equation, using 64 by 64 subpixel samples. Acceptance requires both
total absolute coverage difference at most 8 pixel equivalents and maximum
per-pixel difference at most 24/255. The entire image participates; there is no
edge exclusion. These thresholds bound fixture coverage differences, not all
stroke geometry. The experiment's README records how the initial exploratory
tolerance was revised. Deliberately stroking a transformed centerline with a
scalar width exceeds 300 pixel equivalents of error; dropping the stroke
exceeds 1000. Both controls are required to fail the comparison envelope.

| Fixture | Total coverage difference | Maximum pixel difference |
|---|---:|---:|
| Round stroke under scale(4,1) | 3.243137 pixel equivalents | 13/255 |
| Reflected and sheared round stroke | 2.901961 pixel equivalents | 19/255 |
| Tiny round stroke enlarged 16,384 times, then scale(4,1) | 3.243137 pixel equivalents | 13/255 |

Exact checks distinguish an open contour from an adjacent closed contour,
verify nonzero/evenodd fill interiors and reversed inner winding, preserve a
crop of the full shape without introducing false caps, and leave row padding
untouched. A billion-unit translation canceled by the output map preserves
coverage byte for byte. Quadratic and cubic commands produce a filled fixture;
that fixture is a smoke check, not an independent curve-accuracy oracle.

## Miter and degenerate path findings

The width becomes a radius by dividing by two; the SVG miter ratio is passed
unchanged. `MITER_FIXED` gives the miter-to-bevel behavior, and
`MITER_VARIABLE` supplies the clipped-miter experiment. This follows the
[FreeType stroker contract](https://freetype.org/freetype2/docs/reference/ft2-glyph_stroker.html).

For the right-angle fixture, the mathematical threshold is square root of two.
At a 16.16 limit of 92681 the exported outline has eight points and a beveled
tip. At 92682 it has seven points and the complete miter. The mathematical
threshold and nearby doubles that round to 92682 therefore give identical
geometry. Tests cover both fixed-point sides and values immediately around
the mathematical boundary. This rounding is a measured backend property;
production must decide how to handle a threshold closer than its precision
permits. The experiment refuses miter limits below one instead of accepting
FreeType's internal clamp. SVG parsing and fallback policy remain above this
backend boundary.

A zero-length line reached null-pointer arithmetic in
`ft_stroker_add_reverse_left` through `FT_Stroker_EndSubPath` under UBSan.
Upstream [5c79d6cd1ac73d70a55f3d963fb568aa32f6d794](https://github.com/freetype/freetype/commit/5c79d6cd1ac73d70a55f3d963fb568aa32f6d794)
moves the empty-subpath guard into the public end-subpath operation. Patch
0012 backports it, and the import manifest records the patch and modified
source digests. The earlier font-only review omitted this fix because the
stroker was not compiled; the review now records the geometry consumer.

With the fix, move-only and zero-length line fixtures return empty outlines
without sanitizer errors. That is safe backend behavior, but SVG's round or
square zero-length caps still need explicit geometry in the SVG layer. The
experiment keeps this distinction visible instead of treating empty output
as full SVG support.

## Measured coordinate and storage envelope

The experimental adapter first translates by the initial move point. Its
working scale is a power of two, at least 256 and at least 256 times the
maximum absolute row sum of the output linear transform. It refuses a scale
above 2 to the power 30. This bounds the device effect of rounding an input
coordinate to the working grid to at most 1/512 pixel per axis. Final device
26.6 conversion contributes at most 1/128 pixel per axis. Floating-point
cancellation and the stroker's curve approximation are separate error sources;
these arithmetic bounds are not an end-to-end stroke-accuracy guarantee.

Before conversion, the experiment requires
`(maximum translated control-coordinate magnitude + width/2 * (miter + 2)) * scale`
to fit within 1,048,576 working units. Non-finite input is invalid; geometry
outside that conservative envelope returns LIMIT. A positive width whose
working radius rounds below one unit is refused. No coordinate is independently
clamped. The boundary itself and the next working-grid step are tested.

The rasterizer's pinned `FT_Outline_Render` checks control coordinates against
plus or minus 0x1000000 in 26.6 units, equivalent to 262,144 pixels relative to
the destination origin. The adapter preflights that bound before calling it.
A small clip does not legalize an arbitrarily large outline: the complete
transformed shape must still fit the admitted range. Singular transforms are
handled without inversion; a collapsed-line fill returns empty coverage.

The experiment limits input to 4096 commands. Input and expanded point counts
are checked against the imported unsigned-short outline limit before export
or narrowing. A repeated cubic fixture forces expanded output past that limit
(149,504 points in this run) and must return LIMIT; it is not split into
separately composited pieces.
Coverage axes are limited to 65535 for the imported direct-span representation,
and stride times height must fit both size_t and the supplied capacity.

Allocator hooks track live/peak bytes including block headers. A representative
curved-stroke operation reaches 30 allocations and peaks at 6369 bytes. Failing
each allocation in turn returns an allocation failure and teardown leaves no
live blocks. A caller-imposed cap also refuses allocation, and failure in one
context does not prevent an independent context from succeeding. This is
allocation plumbing evidence; the final shared SVG admission ledger and its
separate LIMIT/NO_MEMORY reporting remain production-adapter work.

## Verification and remaining gates

The proof passes with ASan, UBSan and LeakSanitizer. The sandbox produced an
attachment EPERM with TracerPid zero; the complete suite passed outside it
with leak detection enabled. `tools/import_freetype.py --verify` checks the
updated source and patch digests. The shipped font library cross-builds, and
an experimental shared link of its target objects plus the three geometry
units has no undefined symbols or DT_NEEDED dependencies. This verifies the
upstream dependency closure; the hosted adapter itself is not a target build.

The gallery is regenerated from the passing proof's masks and results. No P5,
QEMU, WPT or browser-reference result is claimed, and no speed claim is made.
The font ABI and shipped source list remain unchanged.

Before 6a is complete, build the versioned production geometry table and
owner-supplied allocation ledger, with outline/context lifetime enforcement,
work and cancellation limits, and precision/refusal semantics informed by
these experiments. Extend independent reference coverage beyond capsules,
exercise browser fixtures for non-scaling strokes and viewport/device maps,
and establish the WPT match/mismatch/fuzzy runner. Dash subdivision, zero-length
cap construction, full SVG paths and painting belong to their specified
renderer stages. Fable's architecture/implementation read remains a separate
review step.
