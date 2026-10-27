# Standalone SVG documents

2026-10-27. This first 6c increment connects XML bytes to the existing SVG
renderer through an independently owned document and cascade. It builds on
[object coordinates](SVG_BOUNDS.md) and the [XML tree adapter](XML.md).
It does not connect Yonder's picture jobs or `<img>` dispatch. There is no new
P5-visible feature or gallery in this increment.

## Ownership and interface

`apps/yonder/svg_document.h` exposes an opaque document with parse, render,
cost and free operations. Parse consumes counted bytes through libhtml's XML
builder, checks the root's SVG namespace and case-sensitive local name, and
collects embedded SVG style elements in document order. UTF-8 and BOM-marked
UTF-16 use libxml's existing decoder. The input can be discarded as soon as
parse returns. Failure publishes no document; XML diagnostics retain the
parser status independently of later CSS/profile errors.

The owner retains its tree, parsed sheets and media strings. Render rebuilds
an isolated cascade for a caller-resolved width/height in CSS pixels at
resolution 1, dark preference and reduced-motion preference. No host-page
styles or computed `color` enter this interface. `currentColor` starts at
black and inherits within the SVG. SVG `style` elements accept absent/empty
`type` or `text/css`; their `media` conditions use the image environment.
Calls on one document require owner serialization. No mutable tree is exposed.

Pixels use the caller's geometry-memory ledger, with the existing
`yonder_svg_pixels_free` contract. They can outlive the document. Scene,
backend and output allocations share that ledger; XML/CSS allocations do not.
A caller can render the same document at different sizes without reparsing.
Zero output dimensions return empty success. Oversized dimensions, malformed
XML, allocation failures, cancellation and unsupported painting have explicit
statuses and leave the output empty.

The implementation stays in Yonder: XML documents, CSS and resource policy
belong to the browser, while libsvg consumes resolved styles and geometry.
This keeps the shared geometry API usable by Canvas without requiring Canvas
to manufacture markup. The gradient/text/frame-loop boundaries in
[SVG_BOUNDS.md](SVG_BOUNDS.md#shared-consumers-through-6c) still apply.

## Static image policy and root coordinates

The adapter requests `SVG_MODE_IMAGE`. This is a host-policy input to libsvg;
the document owner enforces policy by exposing no network/file resolver,
page/script context, event loop or stylesheet-loading callback. Scripts and
event attributes are inert. SVG animation elements are skipped, including
`set` beginning at time zero. CSS is cascaded without a motion sampler.
XML stylesheet PIs and CSS imports do not load resources. Fragment references
remain document-local, subject to the existing base-URL refusal; blocked
external `use`/clip references behave as failed resources. Namespace URIs
are identity strings. XLink href is selected by namespace URI and local name,
not the prefix's spelling; an unqualified href, including an empty one, wins.

These choices implement the security boundary of SVG's
[secure static processing mode](https://www.w3.org/TR/SVG2/conform.html#secure-static-mode)
within the supported graphics profile. They do not claim full SVG conformance:
embedded data images, gradients, text and the other recorded graphics debts
remain unsupported. An active `<image>` element is refused without fetching.

Unlike the inline adapter, which leaves outer box effects to libflow, the
standalone root owns its opacity and transform. Opacity isolates the complete
image. The default transform origin is the viewport center, outside viewBox;
explicit origins and individual transform properties remain unsupported.
Root clip paths use the outer viewport box and its coordinate space, while
clips on interior SVG groups use geometric object bounds. The adapter converts
the root clip into the scene's content coordinates, including letterboxing.
This follows the outer CSS-box distinction and was checked against Chromium's
[clip reference-box implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/paint/clip_path_clipper.cc)
and the [CSS transforms origin rule](https://www.w3.org/TR/css-transforms-1/#transform-origin-property).

## Bounds and cancellation

| Resource | Bound |
|---|---|
| XML encoded, decoded and expanded bytes | 4 MiB each |
| XML nesting / tree work | 128 / 4,000,000 units |
| Retained document state, tree and parsed stylesheet arenas | 32 MiB aggregate |
| Collected stylesheet source / sheets | 256 KiB / 16 |
| Stylesheet collection visits | 65,536 |
| Output | 16,384 per axis and 8,388,608 pixels |
| Adapter traversal | 4,096 scene nodes, 128 ancestry levels, 4,000,000 work units |

The retained-document cap is checked after each stylesheet parse. That parse
can temporarily reach libgarb's separate 96 MiB arena cap before rejection;
this is not a 32 MiB peak-memory guarantee. Each render also uses libgarb's
96 MiB cascade-state and 192 MiB read arenas. XML parser bookkeeping has its
own limits. `document_cost` reports retained document/tree/sheet storage,
excluding temporary cascade/render allocations and allocator overhead.
Picture-job admission must account for these separate owners before wiring
untrusted downloads into asynchronous image loading; the render ledger alone
is not a total-document memory budget.

Cancellation is checked by XML parsing, stylesheet collection, adapter work
and the native scene/paint context. CSS parsing/cascade construction is bounded
but non-preemptible within a call; cancellation is checked around those phases.
The callback/context is borrowed for the individual parse or render call.
Failure cleanup releases partial sheets, trees, scenes, backend state and
pixels. The owner does not retain a cancellation pointer.

## Verification

`tools/test_svg_core.py --yonder` passes under ASan/UBSan with LeakSanitizer
enabled outside the sandbox:

- 20 exact standalone/inline flattened-reference pairs: root presentation,
  viewBox, embedded styles/CDATA, namespace aliases, blocked resources, nested
  viewports, viewport-space root clips and object-space group clips.
- Exhaustive failure positions for the representative styled document:
  24 parse allocations, 17 cascade allocations and 33 render allocations
  (render cuts distinguish budget refusal from allocation failure).
- 613 parse and 1,015 render cancellation positions, with unchanged live
  allocation counts and empty failure outputs.
- Input poisoning, pixels surviving owner teardown, repeated-size rendering,
  two independent documents, UTF-16 endian variants, namespace/case refusals,
  malformed XML, media preferences and byte/sheet/output limits.
- The existing inline connection, shape, stroke, instance, viewport, clip and
  object-bounds regressions remain green.

`tools/svg_reftests/documents.py` compares native output with pinned Chromium
140.0.7339.16 using separate SVG `<img>` documents and flat inline references.
All 60 outcomes are expected: 39 positive matches, 20 rejected shifted controls,
and one pinned semantic difference. Chromium's image mode applies the fixture's
SMIL `set` at 0s; our secure-static profile leaves its base blue fill unchanged.
That difference is counted explicitly, not as browser agreement. Root opacity
has a measured one-channel-level envelope over 4,096 pixels. The harness
rejects off-origin requests and page/resource errors, and adds conflicting
host CSS to prove the image stylesheet boundary. Report:
[documents-results.json](../../svg-evidence/documents-results.json).

Yonder's scripted-page suite passes 8,267 checks with sanitizers and leak checks.
Freestanding Yonder/flowdump and their matching libraries build successfully.
The host fixtures exercise the new API directly; this does not claim a guest
or P5 test of an external image-loading route.

## Next integration gate

The next slice is the image route: tagged SVG picture jobs, intrinsic sizing,
content recognition, lifetime/cancellation and admission accounting, followed
by real `<img>` delivery and a P5 gallery. Gradients and cache/corpus work remain
part of 6c. The renderer continues to receive caller-resolved dimensions;
this increment does not implement replaced-element intrinsic sizing or caches.
