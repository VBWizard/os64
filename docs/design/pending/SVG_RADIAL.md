# Radial SVG gradients

2026-10-27. This increment extends [linear gradient paint](SVG_GRADIENTS.md)
with radial fills and strokes for inline SVG and independent image documents.
The P5 gallery is `/tests/pages/svg-radial.html`, **SVG: in a different light**.
Modern Web enables rendering; scripts enable its reversible focal-point toggle.

## Numeric interface and ownership

`svg_radial_gradient_prepare` accepts resolved start/end circles, the gradient
map, coordinate units, stops and spread method. Like the linear API, it has
no markup, CSS, allocator or frame-loop dependency. It validates through the
existing bounded stop/map contract and prepares an inverse map and normalized
circle parameters. Object coordinates use geometric bounds, including when
the gradient paints a stroke. Nonuniform maps naturally produce elliptical
rings in the target space.

`svg_radial_gradient_position` finds a circle parameter before spreading or
color interpolation. It solves the two-circle equation, rejects roots with
negative radius and chooses the last valid root. The stable quadratic-root
form avoids subtracting almost-equal roots. Geometry is normalized by the
larger radius or center distance. Tangency uses a tolerance of eight binary64
epsilons relative to the center distance/radius difference, accommodating
roundoff from separately resolved values. A tangent equation is linear. Near a double root, discriminant cancellation
uses an eight-epsilon bound relative to its two products. The matching radius
check admits a rounded zero within the same relative bound; ordinary roots
retain the nonnegative-radius test. There is no absolute epsilon that would
fill uncovered cones at small coordinate scales.

This position query returns false for uncovered/empty geometry and samples
whose arithmetic is unrepresentable, clearing its output. A non-markup caller
can therefore reuse the geometry while applying its own paint policy. In
particular, Canvas need not inherit SVG's tangent-cone extension.

`svg_radial_gradient_sample` adds the SVG policy: existing pad/repeat/reflect
and unpremultiplied sRGB stop sampling, with terminal colors or an integrated
stop average on the collapsed side of a tangent cone. The numeric tests
assert both the geometry-only result and the SVG sample for the same point.
This is a reusable boundary, not a Canvas implementation or a claim that its
other color/degenerate-case semantics are already supplied.

Prepared samplers borrow immutable stop storage. Prepared scenes copy radial
descriptors and stops before subsequent provider callbacks, independently for
fill and stroke. Cleanup clears the unselected descriptor pointer and releases
the selected allocation with the scene. Storage and work remain charged to the
existing owner; rendering budgets thirty-two work units per radial pixel.

## Resource adapter

The Yonder adapter resolves `cx`, `cy`, `r`, `fx`, `fy` and `fr`. Absent focal
coordinates use the resolved center. Radius percentages in user coordinates
use the normalized viewport diagonal; x/y percentages use their own axes.
Negative radii and malformed geometry refuse painting with normal cleanup.

Linear and radial definitions may reference each other. Stops and common
attributes travel through the bounded template chain; geometry attributes
are read from the gradient type to which they belong. Source selector matches,
referencing-resource inheritance, typed stop properties, CSS transform
replacement, URL fallback and image-mode isolation retain the linear profile's
contracts. Copied stops requiring variable substitution, unsupported relative
lengths/calc, explicit transform origins and non-sRGB interpolation retain
explicit refusal. Pattern paint servers remain unsupported.

The [SVG2 radial-gradient rules](https://www.w3.org/TR/SVG2/pservers.html#RadialGradientNotes)
define the two circles, transparent areas outside the cone, identical-circle
behavior and SVG's special tangent extension. Identical circles, including
coincident zero-radius circles, paint nothing in this implementation.

## Evidence

The native adapter suite passes 36 radial inline/image pairs, in addition to
its existing linear, image, shape, stroke, viewport, instance and clip tests.
Cases cover centered/eccentric circles, nonzero focal radius, reversed radii,
repeat/reflect inside and outside the focal circle, affine maps, user units,
percentage radii, alpha, strokes, templates across gradient types, CSS transform
replacement, nested viewports, external foci, tangencies, empty/single-stop and
singular resources. Separate checks reject negative/nonfinite geometry and
close a mixed-type reference cycle through the fallback paint.

Numeric API tests independently check analytic colors/positions, transformed
object coordinates, invalid inputs, cone coverage and tangent roundoff. The
painter suite passes 1,822,957 assertions. Both linear and radial scene tests
poison provider storage after preparation, sweep allocation failures in both
refusal modes, sweep cancellation, preserve output padding and return ownership
to baseline. The scripted-window suite passes 10,970 checks, including shared
focal-point mutation and the real picture worker's isolated radial image.
Host suites use ASan, UBSan and LeakSanitizer.

Pinned Chromium 140.0.7339.16 / Playwright 1.55.0 produces 74 expected radial
outcomes: 34 direct matches, two explicit-reference matches, two recorded
identical-circle differences and 36 negative controls. For the identical-circle
cases the pinned browser uses the final stop color; native follows SVG2's
transparent result. Separate `fill=none` references match native, and the two
browser differences have fixed metrics rather than permissive tolerances.
The shared stop sampler also passes the 50 expected linear browser outcomes.

The frozen delivery passes a QEMU gallery check at 25%, 100% and 400% zoom.
The focal-point toggle changes both shared consumers and restores the pearl's
original pixels exactly; the separate image retains its own highlight.
Normal shutdown stops Yonder cleanly, and read-only filesystem checks pass
for both private guest partitions. Screenshots record the
[initial view](../../svg-evidence/yonder-radial-top.png),
[changed highlight](../../svg-evidence/yonder-radial-changed.png),
[restored highlight](../../svg-evidence/yonder-radial-restored.png),
[rings and focal circle](../../svg-evidence/yonder-radial-middle.png),
[glow and independent image](../../svg-evidence/yonder-radial-bottom.png),
[25% view](../../svg-evidence/yonder-radial-25-percent.png) and
[400% detail](../../svg-evidence/yonder-radial-400-percent.png).

The [radial report](../../svg-evidence/radial-results.json) retains source and
binary hashes, compiler flags and browser provenance. Reproduce using the
pinned environment in [the harness instructions](../../../tools/svg_reftests/README.md):

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-radial-native
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-radial-paint
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
python3 tools/svg_reftests/radial.py --native /tmp/svg-radial-native --output /tmp/svg-radial-browser
python3 tools/svg_reftests/gradients.py --native /tmp/svg-radial-native --output /tmp/svg-linear-regression
```

## Review regression

PR #385 exposed a transparent center pixel for concentric gradients with a
nonzero focal radius: the discriminant rounded slightly below zero. A second
rounding boundary could reject the corresponding zero-radius root. The numeric
regressions cover growing/reversed radii at scales from 1e-12 to 1e12, both
signs of discriminant cancellation, eccentric cone boundaries and genuinely
uncovered or negative-radius roots. An odd-sized painted rectangle places the
center exactly on a pixel sample and verifies that its interior remains opaque.
The painter and full adapter suites pass with ASan, UBSan and LeakSanitizer
enabled, and freestanding Yonder builds. The [review browser report](../../svg-evidence/radial-review-results.json)
records the corrected source and 74 expected radial outcomes. The merged
CSS-wide transform suite also retains all 29 expected browser outcomes.
The frozen gallery delivery below retains its hardware-tested bytes.

## Delivery checkpoint

The [frozen manifest](../../svg-evidence/radial-delivery.sha256) identifies
37 files: Yonder, nineteen matching shared libraries, flowdump, ten HTML galleries
and six SVG assets. Eighteen unchanged shared libraries and flowdump reuse
the prior tested delivery bytes. Against that delivery, 33 files are unchanged;
the update consists of Yonder, libsvg and the two new gallery assets.
The six-card gallery covers a movable pearl highlight,
shared elliptical/stroked consumers, repeat/reflect rings, a focal circle,
a transparent glow and an independent image. P5 verification accepted commit
`21a007f4` on 2026-10-27: the focal-point toggle affected cards 1 and 2 as
expected, and the other inspected zoom levels rendered correctly. At 25% the
rings were too small for the tester to verify their detail completely; the
automated and QEMU evidence above covers that scale separately.

Raster-variant caching and corpus-led latency/fidelity gates remain in 6c.
Broader graphics and text remain separate work. There is no new animation loop
or asynchronous raster worker in this slice.
