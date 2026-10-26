# SVG in Yonder — first inline connection

2026-10-26. This increment connects the native [solid painter](SVG_PAINT.md)
to real HTML documents, the CSS cascade, layout and Yonder's window compositor.
It is the first P5 checkpoint, not completion of the static SVG profile in
[SVG.md](SVG.md). The fixture is `tools/pages/svg-inline.html`, installed by
`make stage-pages` as `/tests/pages/svg-inline.html`.

## What reaches the screen

With Modern Web enabled, an outer inline SVG creates a replaced CSS box.
`libflow` owns its placement, CSS dimensions, root CSS transform and root
opacity. Its intrinsic-size callback reads absolute width/height and viewBox
ratio, with the 300 by 150 default object size. The adapter maps viewBox using
`none`, the nine alignments, `meet` or `slice`; the viewport clips the output.
Page zoom scales user units, including an outer SVG transform's translation.

The SVG tree/style provider reads the existing libhtml document. Groups and
paths support solid fill and stroke, currentColor, fill rules, fill/stroke
opacity, isolated group/element opacity, cap/join/miter settings, visibility
and SVG transform attributes. Supported CSS 2D transforms replace the transform
attribute. The outer box's transform and opacity are painted once by Yonder;
their computed values remain available to explicit inheritance inside SVG.
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
units, 4,096 bytes per viewport/transform attribute and 1 MiB per path string.
Output is limited to 16,384 pixels on either axis and 8,388,608 pixels total,
then admitted against the shared byte cap. It allocates the visible crop using
the full viewport's map. Native SVG and geometry limits apply in addition.
There is no scene/path cache or asynchronous cancellation integration. A dirty
paint prepares and renders again, synchronously. These caps bound admitted
work; they are not a demonstrated frame-time guarantee for arbitrary pages.

## Explicit limits

This connection handles paths and groups, with definitions, metadata, titles,
descriptions and style elements omitted from drawing. Other active element
kinds refuse the SVG job. Shapes, use/symbol, nested viewports, text, gradients,
patterns, images, markers, clip paths, masks, filters, dashes, non-scaling
strokes and animation remain follow-on work. External SVG image decoding is
not connected to libimage. SVG-specific hit testing and DOM interfaces are
also outside this slice.

Numeric and px stroke widths and percentage widths are supported; the latter
use the normalized viewBox diagonal. Relative lengths and calc expressions
are refused by the adapter. Descendant CSS transform-origin and individual
translate/rotate/scale properties are refused. Color interpolation is sRGB.
The native painter's degenerate-stroke and arc-coverage limits still apply.

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
Modern Web. Unsupported elements/paints/dashes return no output. It sweeps
**81 allocation positions in both admission and allocator refusal modes**,
with recovery and zero live ledger bytes after each call. Its fixture peak is
**233,084 bytes**, not a real-page performance estimate. Source/header/compiler
and binary provenance accompanies the run.

The existing Yonder suite passes **2,028 checks** and the full window/script
suite passes **8,174 checks**. The garb parser/cascade regression suite passes.
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
that setup failure without a kernel or renderer change. No P5 execution or
broad browser/WPT equivalence is claimed by this checkpoint.

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
