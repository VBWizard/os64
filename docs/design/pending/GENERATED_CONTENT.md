# GENERATED_CONTENT.md — `::before`, `::after` and `content` (slice 8a)

*Opus, 2026-10-31. Slice 8 of MODERN_WEB.md, the arc's last CSS piece:
generated content first, then the rest of the census's CSS. Designed and
built by Opus under the 2026-10-11 ruling (a userland-only design is its
builder's).*

## 1. Why, measured

`content` is the property yonder lacks that the most pages ask for: the
census (740 page loads from 91 sites, the P5's corpus on 2026-10-31) names
`MISSING css-property content` in **555 loads on 70 sites**, above
`transition` (554) and `filter` (473 loads, 63 sites). It is how icon fonts
draw (`.icon::before { content: "\f101"; font-family: icons }`), how
decorative shapes and clearfixes are made (`::after { content: "";
display: block }`), and how separators, required-field stars and quote
marks appear. The rest of slice 8, by the same count:

| Surface | Loads | Sites | Cut |
|---|---|---|---|
| `content` (`::before`/`::after`) | 555 | 70 | 8a |
| `filter` | 473 | 63 | 8c |
| `mix-blend-mode` | 253 | 28 | 8d |
| `backdrop-filter` | 190 | 41 | 8c, beside `filter` |
| `counter-reset` / `counter-increment` | 181 / 163 | 16 / 14 | 8b |
| `quotes` | 71 | 11 | 8b |
| `clip-path` (HTML) | 5 | 4 | booked |
| `will-change` | 0 | 0 | already read, nothing drawn of it |

## 2. What 8a does

- **`::before` and `::after` (and the legacy `:before`, `:after`)** on any
  element that has children boxes: not on replaced elements (`img`,
  `input`, `video`, `iframe`, `canvas`), not on an element whose display
  is `none`. On an element whose display is `contents` the pseudo-element
  boxes are made where its children are, as CSS Display § 2.5 says.
- **`content`**: `normal` and `none` (no box: a pseudo-element without
  content does not exist), and a list of `<string>`, `attr(<name>)`,
  `open-quote`, `close-quote`, `no-open-quote` and `no-close-quote`,
  concatenated. `attr()` reads the originating element's attribute (the
  empty string when absent). The quote words take the `<q>` element's
  marks and nesting level, which yonder already draws, until 8b brings the
  `quotes` property. An item 8a does not draw (`counter()`, `counters()`,
  `url()`, an image function) is read, kept, and draws nothing, so the
  pseudo-element still exists and keeps its box (a clearfix or a coloured
  square needs no text); `/ <alt text>` is read and not used.
- **Any display and position the pseudo-element's style asks**: inline by
  default, and `block`, `inline-block`, `flex`, `grid`, `absolute`,
  `fixed`, floats, all through the same box generation an element child
  takes. That is the point of § 4.
- **Not in 8a** (booked, each a DEBTS row): counters and `quotes` (8b),
  images in `content` (`url()`), `::marker` and `::first-line`/
  `::first-letter` styling, and `getComputedStyle(el, '::before')`, which
  answers the empty declaration it answers today.

## 3. The cascade (libgarb)

Today `add_style_rule` files a selector only when its pseudo-element is
none (`cascade.c`), so a `::before` rule is parsed and never reached. 8a:

- **A rule's selectors for `::before` and `::after` are filed in tables of
  their own**, one per pseudo-element, keyed by the same id/class/type
  buckets as the element tables. A selector matches its ORIGINATING
  element (select.h: "its pseudo-element aside"), so the lookup is the
  element's own, in the pseudo-element's tables.
- **`garb_style_for_pseudo(cascade, element, pseudo)`** answers the
  pseudo-element's winners exactly as `garb_style_for` answers an
  element's: author origin, one per longhand, CSS-wide keywords as
  `GARB_V_WIDE`. Inline `style` never applies (it is the element's), and
  neither do SVG presentation attributes.
- **Computed eagerly, as the element's are, and only when a pseudo rule
  matched**: a slot keyed by (element, pseudo-element), so an element no
  `::before` rule names costs a failed lookup and nothing else. Custom
  properties inherit from the originating element.
- **`content`** is a new longhand, not inherited (`GARB_CONTENT`), its
  value a list of items (`G_CONTENT`).

## 4. The box (libflow)

**A pseudo-element is a synthetic element node owned by libflow**: an
`os64_html_node_t` made in the style pass's arena, its `parent` the
originating element, holding one text child with the resolved content.
It is inserted, in box generation only, as the originating element's
first child (`::before`) or last child (`::after`), and goes through
`flow_range` like any child: an inline pseudo-element becomes inline
items, a block one a block, an absolute one an absolute box. That is why
§ 2's displays come with no code of their own.

- **Its style is pass 1's**: `inherit()` from the originating element's
  `FStyled`, then its author winners (`garb_style_for_pseudo`) through the
  same `lay_sets`, then `finish()`. The element's `FStyled` keeps a
  pointer to each of its pseudo-elements' (node and style), NULL for none.
- **Its content is resolved once, in pass 1**: the items concatenated to
  one string (`attr()` read from the originating element), which becomes
  the synthetic node's text child. Text from content is `generated`, as
  `<q>`'s marks are, so `text-transform` and selection treat it as such.

### The boundary rule

**A synthetic node never leaves libflow.** A public `flow_box_t` made for
a pseudo-element carries `node` = its ORIGINATING element and
`pseudo` = which one (a new field, `GARB_PSEUDO_NONE` for every other
box). So a click on a link's `::before` lands on the link, as in every
browser, and nothing outside libflow ever holds a node no document owns.
What that asks of the readers of `box->node`:

| Reader | What it must do |
|---|---|
| `flow_box_for(tree, element)` and every walk that finds an element's box by its node | skip boxes whose `pseudo` is set: the element's box is its own (`getBoundingClientRect`, `offsetWidth`, scrollIntoView, a focused control) |
| hit testing (the topmost box under a point) | nothing: the pseudo box answers its originating element, which is the target |
| text extraction and selection | skip generated text, as `<q>`'s marks are skipped |
| paint | nothing: a pseudo box is painted as any box of its kind |

Each site is named in § 7 as built, by audit of the readers of `->node`
outside libflow (about fifty-six today).

## 5. Lifecycle

| Event | Pseudo-element style | Synthetic node | Box |
|---|---|---|---|
| a style pass (`f_style_build`) | computed for each element a pseudo rule matched, `content` resolved | made in the pass's arena | — |
| a layout (`f_boxes_build`) | read | inserted first / last among the children | made by `flow_range` |
| the element's attributes change (`attr()`), or its class | recomputed by the next style pass | remade | remade |
| `content: none`/`normal`, or `display: none` on the pseudo-element | none: the pseudo-element does not exist | none | none |
| the originating element is `display: none` | not computed (its subtree has no style) | none | none |
| the pass's arena freed | gone | gone | gone with the tree |

Slice 5's incremental relayout (RELAYOUT.md, Quinn's 5b/5c) reuses styles
and boxes between layouts: a pseudo-element's style and node belong to the
originating element's style record, so whatever keeps or recomputes that
record keeps or recomputes them with it. That is the coordination point,
and it is said here so 5b and 8a meet on it rather than discover it.

## 6. Proof

- **libgarb**: the cascade dump shows a pseudo-element's winners under its
  originating element (`tools/garb_corpus/cascade.txt`); `content`'s
  reader accepts and refuses by its grammar.
- **libflow**: box dumps for an inline, a block, an absolute and a floated
  pseudo-element; `attr()`; quotes by level; `content: none`; a replaced
  element and `display: none` generating nothing; `display: contents`
  generating where its children are; layout dumps for a clearfix and an
  icon-font glyph.
- **yonder**: a page whose link's `::before` is clicked navigates; an
  element's `getBoundingClientRect` is its own box's; the record stops
  naming `content`.
- **WPT**: `css/css-content/` and `css/css-pseudo/` testharness pages
  where the runner can judge them (most of both are reftests, which it
  cannot).
- **The P5**: the census's top `content` sites before and after.

## 7. As built

*(Filled in as 8a is built.)*
