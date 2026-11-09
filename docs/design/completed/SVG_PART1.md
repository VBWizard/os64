# SVG Part 1: static graphics integration

2026-10-29. The accepted static SVG profile is implemented and its feature
slices are reviewed and merged into `codex/svg-integration`. This closeout
accompanies the final integration PR to `userland`; it does not assert that
the final PR has already merged. [SVG.md](../pending/SVG.md) remains the design
for the wider arc, including later graphics and interaction work.

## Delivered profile

Yonder renders supported inline SVG, SVG images and SVG backgrounds with
Modern Web enabled. The profile includes paths and basic shapes, transforms,
fills and strokes with dashes, fragment use instances, symbol/nested viewports,
bounded silhouette clips, opacity groups, linear and radial gradients, and
bounded raster reuse. Script-driven DOM changes can update supported artwork.

Standalone SVG uses the XML/shared-tree adapter and an isolated cascade. An
SVG loaded as an image runs no scripts and fetches no embedded resources.
The renderer stays in C behind numeric geometry/paint interfaces; FreeType
provides outline geometry and coverage. Yonder owns document identity,
reference/style resolution, loading policy and presentation.

Unsupported drawing subtrees are omitted as a whole and reported as partial
support. Unsupported paints/effects on supported elements and unsupported
referenced clip/gradient content still refuse the render. This policy preserves
usable artwork without claiming to render text, embedded images or
foreignObject content. CSSOM supplies the integration profile's 24 SVG
computed properties; the separate `all` shorthand remains a documented debt.

## Final reconciliation

The release candidate combines SVG integration `f9b39a6e` (through PR #399)
with userland `837f5c33` (through PR #397). Merge commit `12d9bda4` retains both
histories. The sole textual conflict was the host harness source list: the
resolution includes current DOM modules and the complete SVG/image pipeline.
No application-code conflict required a new rendering behavior. Two stale
stylesheet-predicate references were corrected to `os64_page_names_sheet`.

Combined-tree verification, with ASan/UBSan and LeakSanitizer enabled:

| Gate | Result |
|---|---|
| Browser script/window integration | 11,687 checks, zero failures |
| libflow, including allocation sweeps and sampled fuzz | 24,093 checks, zero failures |
| libpage, including stylesheet census and allocation sweep | 254,352 checks, zero failures |
| Native SVG integration with production FreeType/math | Passed reference pixels, mutation, ownership, refusal and allocation/cancellation coverage |
| CSSOM WPT against current userland expectations | 1,107 passes; no expected passes lost; `serialize-all-longhands.html` Computed style passes |
| Freestanding Yonder and flowdump | Built successfully with their matching libraries |

CSSOM still reports 1,284 failures, 18 timeouts and 36 not-run results. These
are retained outcomes, not a full-conformance claim; no WPT expectation was
changed by this release reconciliation. libflow also enables
float-cast-overflow checking. [Release results](../../svg-evidence/part1-results.json)
record the combined build and smoke-test evidence.

Chris accepted the individual galleries on the P5, including the final
partial-artwork gallery and both buttons on 2026-10-29. Browsing acceptance
also includes the corrected Rust logo and Debian SVG icons, Wikimedia SVG
examples and approximately 150 OpenSVG images. These are compatibility
samples, not measured coverage or throughput. Final combined binaries passed a QEMU smoke check: inline boats, the
isolated image, both button round trips, and 25%/100% views, followed by a
clean guest shutdown. The earlier P5-tested frozen bundle is retained.

## Deferred work and Canvas handoff

Text shaping and glyph outlines, markers, embedded images, patterns, masks,
filters, foreignObject layout, SVG DOM geometry/hit testing and animation
remain follow-ons. Image sizing is the documented default-object profile;
full intrinsic sizing, data URLs, SVGZ and fragment-view selection remain open.
The Debian header's Adam7 PNG limitation belongs to the raster decoder.

[Fable's architectural review](../pending/SVG_INTEGRATION_FABLE.md) and
[its disposition](../pending/SVG_INTEGRATION_REVIEW.md#disposition-of-fables-architectural-review)
retain the performance work: bounded scene/cascade retention first,
scroll-stable raster admission, then shared reference lookup. Raster-cache hits
still prepare scenes, and rendering is synchronous. No broad latency or
memory-performance target is claimed by this closeout. DEBTS.md tracks these
limits and the opportunity to share typed computed-style semantics.

Canvas starts with a design conversation. It can reuse numeric paths,
geometry and gradient sampling, but mutable drawing state, compositing,
pixel access, script bindings and shared text shaping need explicit contracts.
Later SVG motion should share the browser's frame scheduling with Canvas.
Closing Part 1 does not start either implementation automatically.
