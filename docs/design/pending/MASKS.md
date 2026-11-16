# Masks (CSS Masking 1 § 7): the spinner arc's third slice

Status: design, read and ruled by Chris (2026-11-11, § 10). Opus.

## 1. Why, measured

After slices 1 (`aspect-ratio` definiteness) and 2 (conic gradients, one
colour with two positions), Chris found most of the spinner site's
animations drawing (his estimate: 90%). What is still wrong on its spinner
page is masks: 4, 5, 26 and 28 need one to show their shape at all, 13, 18,
19 and 25 cut a ring or a fade with `-webkit-mask` and draw without the cut,
and 3, 4 and 5 combine two mask layers with `mask-composite`.

The corpus agrees, and the reach is far past spinners. Of the current
corpus's 171 hosts (1852 page loads, `~/os64-opus-kit/tools/corpus_css_scan.py`
and a mask profile beside it):

| What | Hosts |
|---|---|
| any mask property | 84 |
| `mask-image` / `-webkit-mask-image` | 68 / 61 |
| `mask` / `-webkit-mask` shorthand | 45 / 44 |
| `mask-repeat`, `-size`, `-position` (each, either name) | ~40 |
| `mask-composite` / `-webkit-mask-composite` | 24 / 23 |
| `mask-mode` | 14 |
| `mask-clip` / `mask-origin` | 8 / 4 |
| mask images: a linear gradient | 57 |
| mask images: an SVG by `data:` URL / an SVG file | 31 / 17 |
| mask images: a radial gradient | 24 |
| mask images: a conic gradient | 4 |
| `url(#id)`, a reference to an SVG `<mask>` | 2 |

The SVG rows are the icon pattern: `background-color: currentColor` and
`mask-image: url(icon.svg)`, which draws the icon in the text's colour.
Today such an element paints a solid coloured square, which is worse than
no icon. The gradient rows are fades (a list fading out at its edge) and
the spinners' rings.

## 2. What a mask is

A mask is **a stack of image layers that says how much of an element shows**
(CSS Masking 1 § 7). Each layer is an `<image>` (a `url()`, a gradient)
placed exactly as a background layer is — `mask-position`, `-size`,
`-repeat`, `-origin`, `-clip` mirror the `background-*` longhands — and read
as coverage: by default its alpha (`mask-mode: match-source` is alpha for an
image or a gradient), or its luminance (`mask-mode: luminance`). The layers
combine bottom to top by `mask-composite` (`add`, the default, is
source-over; `subtract`, `intersect` and `exclude` are source-out, source-in
and xor). What the element paints — its background, border, shadows, content
and every descendant — is multiplied by the result. Outside the mask
painting area (`mask-clip`, the border box by default) nothing shows.

Two initial values differ from backgrounds' and are the usual trap:
`mask-origin` and `mask-clip` are `border-box` (backgrounds' origin is
`padding-box`). A mask that is not `none` makes the element a stacking
context and a group (§ 7.1), as opacity below 1 does.

## 3. The model: a masked box is a group, its mask applied at the layer's close

yonder already has the machinery, built for `filter` and `mix-blend-mode`
(FILTER.md § 4, MIX_BLEND.md):

- libflow brackets a group with an OPEN and a CLOSE (`is_group`,
  `flow_visit_groups`); the painter opens a **layer** for it (`layer_try`),
  everything inside paints into the layer, and `layer_close` lays the layer
  back, filtered, at its opacity, by its blend mode.
- The layer holds the group's **exact pixels with their coverage**: the pane
  paints it over black and over white and solves for alpha (FILTER.md § 4
  A), so nothing in libos64's drawing changes.

A mask is one more step at that close, in the order CSS Masking 1 gives for
an element's effects: **filter, then clipping and masking, then opacity**.
So:

1. **libflow**: a box with a mask image that is not `none` is a group
   (`is_group`), and a stacking context. Its group's bounds are its mask
   painting area — the border box, or what `mask-clip` names — not its ink:
   a shadow outside the border box is outside the mask and shows nothing,
   so the layer is no bigger than the box.
2. **The painter**, at the group's close and before `layer_close`, builds the
   group's **coverage**, one byte a pixel of the layer: for each mask layer,
   bottom to top, it opens a *capture* layer over the mask painting area,
   paints that mask layer's image into it with the painters backgrounds use
   (`gradient`, and the face's picture lookup through a flag on the layer
   index the `backdrop` verb is handed, `YONDER_MASK_LAYER`), and takes its
   coverage back with a new verb, `layer_take(ctx, uint8_t *out, bool
   luminance)` — alpha, or luminance times alpha — which also discards the
   capture. Each layer's coverage
   is combined into the running coverage by its `mask-composite`.
3. **`layer_close` gains a coverage argument** (NULL for none), multiplied
   into the premultiplied layer after the filter and before the opacity and
   the blend.

Why capture through the verbs rather than draw the mask in the painter: a
mask layer is placed, sized, repeated, rounded and fetched exactly as a
background layer is, and the face already draws pictures (raster and SVG,
scaled and tiled) through `backdrop`. Capturing reuses all of it; drawing
in the painter would mean a second picture path.

The cost is honest and bounded: a masked group paints its subtree twice
(the layer's two backdrops), and each mask layer once into a capture layer
(itself two backdrops, of a picture or a gradient). The layers share the
paint's layer budget with filters and blends (`filter_left`); past it a
masked group is **drawn plain** and the record names it (`css-value layer:
drawn plain`), as a filtered one is, unless its mask hides it whole
(§ 10, ruling 1). A mask that is a
gradient or a picture and does not move is the same bytes every frame, and
the paint cache (YONDER_PERF.md § perf-2) can keep its coverage keyed by its
layers and its size; that is a measured follow-up, not this slice.

## 4. libgarb

- **The longhands**: `mask-image`, `mask-position` (as x and y, like
  `background-position-x/-y`), `mask-size`, `mask-repeat`, `mask-origin`,
  `mask-clip` (`no-clip` beside the boxes), `mask-composite`, `mask-mode`.
  Each is a LAYERS value read with the background grammar's own readers —
  `bg_image`, `position`, the size and repeat readers — so a conic gradient
  or a `var()` in a mask reads exactly as it does in a background.
- **The shorthand** `mask` (Masking 1 § 7.9): a background's shorthand
  without the colour, plus `mask-mode` and `mask-composite`, and the
  `<geometry-box>` pair as background's is.
- **`-webkit-` names**: every `-webkit-mask-*` is an alias of its
  longhand, as the browsers alias them, and `-webkit-mask` of the
  shorthand, read with the standard grammar — all but
  `-webkit-mask-composite`, which is `mask-composite` in the cascade, one
  property, but has words of its own: it takes the
  standard words and the legacy compositing operators' that pages write,
  each read as the standard operator it computes for one channel of
  coverage: `source-over` add, `source-in` intersect, `source-out`
  subtract, `xor` exclude, and `destination-in` intersect (keeping the
  layers below where this one is is the same product). The corpus writes
  `xor` alone in about 1,100 declarations, `source-in, xor` in about 360,
  and `source-in` in about 190 — the gradient-border pattern among them —
  against 6 for `destination-in` and 1 for `destination-out`.
  `destination-out` (the layers below cut by this one) is no standard
  operator with this layer as the source, so it and the legacy words no
  page writes (`clear`, `copy`, the atops, `destination-over`,
  `plus-lighter`) drop the declaration (§ 8). The words are read only by
  the prefixed name: `mask-composite: xor` is invalid, as the standard
  says.
- **@supports** answers what is drawn: a mask of gradients and pictures is,
  from the slice that draws it.

## 5. libflow

- `flow_style_t` gains the mask layers beside the backgrounds — the same
  list shape (`flow_backgrounds_t`) for images, sizes, positions, repeats,
  origins and clips, and lists of composites and modes —
  and `flow_mask_layer(s, i)` answers a `flow_layer_t` as
  `flow_background_layer` does, so the painter's placement code serves both.
- `is_group` adds a mask; a masked group's bounds are its mask painting
  area (§ 3.1).
- `getComputedStyle` writes the `mask-*` longhands as it writes the
  background ones.
- `mask-position` and `mask-size` animate as `background-position` and
  `-size` do (motion.c mixes the mask's lists with the background's
  code). Both are in the tick set and the in-place class beside
  `background-position` and `-size` (PILE3.md § Animations in place):
  only painting reads them, and what a mask paints is inside what its
  element already draws, so their frames move without a layout — but a
  size whose layers name a picture moves by layouts, since how big a
  copy of a picture is kept is worked out after a layout
  (PICTURE_SIZES.md § The needed size). A layout's frame of one repaints
  only the element, the in-place class's rule.

## 6. yonder

- `layer_try` opens a layer for a masked group as for a filtered one.
- The coverage is built at `group_close` (§ 3.2), the face's `backdrop`
  verb told which list a layer index names (background or mask), and
  `layer_take` and the coverage argument of `layer_close` added to the
  verbs (`paint.h`), implemented by the pane in `yonder.c` beside
  `layer_open`/`layer_close`.
- **Pictures**: a mask's `url()` joins the sheet pictures yonder fetches
  (`css_picture`), as a background's does; an SVG by `data:` URL is drawn
  as a background's SVG is (`svg_picture_pixels`).
- **A mask image not yet arrived** is transparent, so the element shows
  nothing until it comes, which is what the browsers do (§ 10, ruling 2).

## 7. Slices

- **M1, masks**: the longhands, both shorthands and the `-webkit-` aliases
  read; libflow's lists, the group, `getComputedStyle`; the painter's
  coverage, every operator and both modes — once the four operators were
  read, drawing them was four lines of arithmetic (`yonder_mask_combine`),
  and reading a value the painter then ignores would draw spinners 3, 4
  and 5 wrong rather than not at all; gradient and picture mask images,
  the icon pattern among them.
- **M2, the rest**: `-webkit-mask-composite` and its legacy keywords (§ 4;
  M1 left the name unread, so a page that wrote only `xor` drew its layers
  added); `mask-position` and `-size` animated (§ 5).
- **The coverage cache** (§ 3) became kept drawings (PILE3.md §
  Transforms, the painter: Kept drawings), a group's two sheets kept
  whole, its masks and all, which the record below asked for:
  Lamplighter's beam, a masked gradient turning, costs about 1.5 ms a
  frame at 100% zoom and 4–5 ms zoomed in on the P5 (Chris's frame meter,
  2026-11-12), growing with the pixels it covers; yonder's record splits
  a paint into its stages (gradients, layers, warps), which says how much
  of that a kept coverage would save. The beam is drawn in its own
  coordinates and turned after, so its coverage is the same bytes from
  frame to frame wherever the view does not cut it. The record of
  2026-11-13 (level 3, every lamp lit, 150%) answered first for the
  turning itself: warps 19.8 ms of a 38.3 ms paint, gradients 7.4,
  layers 6.6. So the warp went first (warp speed, PILE3.md § Transforms,
  the painter). The record after it (100% zoom: gradients 5.1, layers
  5.2, laid 3.7 of a 16.6 ms paint, warps 2.4) put most of what was left
  inside turning groups, so the cache keeps a turning box's sheets — its
  gradients and masks drawn once while only its angle changes — rather
  than a mask's coverage alone.

Each slice gets a cold read before Codex and mutants of its rules, as this
arc's slices have.

## 8. Not in M1 or M2 (DEBTS rows)

- `url(#id)`, a mask from an SVG `<mask>` element (2 hosts): an SVG-side
  feature, taken by name and drawn as no mask until then.
- `mask-border` and `-webkit-mask-box-image`.
- A mask on a plain inline box: pieces on lines, no one layer to group,
  as its opacity is not (PILE3.md § Booked). An inline-block's is drawn
  since the atom group (2026-11-12): the atom on its line is stacked
  whole, so the icon pattern laid out inline-block is drawn masked.
- The SVG geometry boxes `fill-box`, `stroke-box` and `view-box` in
  `mask-origin` and `mask-clip`, and `-webkit-mask-clip: text`: not read,
  so the declaration is dropped.
- `-webkit-mask-composite: destination-out` (1 declaration in the corpus)
  and the legacy words no page writes: not read, so the declaration is
  dropped (§ 4).
- `clip-path` (84 hosts, the same as masks): the next slice, which reuses
  this one's coverage at the layer's close with a shape for its source.

## 9. Proof

- libgarb: declaration cases for each longhand, both shorthands, the
  aliases and the legacy composite keywords.
- libflow: style dumps of the lists; `getComputedStyle` cases.
- yonder: exact pixel cases — a radial ring mask, a linear fade, an SVG icon
  by `data:` URL drawn in `currentColor`, mask size, position and repeat,
  each composite operator, luminance — and the shared budget's fallback.
- WPT: `css/css-masking` joins `tools/wpt_fetch.sh`; its `parsing` pages
  get an expectations file with what each slice makes pass marked required.
- Mutants of each rule in `tools/test_garb_mutants.py`, as slice 2's are.

## 10. Rulings (Chris, 2026-11-11)

1. **Past the layer budget, a masked group is drawn plain** (unmasked) and
   the record names it, as filtered groups are — "as long as masks aren't
   used to hide something that shouldn't be displayed". So a mask that
   hides its element WHOLE is never drawn plain: one whose every layer is a
   gradient with no stop that is not fully transparent, or is sized to
   nothing (`mask-size` 0 on an axis), draws nothing past the budget, as
   it would under it. That test needs no pixels, so it costs nothing where
   the budget is spent. A mask that hides only part of its element is drawn
   plain there, the trade the ruling makes.
2. **A mask image that has not arrived hides its element** until it comes
   (transparent coverage), as the browsers do: the icons appear as their
   pictures arrive.
