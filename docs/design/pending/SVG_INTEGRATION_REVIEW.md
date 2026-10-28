# SVG integration architecture review brief

2026-10-28. Review requested before the SVG integration stack lands on
`userland`. Review the implemented boundaries and their failure modes;
this is not a request to expand the supported SVG profile.

## Scope and reading order

The candidate is `codex/svg-computed-style`, stacked on
`codex/svg-foreign-metadata` (`c029bcc4`, PR #394), above
`codex/svg-integration`. Some foundational SVG commits, including PR #368
and #369, already reached `userland`. A diff against `userland` therefore
omits part of the architecture under review. Read the components below
as a whole; use the diff to locate the latest changes.

1. [SVG.md](SVG.md) and [SVG_REVIEW.md](SVG_REVIEW.md): original boundary
   decisions and the disposition of the first design review. These record
   the proposal; the component documents describe the implemented cuts.
2. [SVG_YONDER.md](SVG_YONDER.md), [SVG_DOCUMENTS.md](SVG_DOCUMENTS.md),
   [SVG_IMAGES.md](SVG_IMAGES.md): inline and standalone document routes,
   style adapters, image jobs, sizing and lifetime.
3. [SVG_GEOMETRY_BACKEND.md](SVG_GEOMETRY_BACKEND.md),
   [SVG_INSTANCES.md](SVG_INSTANCES.md), [SVG_VIEWPORTS.md](SVG_VIEWPORTS.md),
   [SVG_CLIPS.md](SVG_CLIPS.md), [SVG_BOUNDS.md](SVG_BOUNDS.md): coordinate
   systems, instances, clipping and bounds.
4. [SVG_GRADIENTS.md](SVG_GRADIENTS.md), [SVG_RADIAL.md](SVG_RADIAL.md),
   [SVG_CACHE.md](SVG_CACHE.md), [SVG_CACHE_YONDER.md](SVG_CACHE_YONDER.md):
   reusable numeric paint and cache ownership/accounting.

## Questions worth arguing over

| Boundary | Sources | Review question |
|---|---|---|
| XML events → shared DOM | `userland/libxml/`, `userland/libhtml/xml.c`, `userland/apps/yonder/svg_document.c` | Does the parser stay independent of a tree, with expansion/work/memory bounded and partial construction released? |
| Image document → browser | `svg_document.c`, `picture.c`, `yonder.c` | Can an SVG reached as an image fetch a subresource, run script, or borrow the embedding page's styles? Does teardown release dependent state before its document? |
| Cascade → graphics and CSSOM | `userland/libgarb/`, `userland/apps/yonder/svg.c`, `userland/libflow/computed.c` | Are inheritance, presentation attributes, CSS-wide keywords, currentColor and URL origins consistent across the two consumers? Where should shared computation live as this grows? |
| Local coordinates → device coverage | `userland/libsvg/geometry.c`, `stroke.c`, `bounds.c`, `paint.c` | Are strokes expanded in their proper space, including anisotropic transforms? Do geometric bounds exclude effects that must not affect object-bounding-box coordinates? |
| Instance → referenced source | `userland/apps/yonder/svg.c`, `userland/libsvg/scene.c` | Does use styling follow the instance ancestry, with bounded recursion and cycles refused? Do clip and gradient references use the right instance/viewport context? |
| Prepared scene → reusable pixels | `userland/libsvg/cache.c`, `userland/apps/yonder/svg.c`, `yonder.c` | Is reuse keyed by the complete rendered value, with no DOM pins? Are evictions safe while outputs remain independently owned, including allocation failures? |
| SVG → future Canvas | `userland/libsvg/include/svg/svg.h`, `gradient.c` | Can a non-markup caller use numeric geometry and paint without inventing a DOM? Are document/reference semantics kept in the adapter? |

The original pitfalls remain useful adversarial examples: `scale(4,1)` on a
round cap, use styling that differs from the defs ancestry, a CSS transform
that replaces its attribute, changing a shared gradient after a cache hit,
and destroying a document while a separately owned pixel result survives.

## The CSSOM regression and this repair

PR #368 (`cf1ba15f`) added SVG properties to the list exposed by
`getComputedStyle`, but libflow's serializer had no initial/computed-value
cases for them. The existing WPT `css/cssom/serialize-all-longhands.html`
therefore lost its **Computed style** pass. PR #369 added geometry names to
that gap. The accidental early merge exposed the defect on `userland`;
it did not create it.

This branch supplies the integration profile's 24 SVG properties: initial
values, CSS-wide inheritance, numeric and color computation, URL origins,
and serialization. Relative lengths compute at the declaring ancestor;
currentColor resolves on the receiving element. SVG descendants receive
the font/color context needed by CSSOM without becoming HTML layout boxes.
The numeric work uses libflow's existing unit/calculation rules. The
renderer remains a separate consumer of cascade winners: this repair does
not unify the two style adapters or claim general SVG DOM/CSSOM coverage.

The WPT expectation stays unchanged. Its separate **Specified style**
failure predates the SVG change. The cascade's separate, pre-existing lack
of the `all` shorthand is recorded in DEBTS.md: this WPT checks nonempty
values, not whether inherited values were reset. The host regression uses
explicit longhand resets and covers HTML and SVG nodes, hidden descendants, reset/inherited values, percentages, dashes,
paint URLs/fallbacks, short output buffers and zoom-independent CSS pixels.

## Evidence and limits

Validation of this repair on 2026-10-28:

- Focused CSSOM host cases: 393 checks, zero failures.
- Full libflow suite: 24,080 checks, zero failures, including allocation
  sweeps and the sampled layout fuzz corpus.
- Browser script/window suite: 10,991 checks, zero failures.
- Native SVG integration: reference pixels, mutations, allocation and
  cancellation cuts, processing modes and ownership checks passed.
- Full CSSOM WPT directory: 954 passes and no expected passes lost. The
  directory still has 879 failures, 18 timeouts and 36 not-run results;
  this is not a claim of complete CSSOM support. **Computed style** passes.
- Os64 cross-build of `libflow.so` and its required dependencies succeeded.

Host runs used ASan/UBSan with LeakSanitizer enabled; libflow also enables
float-cast-overflow checks. No WPT expectation was changed.

The component documents and `docs/svg-evidence/` retain fixture, allocation,
reference-renderer and P5 evidence. Chris has additionally accepted the
Rust and Debian SVG fixes, the Wikimedia SVG examples, and roughly 150
OpenSVG images during browsing. Those are compatibility samples, not a
complete SVG conformance result.

Useful gates for this review are `tools/test_libflow_host.sh`,
`tools/test_yonder_scripts_host.sh`, `tools/test_svg_core.py --yonder`,
and the CSSOM WPT directory through `tools/test_yonder_wpt_host.sh`.
Run sanitizer gates with leak detection enabled. Existing allocation-failure
sweeps and processing-mode tests are especially relevant to the ownership
and image-isolation boundaries.

Known limits are recorded in [DEBTS.md](../../../DEBTS.md): synchronous scene
preparation/raster work, preparation even on raster-cache hits, bounded
backend quotas, incomplete intrinsic width/height/ratio negotiation, and
image-route extensions such as SVGZ and fragment views. Text, patterns,
masks, filters, foreignObject layout and animation are outside the accepted
static profile. The Debian header's Adam7 PNG refusal is a separate raster
decoder debt, not an SVG failure.

Canvas is a prospective second caller. Numeric gradient sampling is
available; text shaping/outlines still need their own implementation and
contract. Canvas is not promised to be merely a thin wrapper: its mutable
state, script API, pixel access and compositing semantics need design.
A future SVG animation implementation should use the browser's shared
frame scheduling rather than create a competing loop.
