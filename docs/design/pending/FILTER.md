# FILTER.md — `filter` and `backdrop-filter` (slice 8c)

*Opus, 2026-10-31. Slice 8c of MODERN_WEB.md, after 8a/8b
(GENERATED_CONTENT.md). A userland design, its builder's (the 2026-10-11
ruling); written before code because it changes how yonder paints a
group.*

## 1. Why, measured

The census (740 page loads, 91 sites) names `filter` in **473 loads on 63
sites**, the most-asked CSS property yonder lacks now that `content` is
drawn, and `backdrop-filter` in **190 loads on 41 sites**. In the page copies
read so far, `blur()` is most of the functions written (24 of 39), then
`drop-shadow()`, `grayscale()` and `url()` references. So blur is in the first
cut, not a later one.

What pages do with them: blurred hero images and placeholders, frosted
panels (`backdrop-filter: blur()` behind a translucent header), icons
recoloured for dark mode (`invert()`, `brightness(0)`), greyed-out logos
(`grayscale()`), and soft shadows under shaped images (`drop-shadow()`).

## 2. What 8c draws

- **`filter`**: `none`, or a list of the Filter Effects 1 functions:
  `blur(<length>)`, `brightness()`, `contrast()`, `grayscale()`,
  `hue-rotate(<angle>)`, `invert()`, `opacity()`, `saturate()`, `sepia()`
  (each a `<number>` or `<percentage>`), and
  `drop-shadow(<color>? <length>{2,3})`, applied in order. A `url()`
  reference (an SVG filter) is read and named as approximated, and the list
  holding it draws unfiltered: Filter Effects 1 § 5.1 says a list with a
  reference to nothing usable is ignored whole, and yonder has no SVG filter
  primitives.
- **`backdrop-filter`**: the same list, applied to what is painted BEHIND the
  element, inside its border box (rounded by `border-radius`), before the
  element paints.
- **Layout effects of a filter other than `none`** (Filter Effects 1 § 2):
  the element makes a stacking context and is the containing block of its
  absolute and fixed descendants, as a transform is (`f_contains_absolute`,
  `f_contains_fixed`). A `backdrop-filter` does both too (Filter Effects 2
  § 3). A fixed box a filter contains scrolls with it. **The root is the
  exception** (both specs: "unless the element it applies to is document
  root element", `f_anchors`): `html { filter: grayscale(1) }` leaves a
  fixed header fixed to the viewport.
- **A plain inline's filter is not drawn** and makes nothing
  (`f_filtered`), and **an inline-block's is not drawn** though it is a
  containing block, as their opacity is painted opaque (§ 10).

## 3. The problem: a group has no coverage

yonder paints a group (opacity below 1, a transform drawn through its
matrix) **in place** (yonder.c's `glass_group_open`/`_close`): at open it
copies what is under the group's bounds, the group paints straight onto the
surface, and at close the copy is laid back at 255 − alpha. That is exact for
opacity and needs no layer.

A filter cannot work that way. `grayscale()` must change the group's pixels
and not the page behind them, and `blur()` must spread the group's edges
over the page. Both need the group's own pixels **with their coverage**
(alpha), apart from the backdrop. yonder's drawing primitives cannot give
that: `os64_draw_blend`, the fills, text and pictures all treat the
destination as opaque and write alpha 255.

## 4. Options for the layer

**A. Two backdrops (my lean).** Paint the filtered group's subtree twice
into two scratch layers the size of its bounds: once over black, once over
white. Per pixel, with straight compositing, black gives `B = c·a` and white
gives `W = c·a + 255·(1 − a)`, so `a = 1 − (W − B)/255` and the
premultiplied colour is `B` itself. That recovers the group's exact pixels
and coverage with **no change to any primitive**: every verb paints as it
does today, onto an opaque surface. Then the filter runs on the
premultiplied layer, and the layer is composited over the real surface.
- Cost: every verb inside a filtered group drawn twice, once on each
  backdrop, and only for filtered groups.
- Exact up to rounding: `W − B` is a channel difference of two 8-bit
  paints, so `a` is exact to 1/255. It is taken from the green channel, the
  one with the most precision in yonder's ARGB.
- No second walk: yonder's pane already draws every verb into each target
  it holds, and a group laid through its matrix already paints onto two
  SHEETS, black and white (warp.h, PILE3.md § Transforms). A filtered group
  opens the same pair of sheets, so the one walk paints both backdrops.
  (Drafted as a nested walk over the group's layers, `flow_visit_range`;
  the pane made it unnecessary.)

**B. Alpha-aware primitives.** Teach every primitive Porter-Duff "over"
onto a destination with alpha: `os64_draw_blend`, the fills, text drawing,
picture blits, gradients, borders. That is the general fix, and it touches
libos64's drawing everywhere and every caller of it. Rejected for 8c: too
wide for what it buys now. It stays the shape for a later compositor.

**C. In place, guessing coverage.** Filter the surface where it changed from
the copied backdrop. Rejected: a group that paints the backdrop's own colour
counts as uncovered, and an antialiased edge cannot be told from a
half-covered pixel.

## 5. The filters, as computed (on the premultiplied layer)

- **Colour matrices** (`grayscale`, `sepia`, `saturate`, `hue-rotate`) and
  the component transfers (`brightness`, `contrast`, `invert`, `opacity`):
  Filter Effects 1 § 13's matrices and functions, on unpremultiplied colour,
  per pixel. Several in a row are one composed matrix where they compose.
- **`blur(r)`**: a Gaussian of standard deviation `r`, approximated by three
  box blurs (the standard approximation, error under 3%), separable, on the
  premultiplied layer, so edges fade into transparency rather than into
  black. The layer is grown by `3r` on every side, because a blur spreads
  ink that far.
- **`drop-shadow(c x y r)`**: the layer's alpha, blurred, offset by
  `(x, y)`, tinted `c`, under the layer. `r` is a blur RADIUS read as a box
  shadow's (Filter Effects 1 § 13: a deviation of `r/2`), so it is blurred
  as yonder blurs a box shadow, three box blurs of radius `r/2`. Each side
  grows by that blur's reach less the offset toward the other side, never
  below 0, and functions after the first spread what the ones before made
  (`flow_filter_reach`).

## 6. Bounds and budget

- **Bounds, not ink**: a filtered group's bounds are grown by what its
  filter spreads (`filter_grown`), once, after its members are gathered and
  before its matrix; the box's own ink is left as it is, or its own layer
  would be counted twice. The view a filtered group's members are culled
  against is grown the same way, so a box the blur reaches in from outside
  the view is painted. A change to any box inside a filtered group marks
  the group's bounds (`mark_filtered`), since the filter spreads it.
- **A budget per paint**: filtered area (each layer charged the pixels it
  shows in the view, not its margin, so a small repaint beside a big blur
  is not refused what the rest of the page was drawn with) up to
  `YONDER_FILTER_AREA_VIEWS` (2) times the view's pixels, and a blur's
  standard deviation up to `FLOW_FILTER_BLUR_MAX` (100 px, libflow's, so
  its bounds and the painter agree; a larger one is drawn at it). Past the
  area budget, or a layer the face cannot make or filter, a group paints unfiltered
  and the record names it (`css-value filter: drawn unfiltered`), so a page
  of a hundred blurred cards degrades, never stalls the window.

## 7. `backdrop-filter` (8c's second part)

It needs no coverage. Before the element paints, what is already on the
surface inside its border box is copied, filtered (blur reading a margin of
3r beyond the box and clamping at the surface's edge), clipped to the
rounded box, and written back. Then the element paints over it as usual.
The copy is the same scratch as a group's. It comes after `filter`'s part,
since it shares the filters and the budget.

## 8. Lifecycle

| Event | Style | Layer | Ink |
|---|---|---|---|
| a layout | `filter`/`backdrop-filter` computed (`flow_style_t`) | — | grown by the spread |
| a paint reaching a filtered group's `PART_OPEN` | read | two sheets made (black, white), the targets until the close | — |
| its `PART_CLOSE` | — | coverage recovered, filtered, composited, scratch returned | — |
| past the budget | — | none: painted unfiltered, recorded | as grown |
| an animation of `filter` | discrete: the first value until halfway (§ 10) | as at a paint | as the value drawn |

## 9. Proof

- **libgarb**: `filter` and `backdrop-filter` read to their grammar,
  `url()` named approximated (`declarations.txt`, `skips.txt`).
- **libflow**: a filtered box makes a stacking context, contains its
  absolute and fixed descendants, opens a group, and grows its ink by its
  spread (box and layer dumps).
- **yonder paint (host)**: pixel cases through the real painter.
  - Coverage recovery: a half-transparent and an antialiased group recovered
    exactly (to 1/255).
  - Each colour filter against Chrome's numbers for fixed inputs, from
    captures, not memory.
  - Blur of a square: symmetric, energy kept, fading to transparent.
  - `drop-shadow` offset and tint.
  - The budget: past it, unfiltered and recorded.
- **Mutants** per rule; **WPT**: `css/filter-effects` where the runner
  can judge it (most are reftests), and no loss elsewhere.
- **The P5**: blurred placeholders, dark-mode icons and frosted headers on
  the census's top `filter` sites.

## 10. Not in 8c (DEBTS rows)

- SVG filter references (`url(#f)`) and the SVG filter primitives.
- `filter` on an inline box or an inline-block: not drawn, as their
  `opacity` is not (PILE3.md § Booked: neither is one layer to group).
- `filter` and `backdrop-filter` on a table's rows, row groups and columns:
  no containing blocks, as a transform on them is not (PILE3.md § Booked).
- Interpolating `filter` lists (Filter Effects 1): an animated filter
  is discrete, its first value until halfway and its second from there.
- `mix-blend-mode` (8d) and `clip-path` on HTML.

## 11. As built (part 1, `filter`)

- **libgarb** reads both properties to § 2's grammar (`filter_list`),
  names a list holding a `url()` approximated, and a `backdrop-filter` by
  its first function while it is not drawn.
- **libflow** computes them into `flow_filter_t` lists (`author_filter`).
  `f_filtered`, `f_contains_fixed` and `f_group_filter` give § 2's layout
  effects and the group; `flow_filter_reach` is § 6's spread, shared by
  the group's bounds (`filter_grown`), the view its members are culled
  against, `box_seen`, and the painter. A change inside a filtered group
  marks the group's bounds (`mark_filtered`). `getComputedStyle` writes
  the lists back (`filters`, computed.c).
- **What a layer holds**: the group's bounds (grown by the OUTPUT reach)
  cut to the view grown by the INPUT reach — `flow_filter_reach` mirrored,
  top for bottom, since a shadow cast down shows below a source above the
  view (the cold read's first finding) — and cut to what clips the group
  from outside, which the spread may not pass. libflow culls a filtered
  group's members against the view grown the same mirrored way
  (`filter_sources`).
- **yonder**: `filter.c` is pure, host-tested pixel by pixel — the
  recovery, each function (the colour ones held to WPT's reftest
  expectations), the blur, the drop shadow, the lay. The painter opens a
  layer per filtered group (`filter_try`), inside any matrix, at most
  `YONDER_FILTER_AREA_VIEWS` times the view's pixels a paint; the pane makes
  it two sheets (`pane_sheets_open`, shared with a laid group, to their own
  `FILTER_SHEET_*` limits, which hold a whole 4K window and its spread) and at the
  close recovers, filters and lays them (`pane_filter_close`). A refused
  layer paints the group unfiltered, and the record says
  `css-value filter: drawn unfiltered`. The paint's cost line has a
  `filters` stage.
- **Measured** in QEMU: a page of every function, a blur, drop shadows on a
  rounded box and on text, a filter under opacity and one under a
  rotation, drawn as § 5 says, the `url()` case unfiltered and named.
  WPT `css/filter-effects/parsing`: 146 passes, nothing lost in
  `css/cssom` or `css/cssom-view`.
