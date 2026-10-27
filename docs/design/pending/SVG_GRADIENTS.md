# Linear SVG gradients

2026-10-27. Linear paint servers extend the inline and standalone-image
profiles. The gallery is `/tests/pages/svg-gradients.html`, **SVG: a change
in atmosphere**. Modern Web enables SVG; scripts enable its reversible sky
button. Radial gradients are a separate increment on this numeric foundation.

## Numeric library boundary

`svg_linear_gradient_prepare` accepts endpoints, a gradient transform,
coordinate-unit choice, spread method and normalized sRGB stops. It combines
the consumer map, geometric object bounds when requested, and gradient map;
the resulting inverse projection maps sampling positions onto the gradient.
It has no DOM, cascade or allocation dependency. `svg_linear_gradient_sample`
returns straight-alpha ARGB at a position in the selected sampling space.
A future Canvas adapter can supply its own resolved values without creating
markup. Canvas retains responsibility for its own coordinate/lifetime rules
and any interpolation semantics different from SVG.

The prepared numeric sampler borrows immutable stops. The scene-provider
boundary has a different lifetime: libsvg copies the descriptor and its stops
before the next provider callback, charges that storage to its context and
releases it with the scene. Fill and stroke have independent copies. A caller
may discard or reuse provider scratch storage after preparation.

Object coordinates use analytic geometric bounds, without stroke expansion.
The map order is consumer-to-sample × object-box × gradient transform. This
preserves the gradient under rotation, shear and nonuniform scale. Fill and
stroke sample at output pixel centers, including the target's origin, and
retain existing coverage, opacity, clipping and isolated-group compositing.

Pad, repeat and reflect support negative as well as positive positions.
SVG interpolates unpremultiplied sRGB and alpha independently; premultiplication
occurs when composing the sample. Duplicate offsets retain source order and
the last duplicate owns the exact position. Zero-length gradients and a
single stop produce the final stop color. Empty stop lists, singular maps and
zero object extents produce no paint. Nonfinite or unrepresentable numeric
inputs fail preparation rather than reaching raster arithmetic.

## Resource and style adapter

Yonder resolves document-local linear-gradient references and template chains,
with unqualified `href` preceding namespace-correct XLink href. The first
specified endpoint, units, spread and gradient-transform attributes win along
the chain. The first gradient containing stop children supplies the stop list.
Missing/invalid resources and cycles use the paint's fallback color, or no
paint if absent. A valid empty gradient remains empty instead of using a
fallback. Image-mode external references fail without fetching.

The adapter supports objectBoundingBox and userSpaceOnUse; percentages in
user space use the consumer's current viewport. Numeric and absolute-unit
coordinates are resolved before calling libsvg. Relative/calc coordinates,
radial resources, cross-type gradient templates, individual CSS transforms
and explicit transform origins are outside this increment. CSS transform on
the referenced gradient replaces its gradientTransform attribute.

`stop-color` and `stop-opacity` are typed, non-inherited libgarb properties,
including presentation attributes and CSS-wide inheritance. Resource colors
follow resource ancestry, not the painted shape's ancestry. Inherited template
stops receive inheritance from the referencing gradient; source selector
matches are retained. Variable-dependent styles on copied stops are refused,
matching the existing bounded instance profile. Non-sRGB interpolation is
refused. Stop offsets are clamped to [0,1] and made nondecreasing; stop opacity
multiplies the color's alpha. Static-image script and animation children are
inert, including inside a gradient definition.

Limits are 256 stops and 128 resource-chain entries. Attribute scanning,
ancestry, chain comparisons and stop processing debit adapter work; scene
copies debit storage/work. Gradient painting reserves twelve work units per
pixel for its bounded stop search and sample computation. Existing allocation,
cancellation, output padding and teardown contracts remain in force. No
new frame loop, asynchronous painting or raster-variant cache is introduced.

The normative basis is [SVG2 paint servers](https://www.w3.org/TR/SVG2/pservers.html),
including template inheritance, gradient coordinate systems, stop offsets and
unpremultiplied color interpolation.

## Evidence

Host tests run with ASan, UBSan and LeakSanitizer enabled:

- Twenty-four exact inline/image pixel pairs cover coordinate systems, affine
  transforms, spread modes, opacity, duplicate/clamped offsets, templates,
  currentColor, CSS replacement, empty/degenerate resources, nested viewports,
  strokes and clips. Numeric sampler checks independently verify sample values.
- Resource tests cover cycles, blocked external references, inert image
  children and the stop-count cap. Existing adapter regressions remain green.
- Painter tests pass 1,749,268 assertions. Gradient-specific sweeps exercise
  seven preparation and 41 rendering allocation positions with both memory
  refusal modes, plus 38 preparation and 880 rendering cancellation positions.
  Poisoning provider stop storage after preparation proves scene ownership.
- The scripted-window suite passes 10,955 checks: changing one stop repaints
  two inline roots; the real picture worker's external image retains its own
  styling. Geometry accounting returns to baseline on teardown.
- The full libgarb parser/cascade regression suite passes.

Pinned Chromium 140.0.7339.16 / Playwright 1.55.0 produces 50 expected outcomes:
22 direct matches, two explicit-reference matches, two separately pinned
semantic differences and 24 visibly wrong controls. No mismatch is silently
absorbed into a broad tolerance. The two differences are:

1. Template `currentColor`: SVG2 shadow inheritance uses the referencing
   gradient; the pinned browser uses the source ancestry. An explicit blue
   stop reference agrees with native pixels.
2. userSpaceOnUse percentages in a nested consumer: native uses its viewport;
   the pinned browser uses the definition's viewport. An explicit coordinate
   reference agrees. Chromium's [issue 41055285](https://issues.chromium.org/issues/41055285)
   describes this class of resource-relative-length behavior.

The [browser report](../../svg-evidence/gradients-results.json) retains hashes,
compiler flags and native-source provenance. Reproduce with:

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-gradients-native
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --paint --output /tmp/svg-gradients-paint
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
python3 tools/svg_reftests/gradients.py --native /tmp/svg-gradients-native --output /tmp/svg-gradients-browser
```

The browser command requires the pinned environment described in
[the comparison harness](../../../tools/svg_reftests/README.md).

## Delivery checkpoint

The [frozen manifest](../../svg-evidence/gradients-delivery.sha256) identifies
35 files: Yonder, nineteen matching shared libraries, flowdump, nine HTML
galleries and five SVG assets. The gallery demonstrates a shared mutable sky,
object and user coordinates, repeat/reflect, transparency and an isolated
external postcard. Visible QEMU checks pass the toggle in both directions,
shared consumers, user-space coordinates, repeat/reflect, transparency,
independent image colors and 25%/400% zoom. The restored sky, rectangle and
stroke match their original captured pixels exactly. The guest shuts down
cleanly and both ext2 volumes pass read-only filesystem checks. P5 verification
is pending.

Screenshots: [morning](../../svg-evidence/yonder-gradients-top.png),
[evening](../../svg-evidence/yonder-gradients-changed.png),
[restored](../../svg-evidence/yonder-gradients-restored.png),
[spread modes](../../svg-evidence/yonder-gradients-middle.png),
[alpha and private image](../../svg-evidence/yonder-gradients-bottom.png),
[25%](../../svg-evidence/yonder-gradients-25-percent.png),
[400%](../../svg-evidence/yonder-gradients-400-percent.png).

Radial gradients, raster-variant caching and corpus-led latency/fidelity gates
remain in 6c. Text and broader graphics remain later work; this increment does
not claim complete static SVG or Canvas support.
