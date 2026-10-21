# SVG.md — a picture drawn from a tree: the renderer's seam

*Slice 6 of [MODERN_WEB.md](MODERN_WEB.md). Design by Fable, 2026-10-21,
from a read of libhtml, libgarb, libflow, libimage, libfreetype's port and
yonder's painter at `userland` 44768c2f. The census put inline SVG first
by sites: every Google and GitHub icon, 140 asks, and `MISSING image svg`
beside them. Opus builds it in three cuts; Fable reads the seam before the
first merges. Chris rules and tests.*

## What this settles

1. **To the layout, an SVG is a picture.** An `<svg>` element among HTML
   is a REPLACED box, sized by SVG's own sizing rules (its `width`,
   `height`, `viewBox` and the default object size), laid out exactly as
   an `<img>` is: inline and atomic by default, block when styled so,
   `object-fit` and friends applying. Its descendants make no box; the
   renderer walks them. `<img src="x.svg">` is the same picture with the
   tree parsed from the file. Nothing in libflow learns what a `<path>`
   is.
2. **The renderer is a library, libsvg, that takes a tree and gives back
   pixels.** In: an element in the SVG namespace, a provider that answers
   each element's computed paint properties, a provider that shapes and
   draws text, a size in device pixels. Out: straight-alpha ARGB, the
   format every picture in the house already is (`os64_image_t`). It
   allocates through the caller's hooks, reads no file, opens no socket,
   knows no window, and is host-tested under ASan like every library
   before it.
3. **Filling and stroking are FreeType's**, reached through one new verb
   on the font backend table. The tree already carries FreeType's smooth
   rasterizer (`smooth.c`, 256-level coverage, nonzero and even-odd) for
   glyphs, and its stroker (`src/base/ftstroke.c`, imported with the base
   module and simply not listed in `sources.mk` today) does caps, joins and
   miter limits on curves, which is SVG's stroke model to the letter and a
   month of subtle bugs to rewrite. The backend table's contract already
   says a consumer checks `revision` and `struct_size`; this is the revision
   that adds `outline_render`. No FreeType type crosses the seam: the
   outline is os64's own struct, converted inside the port.
4. **Styling is the cascade's.** SVG's presentation attributes (`fill=`,
   `stroke=`, `transform=`) are author declarations of specificity zero
   (SVG 2 § 6.5), read by libgarb beside the `style` attribute; `fill`,
   `stroke` and the rest become properties in libgarb's table with slice
   0a's mark; type selectors match SVG elements by their local name,
   case-sensitively, so a page's `svg path { fill: currentColor }` works.
   The renderer never parses CSS.
5. **Rendering happens at paint, on the window thread, cached like a
   gradient.** The box's content size is known only when the page is laid
   out, so the pixels are made when the painter reaches the box, kept in
   the paint cache under the node, the document version, the size and the
   zoom, and found again next paint. An `<img src=svg>`'s TREE is parsed
   on the worker as any picture is decoded; its pixels are made on the
   window thread.
6. **The oracle is pictures compared with pictures.** Slice 0a's runner
   gains reftests: a test page and its reference painted by the headless
   painter and compared with WPT's own fuzziness annotations. That runner
   serves slice 4's and slice 8's CSS reftests too, which is why it comes
   first here.

## The seam, exactly

**libflow.** One change in `boxes.c`: an element in the SVG namespace whose
PARENT is an HTML element is replaced (`replaced()` says yes), like `img`;
its subtree is walked for nothing. An SVG element under an SVG element
stays `display: contents` as today (nothing to lay out). `style.c`'s
"an SVG element draws nothing here" becomes "an SVG root is a picture;
what is inside it is the renderer's". The replaced size comes through the
oracle libflow already asks (`flow_env_t.replaced_size`) and yonder
answers it for an `<svg>` from the element itself; `flow_replaced_fixed`
reads the box's style as it does for a picture.

**SVG sizing, which yonder's oracle implements** (CSS Images 3 § 4 and SVG
2 § 8.2, the rules browsers use for an `<svg>` and an `<img src=svg>`):
`width`/`height` attributes in px (or a unit libgarb's lengths know) give
the intrinsic size; a percentage or an absent one gives none on that axis;
`viewBox` gives the intrinsic RATIO; with a ratio and one size the other
follows; with neither, the default object size, 300 by 150. CSS `width`
and `height` on the element override as for any replaced box.

**libsvg's door** (`userland/libsvg/include/svg/svg.h`):

```c
/* What an element paints with and how, as the cascade computed it: the
 * renderer asks per element, in render order, and never caches across
 * calls (the caller's cache is the picture). */
typedef struct {
    os64_svg_paint_t fill, stroke;      /* none | colour (0xAARRGGBB) | a paint server by node */
    double fill_opacity, stroke_opacity, opacity;
    os64_svg_fill_rule_t fill_rule, clip_rule;
    double stroke_width;                /* user units */
    os64_svg_linecap_t linecap; os64_svg_linejoin_t linejoin; double miterlimit;
    const double *dashes; int32_t ndashes; double dashoffset;
    const os64_html_node_t *clip_path;  /* the element clip-path names, or NULL */
    bool hidden;                        /* display: none, or visibility: hidden */
    double transform[6];                /* the element's own, CSS and attribute composed */
    /* text: the font the cascade computed, as libflow's family list */
    const flow_family_list_t *family; double font_px; bool bold, italic;
    os64_svg_anchor_t anchor;
} os64_svg_style_t;

typedef struct {
    void *ctx;
    /* The element's computed style; false when it cannot be answered (the
     * element is then skipped, not guessed). */
    bool (*style)(void *ctx, const os64_html_node_t *element, os64_svg_style_t *out);
    /* Text: shape `utf8` in the style's font and hand back the glyph run's
     * advances and, per glyph, its coverage bitmap at the device scale —
     * libos64's text engine behind yonder's own provider. */
    bool (*shape)(void *ctx, const os64_svg_style_t *s, const char *utf8, size_t len,
                  double scale, os64_svg_run_t *out);
    void (*run_release)(void *ctx, os64_svg_run_t *run);
    /* The rasterizer: the font backend's outline verb, or a test stand-in. */
    const os64_font_backend_t *backend;
    os64_font_engine_t *engine;
    /* Memory: what the renderer may hold at once, and how it allocates. */
    size_t max_bytes;
    void *(*alloc)(void *ctx, size_t bytes);
    void (*free)(void *ctx, void *p, size_t bytes);
} os64_svg_host_t;

/* Render `root` (an element in the SVG namespace, the outermost <svg>)
 * into `width` x `height` device pixels, straight-alpha ARGB, the viewport
 * mapped by viewBox and preserveAspectRatio. The pixels are the host's
 * (alloc) and are freed by the caller with the host's free. */
os64_svg_status_t os64_svg_render(const os64_svg_host_t *host, const os64_html_node_t *root,
                                  uint32_t width, uint32_t height, os64_image_t *out);
/* The intrinsic size and ratio of `root`, for the layout's oracle: each
 * `has_*` false when the attributes give none. Needs no host. */
void os64_svg_intrinsic(const os64_html_node_t *root, os64_svg_intrinsic_t *out);
```

**The backend's new verb** (`os64/font_backend.h`, revision 2):

```c
/* An outline in 26.6 device coordinates, y down: `points` and one `tag`
 * each (ON, CONIC, CUBIC, FreeType's own vocabulary), `contours` the index
 * of each contour's last point. */
typedef struct {
    const os64_font_pos_t *x, *y; const uint8_t *tags; const uint16_t *contours;
    uint32_t npoints, ncontours;
} os64_outline_t;
/* Coverage of the outline's filled area (nonzero, or even-odd) inside a
 * `width` x `height` bitmap whose top-left is the outline's (0, 0): one
 * byte per pixel, rows `width` apart, written whole. `stroke` non-NULL
 * strokes the outline instead — width in 26.6, cap, join, miter limit —
 * through FreeType's stroker, the dashes having been cut by the caller.
 * BAD_ARGUMENT for an outline FreeType will not take; LIMIT past the
 * engine's memory. */
os64_font_status_t (*outline_render)(os64_font_engine_t *, const os64_outline_t *,
                                     bool even_odd, const os64_outline_stroke_t *stroke,
                                     uint32_t width, uint32_t height, uint8_t *coverage);
```

Fill and stroke of one shape are two coverage bitmaps composited in turn,
fill first, each tinted by its paint and opacity. A dash array is applied
by libsvg on the flattened path before the stroker sees it (FreeType does
not dash), which is also where `stroke-dashoffset` lives.

**yonder.** Three touches. The painter's `image` verb, reaching a replaced
box whose node is an `<svg>` (or an `<img>` whose picture is a parsed SVG
tree), asks the paint cache for the pixels under (node, document version,
content width, content height, zoom, dark-pages state) and, missing,
renders through libsvg with a host that answers `style` from the page's
cascade (`garb_style_for` and libflow's computed longhands for the font),
`shape` through the page's text context, and `backend` from the text
engine's; then draws them as it draws a picture (`yonder_draw_picture`,
blending the alpha). The picture job, on bytes whose first bytes say SVG
(`<?xml` or `<svg`, with the content type as the tie-breaker), parses them
with libhtml into a private document owned by the picture (the product
gains a `tree` beside `image` and `sequence`), and the oracle answers its
size from `os64_svg_intrinsic`. The replaced-size oracle answers an
inline `<svg>` from the node. **A standalone SVG has its own cascade**
(Quinn's finding 10): the page's cascade knows nothing of the private
document's nodes (`garb_style_for` answers an empty style for a node it
never saw), so the picture owns a `garb_cascade_t` built on the window
thread at first render over the private document's own `<style>`
elements and presentation attributes, at the page's environment, freed
with the picture; the host's `style` answers from whichever cascade the
element's document owns. An inline `<svg>` uses the page's.

**libgarb.** The paint properties, marked `modern`: `fill`, `fill-opacity`,
`fill-rule`, `stroke`, `stroke-width`, `stroke-opacity`, `stroke-linecap`,
`stroke-linejoin`, `stroke-miterlimit`, `stroke-dasharray`,
`stroke-dashoffset`, `clip-rule`, `stop-color`, `stop-opacity`,
`text-anchor`, `dominant-baseline` (the two values icons use);
`currentColor` resolving to `color` as elsewhere. Presentation attributes
read as declarations of specificity zero below every sheet. Type selectors
and attribute selectors matching SVG elements by local name, case kept.
The cascade already walks every element whatever its namespace.

**libhtml.** Already adjusts foreign names (`viewBox`, `foreignObject`) per
the standard's tables (`tree.c`'s `adjust`). An SVG file parsed as a page
gets the HTML parser's treatment: `<svg>` as the root of a fragment in the
SVG namespace, CDATA honoured in foreign content, entities, processing
instructions dropped as comments. That is not an XML parser, and the
as-built says so with the cases it fails (a DOCTYPE with internal entities,
namespace prefixes other than `xlink:`); an XML parser is booked with the
first real file that needs one.

## What the renderer draws (slice 6's whole)

- **Structure:** `svg` (nested too), `g`, `defs`, `symbol`, `use`
  (`href`/`xlink:href` to an element in the same document, with the
  referenced subtree rendered at `x`,`y` and sized for a `symbol`), `title`
  and `desc` skipped, `switch` as its first child.
- **The viewport:** `viewBox` and `preserveAspectRatio` (every `xMid`
  spelling, `meet` and `slice`, `none`), nested viewports for inner `svg`
  and `symbol`, `overflow` clipping the viewport as SVG says.
- **Shapes:** `rect` (with `rx`/`ry`), `circle`, `ellipse`, `line`,
  `polyline`, `polygon`, `path` with the whole path grammar: every command
  absolute and relative, the shorthands, arcs converted to cubics by the
  standard's endpoint-to-centre algorithm.
- **Transforms:** the `transform` attribute (`matrix`, `translate`,
  `scale`, `rotate` with a centre, `skewX`, `skewY`, lists), composed with
  CSS `transform` on the same element; `transform-origin` as CSS says.
- **Paint:** solid colours in every CSS syntax libgarb reads, `none`,
  `currentColor`, opacity at the three levels (`opacity` as a group,
  `fill-opacity`, `stroke-opacity`), `fill-rule` and `clip-rule`; strokes
  with width, caps, joins, miter limit, dashes and offset; `vector-effect:
  non-scaling-stroke` honoured (icons use it).
- **Clipping:** `clip-path` naming a `clipPath` of shapes (the union of its
  children's coverage, `clipPathUnits` both ways), applied as a coverage
  multiply.
- **Gradients:** `linearGradient` and `radialGradient` with `stop`s,
  `gradientUnits`, `gradientTransform`, `spreadMethod`, `href` inheritance
  between gradients: the paint server fills the shape's coverage with the
  gradient evaluated per pixel (yonder's own gradient code is CSS's shape;
  the renderer writes its own small evaluator, since a radial with a focal
  point is SVG's and not CSS's).
- **Text:** `text` and `tspan` with `x`, `y`, `dx`, `dy`, `text-anchor`,
  `dominant-baseline`'s common values, shaped and drawn through the host's
  text provider as glyph coverage bitmaps placed along the baseline: the
  untransformed and uniformly scaled cases, which are what labels in
  diagrams are. ROTATED text waits for an outline export from the font
  engine (the backend table has none; booked with the first page whose
  rotated label matters).
- **Opacity groups:** a `g` or shape with `opacity` below 1 renders its
  children into a scratch layer and composites once, as CSS groups do.

**Not drawn, each booked with a trigger:** `image` (a raster inside the
SVG: the picture job's second fetch), `pattern`, `marker`, `mask`,
`filter` (`feGaussianBlur` is the one pages use, with `drop-shadow`
already in yonder's painter the day a page asks), `foreignObject`
(HTML inside SVG: a layout inside a picture), `textPath`, SMIL animation
(never: CSS animations on SVG elements come free with the cascade), `a`
inside SVG as a link (hit testing into a picture: with the first page that
needs it), scripting of SVG DOM interfaces (`SVGElement` and the rest are
the DOM arc's; the interface objects slice adds `SVGElement` and
`SVGSVGElement` as Illegal constructors over the generic element prototype
so feature detection answers yes).

**Bounds.** A render is capped by the picture cap (an SVG at a box larger
than `OS64_IMAGE_DIM_MAX` on a side is rendered at the cap and scaled, as
a picture would be); the renderer's working memory by `max_bytes` (a
`use` graph is depth-limited, 32, and reference cycles refused by name);
a path's point count and a dash array's length by stated limits that make
the fixture that breaks them. Coverage bitmaps are per shape, never a
whole-picture layer except for an opacity group or a clip.

## The oracle: reftests, and the renderer's own

**The reftest runner** (6a, on slice 0a's runner): a WPT reftest names its
reference in `<link rel=match>` (or `mismatch`); the runner lays out and
paints both pages with the headless painter at the standard's 800 by 600
viewport and compares pixel by pixel, honouring `<meta name=fuzzy
content="maxDifference=N;totalPixels=M">`. The painter already paints
pixels in the harness (the paint-cost cases); what is new is the compare
and the reference's load. The expectation file lists the passing reftests
as it lists testharness tests.

**The renderer's host suite** (`tools/test_svg_host.sh`): for each fixture
SVG a reference PNG, rendered once by a browser Chris trusts (headless
Chromium in WSL, the house's render path), compared with a tolerance
suited to anti-aliasing differences: a pixel counts as wrong when its
channels differ by more than 16 AND it is not within one pixel of an edge
(an edge pixel is one whose coverage neighbours differ); the fixture
passes under 0.1% wrong pixels. Besides pictures, exact checks: the
bounding box of every shape's coverage against the fixture's annotation,
the path parser's output on the SVG 2 grammar's own examples, the arc
conversion against the standard's worked example, every allocation failed
in turn, and the mutants' targets: a wrong fill rule, a dash cut in the
wrong space, a `use` cycle not refused.

**WPT directories:** `svg/shapes/reftests/`, `svg/painting/reftests/`
(the stroke and marker ones; markers will fail and say so),
`svg/struct/reftests/` (`use`, `symbol`, nested `svg`),
`svg/coordinate-systems/`, `svg/path/`, `svg/pservers/reftests/`
(gradients), `svg/text/reftests/` (the subset the text provider draws),
`css/css-transforms/` for the SVG cases. Acceptance: Google's and GitHub's
icons draw; suckless's logo; the `MISSING element svg` and `MISSING image
svg` lines leave the records (the tally).

## The cut

| Cut | Builds | Oracle | Owner |
|---|---|---|---|
| 6a | the reftest runner on 0a's runner, with `fuzzy`; the `svg` directories in `tools/wpt_fetch.sh` | the runner proves itself on CSS reftests that pass today (`css/css-sizing/`'s simplest), and on a hand-made pair that must fail | Opus; after 0a |
| 6b | the backend verb and the stroker module; libsvg: the door, the viewport, shapes, paths, transforms, solid paint, strokes, dashes, clips, `use`; libgarb's properties and presentation attributes; libflow's replaced `svg`; yonder's oracle, painter hookup and the picture job's tree | `test_svg_host.sh`; the reftests under shapes, struct, painting, coordinate-systems, path | Opus; Fable reads the door and the backend verb before merge |
| 6c | gradients, text, opacity groups, `<img src=svg>` end to end, the dark-pages interaction (an icon's `currentColor` follows the page's) | `svg/pservers/`, `svg/text/`; the icons on the P5 beside Chrome | Opus |

6a can start the day 0a lands and runs beside the fan-out. 6b waits for
slice 0a's mark in libgarb; 6c follows 6b.

## Risks named

- **FreeType's stroker on degenerate input** (zero-length segments, a
  miter of a near-180-degree join) returns errors rather than garbage; the
  port maps them to a skipped stroke and the renderer records a `FAILED
  svg` line through the page file so a blank icon is explained.
- **Parsing SVG files with an HTML parser** is the honest shortcut above;
  the fixture suite includes real icons saved from the census's sites so
  the shortcut is measured, not assumed.
- **Hit testing**: an inline SVG is one box to `flow_hit`; a link drawn
  inside it is not a link yet. Said in the as-built, booked with its page.
- **Memory**: every icon's pixels sit in the paint cache under the cache's
  own budget; a page of a thousand icons pays a thousand small renders on
  its first paint and reads the cache after, which is what the paint-cost
  instrument will show.
