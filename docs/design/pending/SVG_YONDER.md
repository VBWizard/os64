# SVG in Yonder — first inline connection

2026-10-26. This increment connects the native [solid painter](SVG_PAINT.md)
to real HTML documents, the CSS cascade, layout and Yonder's window compositor.
It is the first P5 checkpoint, not completion of the static SVG profile in
[SVG.md](SVG.md). The fixture is `tools/pages/svg-inline.html`, installed by
`make stage-pages` as `/tests/pages/svg-inline.html`.

The [standalone document adapter](SVG_DOCUMENTS.md) also uses this renderer
with an isolated XML tree/cascade and image-owned root effects. The
[image route](SVG_IMAGES.md) connects it to picture jobs, `<img>` and backgrounds.

## What reaches the screen

With Modern Web enabled, an outer inline SVG creates a replaced CSS box.
`libflow` owns its placement, CSS dimensions, root CSS transform and root
opacity. Its intrinsic-size callback reads absolute width/height and viewBox
ratio, with the 300 by 150 default object size. The adapter maps viewBox using
`none`, the nine alignments, `meet` or `slice`; the viewport clips the output.
Page zoom scales user units, including an outer SVG transform's translation.

The SVG tree/style provider reads the existing libhtml document. Groups, paths
and [basic shapes](SVG_SHAPES.md) support solid fill and stroke, currentColor, fill rules, fill/stroke
opacity, isolated group/element opacity, cap/join/miter settings, visibility
and SVG transform attributes. Supported CSS 2D transforms replace the transform
attribute. The outer box's transform and opacity are painted once by Yonder;
their values remain available to explicit inheritance inside SVG. HTML ancestor
opacity and the root's CSS opacity/transform use libflow's supported profile.
The adapter retains cascade values and evaluates them through its narrower
profile when an SVG descendant paints them, including explicit inheritance.
Ordinary strokes expand in user space before transformation.

`libgarb` parses SVG paint properties and supported presentation attributes.
Presentation declarations enter author origin at zero specificity, before
stylesheets; inline style, specificity, importance and custom-property
substitution use the existing cascade. Attribute values are parsed as values,
not concatenated into a declaration block. SVG width/height attributes accept
unitless user coordinates without changing CSS's length grammar. Parsed values
are copied into the cascade's bounded arena and temporary parsers are freed.
`libpage` discovers SVG style elements alongside HTML stylesheets.

Mutation uses Yonder's existing document/cascade/layout rebuilding. A script
changing the check mark's color therefore changes its next paint. Modern Web
off restores the foreign-content layout path and disables SVG rendering.

These boundaries follow [SVG presentation attributes](https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes),
[viewBox mapping](https://www.w3.org/TR/SVG2/coords.html#ViewBoxAttribute) and
[paint/currentColor](https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint).
The implemented subset below limits those contracts.

## Ownership and work

The synchronous adapter pins document lifetime while a scene exists. Yonder's
window thread serializes mutations around that call; the pin is not a mutation
lock. No resource fetch or other I/O occurs in the adapter.

A Page-owned SVG subdivision admits at most **64 MiB** across scene preparation,
FreeType, painter scratch and returned pixels. Each paint releases its scene
and output. The output is straight-alpha ARGB; the compositor consumes it
before the adapter's explicit free. This subdivision does **not** combine the
HTML tree, cascade, layout and raster-image budgets into a whole-page ledger.
Those libraries retain their separate bounds; unification remains a debt.

Adapter bounds are 4,096 scene nodes, 128 ancestry levels, 4,000,000 adapter work
units, 4,096 bytes per viewport/transform attribute and 1 MiB per path or point-list string.
Output is limited to 16,384 pixels on either axis and 8,388,608 pixels total,
then admitted against the shared byte cap. It allocates the visible crop using
the full viewport's map. Native SVG and geometry limits apply in addition.
There is no scene/path cache or asynchronous cancellation integration. A dirty
paint prepares and renders again, synchronously. These caps bound admitted
work; they are not a demonstrated frame-time guarantee for arbitrary pages.

## Explicit limits

The inline profile handles paths, groups and [basic shapes](SVG_SHAPES.md),
plus [dashes and zero-length caps](SVG_STROKES.md) and
[same-document use instances](SVG_INSTANCES.md) and
[referenced/nested viewports](SVG_VIEWPORTS.md) with rectangular overflow clipping.
[One-silhouette clip resources](SVG_CLIPS.md) extend clipping beyond rectangles;
[object-coordinate clips](SVG_BOUNDS.md) fit the drawing's geometric bounds.
Definitions, clip resources, uninstantiated symbols, metadata, titles, descriptions and style
elements are omitted from drawing. [Linear gradients](SVG_GRADIENTS.md) supply fill and stroke paint.
[Radial gradients](SVG_RADIAL.md) extend the same paint path.
Other active element kinds refuse the SVG job. Text,
patterns, images, markers, broader clip combinations, masks, filters, non-scaling
strokes and animation remain follow-on work. External SVG images use the [Yonder picture route](SVG_IMAGES.md),
with document ownership separate from libimage raster decoding. SVG-specific hit testing and DOM interfaces are
also outside this slice.

Numeric and px stroke widths and percentage widths are supported; the latter
use the normalized viewBox diagonal. Relative lengths and calc expressions
that require evaluation inside the SVG adapter are refused. Flow-owned
ancestor/root opacity and root transforms retain libflow's support; a
descendant explicitly inheriting a calc opacity is outside the adapter profile. Descendant CSS transform-origin and individual
translate/rotate/scale properties are refused. Color interpolation is sRGB.
Nonzero strokes that collapse below the backend grid and the native arc-coverage limits still apply.

Active unsupported paints/elements and recognized unsupported presentation
attributes refuse the job without publishing partial pixels. Yonder records
`FAILED svg` for render failures, including malformed inputs and admission
refusals. CSS properties absent from garb remain governed by its skipped-style
diagnostics; this is **not** a claim that every omitted SVG feature causes a
renderer refusal. Enabled inline SVG is no longer globally counted as missing.

## Verification and reproduction

```sh
ASAN_OPTIONS=detect_leaks=1 python3 tools/test_svg_core.py --yonder --output /tmp/svg-yonder
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_yonder_scripts_host.sh
ASAN_OPTIONS=detect_leaks=1 bash tools/test_garb_host.sh
CCACHE_DISABLE=1 make -C userland -j4 "$PWD/userland/bin/yonder" "$PWD/userland/bin/tests/flowdump"
make stage-pages
```

The focused integration harness compiles production HTML, page, cascade, layout,
SVG, FreeType and pinned math sources. It checks actual paint dispatch and
pixels, presentation/CSS precedence, embedded SVG stylesheets, currentColor,
viewBox alignment/stretch/slice, zoom, cropped placement, mutation and disabled
Modern Web. Unsupported elements/paints return no output. It sweeps
**81 allocation positions in both admission and allocator refusal modes**,
with recovery and zero live ledger bytes after each call. Its fixture peak is
**233,084 bytes**, not a real-page performance estimate. Source/header/compiler
and binary provenance accompanies the run.

The existing Yonder suite passes **2,028 checks** and the full window/script
suite passes **8,221 checks** after the second review fix (including the
import-map integration from #366). The garb
parser/cascade regression suite passes.
The full window harness checks root opacity, explicit visible descendants of a
hidden SVG, CSS replacing the root transform attribute, and script recoloring
through the actual compositor. Host runs preserve ASan, UBSan and LeakSanitizer;
run outside a sandbox that prevents leak-scanner attachment.

A windowed QEMU boot with private copies of both disks displayed all six
fixture cards, scrolled through them, and changed the check mark from blue to
orange through its button. Zoomed repaint was checked at 100% and 150%.
The [first viewport](../../svg-evidence/yonder-inline-qemu.png) and
[clicked result](../../svg-evidence/yonder-inline-recolor-qemu.png) are guest
screenshots.
The initial test-disk installation failed before Yonder opened: its older
libos64 lacked five UI symbols. Installing the matching library cohort resolved
that setup failure without a kernel or renderer change.

On the P5, Chris confirmed that switching dark pages changes the surrounding
backgrounds while the painted amber SVG path retains its color. With scripts
enabled, clicking the fixture button changes the check mark from blue to
orange. The button sets orange on each click; it is not a toggle. These
hardware observations do not establish broader browser/WPT equivalence or
page-scale performance. The guest and P5 checks used `cf1ba15f`, before the
import-map merge. The combined revision passes the window/script suite above
and the freestanding Yonder/flowdump build.

## P5 checkpoint and next work

Install the rebuilt Yonder, flowdump and matching shared libraries together;
the added flow environment field requires rebuilding consumers. The gallery
needs Modern Web on; its recolor button also needs scripts on. Inspect the
landscape, equal-color opacity overlap, blue/orange CSS bars, amber hole,
stretched round caps, and the blue check mark turning orange. Scroll, resize
and zoom to check clipping and repaint. Switch Modern Web off to confirm the
artwork disappears.

Keep this connection in its own PR. Stack dependent SVG follow-ons while it
is unmerged, and inspect concurrent Yonder changes before extending shared
files. Remaining 6b work and page-accounting/performance gates are in DEBTS.md;
small fixture success does not close those gates.

## Review regressions

The first review identified two adapter boundary errors. Rejected dimension
attributes wrote their numeric prefix into the fallback size before refusing
the suffix. Dimension parsing now publishes a value only after accepting the
whole attribute. Tests cover percentages on either axis, invalid suffixes,
negative dimensions and valid absolute-size controls. The responsive
`width="100%" viewBox="0 0 400 100"` case now lays out at 256 by 64 in the
256-pixel fixture, rather than 256 by 192.

Root CSS transforms also went through the restricted descendant parser, even
though libflow paints that transform. The adapter now defers CSS transform
conversion to the SVG element that paints it. Window tests reproduce the
three former refusals (`translateX`, `scaleX`, and percentage translation),
verify their actual pixel placement, and retain explicit descendant inheritance.
The new sizing assertion and three window checks failed before the fixes; the
focused SVG integration, 8,200-check window/script suite and freestanding build
pass afterward. ASan, UBSan and leak detection remain enabled.

The second review found the same boundary error for opacity: a calculated
opacity on an HTML wrapper or the outer SVG caused an adapter refusal before
flow could composite it. Opacity values are now retained for explicit
inheritance and evaluated by the adapter for SVG descendants that paint them.
Window regressions cover calculated wrapper and root opacity, the root
explicitly inheriting its wrapper's calculated opacity, numeric descendant
inheritance, initial/unset resets and percentage opacity. The first three
checks failed before the fix. Explicitly inheriting calc opacity into a
descendant still returns unsupported without output or leaked allocations,
matching the documented descendant profile.
The focused SVG integration, 8,221-check window/script suite and freestanding
Yonder/flowdump build pass after this fix, with ASan, UBSan and LeakSanitizer
enabled for the host suites.
