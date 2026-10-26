# SVG geometry experiment

Run from a checkout with the pinned FreeType import:

```sh
python3 tools/test_svg_geometry.py --output /tmp/svg-geometry
```

The runner builds the shipped FreeType source lists and port configuration,
plus upstream `ftstroke.c`, `ftglyph.c` and `ftbitmap.c`. These three units are
linked by this experiment; they are not added to the shipped library's build
list. It links the private hosted adapter in `proof.c`, with ASan, UBSan and
leak detection enabled, and keeps the binary, raw PGM coverage, PNG previews,
results and a self-contained `index.html` in the chosen output directory.
The PNG/HTML generator uses the Python standard library. The adapter uses
host libm and is not a public or target-ready geometry API.

The test compares selected round strokes to an independent analytic capsule
oracle sampled at 64 by 64 centers per pixel. Its two limits are a total
absolute coverage difference of 8 fully covered pixel equivalents and a
maximum individual difference of 24/255, over the complete 192 by 128 image.
No edge pixels are exempt. These are fixture coverage tolerances, not a bound
on the geometric error of arbitrary FreeType strokes. The first exploratory
3-pixel total bound exposed a 3.24-pixel difference for the stretched cap;
raising reference sampling from 32 to 64 barely changed it. The retained
8-pixel envelope accommodates both that curve/raster approximation and the
shear fixture while rejecting the wrong stroke order by over 300 pixel
equivalents and a missing stroke by over 1000. The maximum-pixel check also
prevents concentrating the allowed total error in a small lost detail.

Expected matches and negative controls are asserted in C. Saved images and
the HTML gallery are review artifacts, not inputs that determine the test
verdict. Other assertions cover exact fill-rule interiors, winding, open vs
closed stroke contours, crop equivalence, untouched row padding, fixed-point
miter transitions, invalid geometry, coordinate/count limits, context
isolation and allocation failure at each reached allocation.

The default sanitizer settings retain leak detection. If the sandbox denies
LeakSanitizer attachment, run the same binary outside it; do not interpret an
attachment failure as a clean leak result. The binary takes the output
directory as its single argument.

See [the geometry results](../../docs/design/pending/SVG_GEOMETRY.md) for the
measured envelope, upstream fix and production decisions still required.
