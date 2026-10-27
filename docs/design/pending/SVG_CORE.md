# SVG path and scene foundation

Status: first 6b increment, 2026-10-26, based on merged PR #360 at `d4846e3d`.
This implements part of [SVG.md](SVG.md), not the complete static renderer.
The subsequent [solid painter](SVG_PAINT.md) builds on these contracts;
validation numbers below describe this foundation increment.

## Delivered boundary

`userland/libsvg` builds a native C `libsvg.so`. Its public interface is
`svg/svg.h`; it imports arithmetic from `libmath.so`, with no allocator,
I/O, tree, CSS or FreeType-private imports. The caller supplies allocation
and cancellation callbacks. The library exports path parsing, conversion to
the public geometry command stream, and group/path/shape scene preparation. [SVG_SHAPES.md](SVG_SHAPES.md) records
the subsequent typed-shape conversion and provider callback.

The root image inventory, userland build, prelink population and debugger
dependencies include the library. The [Yonder connection](SVG_YONDER.md)
links it for inline SVG painting. The host
harness consumes the public SVG and geometry interfaces together, including
the production FreeType adapter and pinned math sources. Yonder's layout,
computed styles and painter are unchanged.

## Paths

The parser accepts absolute and relative M/L/H/V/C/S/Q/T/A/Z commands,
repeated argument groups, implicit lines after moveto, compact number
boundaries and arc flags. Smooth controls depend on the preceding command
family. Closing restores the subpath start. Explicit zero-length segments
remain available to later cap/direction handling. A malformed segment returns
`SVG_MALFORMED` with an owned complete prefix and an error byte offset.
Resource failures return no prefix. These behaviors follow the
[SVG path rules](https://www.w3.org/TR/SVG2/paths.html).

The decimal scanner is locale independent and bounded. It accepts SVG 1.1's
unambiguous trailing decimal point; strings such as NaN and infinity are
malformed. Decimal conversion keeps up to 19 significant digits and uses
binary64 arithmetic; it does not promise correctly rounded `strtod` results.
Overflow, nonzero underflow to zero, or nonfinite relative/reflected coordinates
return `LIMIT`. Empty input makes an empty owned path. CSS `none` must be
translated to empty data by the style provider.

Owned path commands use absolute double coordinates and retain endpoint-form
arcs. Input bytes can be released immediately. A path can be lowered repeatedly
for different transforms and tolerances. Scene paths belong to their scene;
their separate release returns `BUSY`.

## Arc conversion and geometry

Conversion follows the SVG
[endpoint-to-center construction](https://www.w3.org/TR/SVG2/implnote.html#ArcImplementationNotes).
Negative radius signs are dropped. Coincident endpoints omit the arc; a zero
radius makes a line. Insufficient radii are enlarged together. Conversion uses
a translated, normalized frame, `hypot`, and checked finite arithmetic.

Arcs become cubic Hermite pieces in user space. For each device coordinate,
the ellipse's fourth derivative is bounded by the sum of the magnitudes of
the mapped ellipse basis vectors. A piece of angular width `h` has scalar
interpolation error bounded by `M*h^4/384`. Subdivision reserves half of the
requested per-axis positional tolerance for this error. A separate conservative
arithmetic admission check covers coordinate magnitude and sensitivity near
the meeting of the two possible arc centers. Small-angle, extreme-radius or
high-zoom inputs can return `LIMIT`; the numerical admission check is not a
formal proof of correctly rounded geometry for arbitrary binary64 inputs.

The allowed arc tolerance is finite and in `(0,1]` device pixels. Singular
maps require no inverse. Source endpoints are preserved in the generated
stream; a segment following CLOSE gets the MOVE required by the backend.
Repeated CLOSE commands need no duplicate backend contour. Output is capped
at the smaller of the caller limit and the backend's 4,096-command limit.
Filled paths are not split to evade that bound.

This tolerance measures curve position. It excludes tangent/stroke-boundary
error, backend coordinate quantization and raster coverage approximation.
The caller passes the resulting borrowed command view to the existing geometry
table and releases the owned conversion product afterward. That table copies
its input into independently owned outlines.

## Scene/provider contract

The provider is an implementation source interface, not a frozen browser ABI.
`acquire(mode)` establishes a stable document/style snapshot and supplies a
root source identity. `node` supplies structural links and a supported kind;
`style` supplies resolved values and the winning path data. The host keeps
mutation serialized during preparation. The successful scene retains the
snapshot pin until release, protecting source identity lifetime. A failure
releases the pin and partial products; `PENDING` can be retried.

The structural view contains groups and paths. A future DOM adapter must
classify non-rendering metadata appropriately and report unsupported rendering
features explicitly. Root siblings are outside the prepared subtree. Traversal
is iterative and checks active ancestry; repeated source identities in separate
branches remain legal and receive distinct instance identities. This proves
the identity/inheritance seam, not SVG `use` expansion.

The style request includes source, instance, parent instance, parent computed
style and processing mode. The provider applies CSS inheritance per property;
opacity and transform are not blindly inherited. Prepared items retain parent
indices, local styles, composed maps, paths and malformed-path diagnostics.
Group opacity remains on the group. `display:none` skips its subtree;
`visibility:hidden` keeps descendants available. The provider supplies the
winning resolved transform, including origin, rather than multiplying a losing
attribute value into it.

The caller supplies the resolved root viewport map. Shapes lower to native
paths; container viewport fields distinguish frame placement from descendant
content maps. The provider resolves viewport dimensions and viewBox mapping;
intrinsic sizes and CSS layout negotiation remain outside the native API.
Solid fill/stroke values are retained for the separate
[paint entry point](SVG_PAINT.md). Unrepresentable active features must return
`UNSUPPORTED`, not silently become solid colors or ordinary strokes.

Mode is explicit at acquisition and styling. No fetch or resolver callback is
exposed. An IMAGE provider must itself honor the no-fetch rule; passing the
enum does not sandbox host callbacks. External-image loading and browser policy
enforcement remain integration work.

## Ownership, admission and work

The context uses the geometry allocator callback type so SVG and FreeType can
share one owner admission ledger. Each allocation is charged once by that
owner, including the SVG allocation header. SVG live/peak counters report its
subdivision; they are not another admission cap. Scene pins must account for
document ownership under the same host policy. Replacing a scene can keep old
and new products live together, both charged.

Limits are literal, including zero. `svg_default_limits()` supplies:

| Quantity | Default |
|---|---:|
| Input bytes per path | 1,048,576 |
| Canonical source commands per path | 32,768 |
| Converted geometry commands | 4,096 |
| Visited source occurrences per preparation | 100,000 |
| Active scene depth, including root | 128 |
| Work per create/parse/lower/prepare operation | 4,000,000 |

Work charges input bytes, command/ancestry visits, arc subdivision, callback
boundaries, allocation bytes and copied bytes. Limits interact: the work or
owner byte cap can stop before the command/node maximum. Callback-internal
CPU work belongs to the host; callbacks must remain bounded and nonblocking.
Cleanup neither allocates nor polls cancellation. The context refuses destruction
while products live, rejects reentrant operations, and enforces product ownership.
Queries returning borrowed views require those products to remain alive.

## Validation

Run from this worktree:

```sh
python3 tools/test_svg_core.py --output /tmp/os64-svg-core-check
CCACHE_DISABLE=1 make -C userland -j4 svg-library
```

The host test compiles production SVG membership, the FreeType source lists,
and pinned libmath sources. ASan, UBSan and LeakSanitizer are enabled. A sandbox
run reported attachment `errno 1` with `TracerPid: 0`; the same binary passed
outside the sandbox with leak checks enabled. No leak-disabled result is used.

The final run passes 148,122 assertions. The suite covers grammar and prefix errors, close/smooth state, compact flags,
number/range limits (including million-digit exponent-cancellation regressions),
ownership and reentry, scene ancestry/instance styling,
mutation across preparations, pin cleanup, unsupported/pending outcomes,
hidden descendants and suppressed sibling cycles. It includes a 4,097-command
source that parses but cannot be converted into one backend outline, explicit
work/depth/input limits, and 4,000 deterministic arbitrary byte streams.

The million-digit regression fails against the original scanner and passes
with exponent saturation bounded above the admitted input length.

The combined parse/lower/scene pipeline retains a small scene while preparing
a 24-level scene, exercising item and traversal-stack growth. It sweeps 23 allocation failure positions
in both admission-refusal and physical-exhaustion modes, plus 526 cancellation
positions. Its observed owner peak is 17,912 bytes; this fixture peak is not a
page budget estimate. Sixty ellipse/transform combinations, sampled along their
generated cubics, have maximum measured device-axis positional error
`0.00649122335` pixels against a requested `1/64`. These include reflection,
shear, unequal scale, 16,384× zoom and a singular map. A tightly budgeted,
high-zoom semicircle is explicitly refused by numerical admission.

The parsed radius-20 circle goes through the public FreeType fill and stroke
operations. Against an independent 64×64 subpixel analytic coverage oracle:

| Arc tolerance | Converted commands | Maximum coverage difference | L1 difference |
|---|---:|---:|---:|
| 1/64 | 18 | 32/255 | 8.164706 |
| 1/1024 | 34 | 32/255 | 8.211765 |
| 1/4096 | 34 | 32/255 | 8.211765 |
| Independent 512-edge circle | 514 | 2/255 | 0.388235 |

The provisional circle threshold of 24/255 failed. The independent polygon
and tighter arc conversions isolate a cubic-raster coverage difference in
this fixture; finer positional geometry does not ensure monotonically better
pixel coverage. The new fixture's measured envelope is maximum 40/255 and
L1 10. At each tolerance, a one-pixel translation and a 10% radius increase
exceed both bounds. Existing geometry and browser comparison thresholds are
unchanged. This is selected analytic evidence, not browser conformance.

The freestanding cross build passes `--no-undefined`; dynamic imports are
math functions and the single dependency is `libmath.so`. No P5, QEMU,
Yonder paint, performance or new browser/WPT result is claimed by this increment.

## Following work

Yonder integration must account for concurrent work on the browser. Keep the
connection slice in its own PR and stack dependent follow-on PRs on that branch
while it is unmerged. Before editing shared Yonder files, inspect the active
browser changes and keep the adapter changes localized; refresh the stack as
its base merges so review diffs show each increment's work.

The [Yonder connection](SVG_YONDER.md) supplies the native tree/style adapter,
paint properties, outer sizing and viewBox mapping. [Basic shapes](SVG_SHAPES.md)
[dash/cap construction](SVG_STROKES.md) and [same-document instances](SVG_INSTANCES.md)
extend that consumer. [Viewports](SVG_VIEWPORTS.md) add symbol/referenced SVG
sizing, nested viewports and rectangular overflow clipping.
[Clip silhouettes](SVG_CLIPS.md) add one direct path/shape clip resource;
broader clip combinations remain part of 6b. The
[solid painter](SVG_PAINT.md) consumes prepared paths/groups and isolates opacity;
the connection paints them in real inline SVG boxes. Parsed-path interning, cache lookup,
length/marker APIs and stroke-error control need their own evidence.
[Geometric bounds](SVG_BOUNDS.md) adds allocation-free path/subtree queries
and object-coordinate clip mapping with separate analytic/browser evidence.

The first useful P5 checkpoint is the connection's real inline fixture page
reaching Yonder's paint path. Full 6b completion is not a prerequisite for that checkpoint,
but this library increment alone does not reach it. Remaining work is recorded
under SVG in `DEBTS.md`.
