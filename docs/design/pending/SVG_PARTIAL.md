# SVG partial artwork

2026-10-29. Implements the approved policy in finding 3 of
[the integration review](SVG_INTEGRATION_REVIEW.md#disposition-of-fables-architectural-review).
The branch stacks on the CSSOM repair; scene retention and other performance
findings are separate work.

## Behavior and boundary

A supported drawing no longer disappears because a sibling uses text, an
embedded image, foreignObject, or another drawing element the profile cannot
render. Yonder omits that entire subtree before resolving its paint, transform,
geometry or descendants. A use instance targeting such an element is omitted
at that occurrence, preserving the use's siblings. This is a documented
partial-rendering policy, not a claim of full SVG conformance.

Yonder's provider returns display:false for the omitted occurrence. libsvg's
strict provider contract and failure statuses remain unchanged. A root with
unsupported paint, recognized unsupported effects on supported elements, or
unsupported referenced clip/gradient contents still fails without publishing
pixels. Parser failures, limits, allocation failures and cancellation remain
failures. Unsupported content inside a clip is not silently dropped: that
could change what supported artwork the clip exposes.

Definitions that do not directly paint (including pattern, mask, filter and
marker elements) are ignored when encountered as ordinary drawing children.
That does not implement their resource semantics. A shape referencing an
unsupported pattern still refuses the root. Script elements do not paint;
the browser's existing script processing and secure-static image isolation
remain separate. No embedded SVG image fetch is introduced.

## Diagnostics and cache behavior

A successful yonder_svg_pixels_t carries `omitted_subtrees`, a bounded count
of unsupported drawing occurrences, rather than all descendants beneath them.
Explicit display:none occurrences and content skipped by existing inert or
non-rendering rules do not count. For a use instance whose source display
uses variables, the omission is conservatively counted: the DOM's substituted
style is not an instance-style oracle. The count describes omissions, not an
estimate of how many pixels would have been visible.

The window records `MISSING svg` or `MISSING svg image`, with the maximum count
seen in a single successful render during the load. This is not a page-wide
census and repainting does not add to it. Failed renders keep `FAILED` and
return no successful omission count. Freeing an output clears the count along
with its pixels.

The raster cache still keys supported prepared artwork by value. Omitted
content need not enter that pixel key: fresh preparation provides the current
omission count even on a hit. Cached pixels remain independently owned.

## Verification

`tools/test_yonder_svg_partial.inc` compares partial artwork byte for byte
against the supported drawing alone: text, external images, foreignObject
subtrees containing SVG, unknown SVG containers, unsupported styles on omitted
nodes, repeated use instances, hidden content and unused resources. Inline
cases cover 25%, 50%, 100%, 200% and 400%; isolated image cases use the same
reference artwork. Controls require refusal for pattern paint, mask/filter
attributes, unsupported clip silhouettes and non-stop gradient content.

The full native SVG integration suite passes with ASan, UBSan and
LeakSanitizer, including 33 allocation positions in both refusal modes and
570 cancellation positions for partial artwork. Browser integration passes
11,014 checks, including the real inline/image painters, omission diagnostics,
cache hits, mutation and refusal diagnostics. The Os64 Yonder build passes.

The gallery is `/tests/pages/svg-show-goes-on.html`, with Modern Web enabled;
Scripts enables its two buttons. The first boat remains visible when its
label switches between omitted text and a drawn checkmark. The second boat
keeps its unsupported foreignObject subtree out of the image. The postcard
keeps its lighthouse without fetching its embedded picture. The fourth frame
is empty with unsupported pattern paint and shows a rounded rectangle with
solid paint. QEMU checks passed at 25%, 100% and 400%, including both
button round trips. Hardware acceptance is pending.

[Results](../../svg-evidence/partial-results.json) and the
[frozen delivery manifest](../../svg-evidence/partial-delivery.sha256) record
the tested cohort: 41 files, with 37 reused unchanged from the prior delivery.
