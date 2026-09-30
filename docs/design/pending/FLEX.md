# FLEX.md — flexible box layout: rows and columns that share their room

*Written 2026-09-30 by Opus, the night POSITION.md's last slice (sticky,
#178) was merged and Chris set danlegt.com beside Chrome: "the 6 buttons
should be in a row, not stacked on each other". This is the second part
of GARB.md's pile 2; grid is the third. The library is libflow and its
rules are [LAYOUT.md](../completed/LAYOUT.md)'s; the properties come from
libgarb ([GARB.md](../completed/GARB.md)); positioned boxes inside a flex
container follow [POSITION.md](POSITION.md). Nothing here is waiting on a
ruling: the questions are web semantics, decided below with Chrome as the
yardstick (§ Decisions), and the one question about what a person sees —
the slice order — is put in § Rulings.*

## Why this, and why now

libgarb reads `display: flex` and libflow lays the box out as the block it
is on the outside (`inline-flex` as an inline-block), each child a block
in document order, full width, stacked. The web of 2026 lays out almost
every row with flexbox:

- a nav bar — danlegt.com's six buttons (`nav { display: flex;
  justify-content: space-around }`, each link `flex: 1`) — is a column of
  full-width bars;
- a toolbar, a header with a logo on the left and links on the right
  (`margin-left: auto` on the last item), a row of icon and label inside
  one button (danlegt's `nav a { display: flex; align-items: center }`):
  each stacks;
- zombo.com's "Loading… 0%" line (`display: flex; justify-content:
  center; gap: 14px`) is three lines;
- a row of cards wraps onto as many lines as it needs (`flex-wrap:
  wrap`): it becomes one card per line;
- a footer whose columns share the width equally (`flex: 1`) is a stack.

A stack still reads, which is why positioning came first. But it is the
single largest difference left between yonder and Chrome on a modern
page, and every one of those pages is written as if it could not happen.

## What CSS asks (CSS Flexible Box Layout 1)

A FLEX CONTAINER (`display: flex`, or `inline-flex` on the outside of a
line) lays its in-flow children out as FLEX ITEMS along a MAIN AXIS —
across (`flex-direction: row`, the initial value) or down (`column`), each
reversible — and sizes and aligns them on the CROSS AXIS, the other one.

| Property | On | Values | What it does |
|---|---|---|---|
| `flex-direction` | container | `row`, `row-reverse`, `column`, `column-reverse` | the main axis and which end the items start from |
| `flex-wrap` | container | `nowrap`, `wrap`, `wrap-reverse` | one line, or as many as the items need |
| `flex-flow` | container | shorthand: direction, wrap | |
| `justify-content` | container | `flex-start`, `flex-end`, `center`, `space-between`, `space-around`, `space-evenly` (and `start`, `end`, `left`, `right`, `normal`) | where the main axis's free space goes |
| `align-items` | container | `stretch` (initial, as `normal`), `flex-start`, `flex-end`, `center`, `baseline` (and `start`, `end`, `self-start`, `self-end`) | each item's place on the cross axis |
| `align-self` | item | `auto` or any `align-items` value | one item's, over its container's |
| `align-content` | container | `stretch`, `flex-start`, `flex-end`, `center`, `space-between`, `space-around`, `space-evenly` | where the lines go, when there are several |
| `gap`, `row-gap`, `column-gap` | container | a length or percentage | space between items and between lines (grid's too) |
| `flex-grow` | item | a number, initial 0 | its share of free space |
| `flex-shrink` | item | a number, initial 1 | its share of an overflow, weighted by its basis |
| `flex-basis` | item | `auto`, `content`, a length or percentage | its size before growing or shrinking |
| `flex` | item | `none`, `auto`, `<grow> <shrink>? <basis>?` | shorthand; `flex: 1` is `1 1 0%` |
| `order` | item | an integer, initial 0 | where it goes among its siblings |

The algorithm is § 9's: the items' HYPOTHETICAL main sizes (their basis
held to their limits), lines collected from them, the free space on each
line shared out by `flex-grow` or taken back by `flex-shrink` — items that
hit a limit frozen and the rest shared again (§ 9.7) — then each line's
cross size from its items, each item placed on the cross axis, and the
main axis's leftover given to auto margins, else to `justify-content`.
Two rules matter more than they look:

- **An item's automatic minimum** (§ 4.5): `min-width: auto` on a flex
  item is not zero but its CONTENT'S min-content width (its widest word),
  capped by a width it was given — so a row of items shrinks until a word
  would break and no further, and overflows instead.
- **Items are BLOCKIFIED** (§ 4): an inline child is laid out as a block,
  an inline-block as a block, an inline table as a table, and each run of
  text among them becomes an anonymous item of its own (a run that is only
  white space makes none, preserved or not).

## How it enters libflow

The rule that shapes it: **a flex item is laid out once, like every other
box.** Everything the algorithm needs to know BEFORE an item is laid out
is a width, and libflow already knows every box's two content widths
without laying it out (`intrinsic`, measured once and kept — the table
layout's discipline, LAYOUT.md § Bounds). So a ROW is resolved first and
laid out after: each item is handed the width the algorithm gave it, as
an absolute box is handed the width its equations gave it (`abs_sized`),
and what the cross axis decides afterwards — a stretched height, a line's
alignment — sets a height or MOVES the laid-out box (`translate`), which
does not change what is inside it — except in an item that is itself a
flex container, whose items are PLACED again against the height it was
given (`refit`: moved and sized, not laid out again). A COLUMN is laid
out first and resolved after: its items take their widths from the container, as blocks do, and
growing or shrinking changes a height, never a width, so nothing reflows.
No item is laid out twice, and a page of nested flex containers costs what
the same page of blocks costs.

### libgarb

Every property in the table is read into the property table, with the
`flex`, `flex-flow` and `gap` shorthands (and `grid-gap`, the old spelling
of `gap` every flex page still writes). `display: flex` and `inline-flex`
leave `kApproximated`, so `@supports (display: flex)` says yes from F1 —
and the page's `inline-block` fallback, written for browsers without
flexbox, is no longer taken. F1 laid a wrapping container out on one line
and kept `flex-wrap: wrap` among the approximations; F2 wraps it, and
`@supports (flex-wrap: wrap)` says yes.

### Pass 1 — style

`flow_display_t` gains `FLEX` and `INLINE_FLEX` (grid will add its own),
and `flow_style_t` gains the properties: direction, wrap, the three
alignments and `align_self`, `flex_grow` and `flex_shrink` in thousandths
(as `opacity` is), `flex_basis` (a `flow_length_t` with a CONTENT kind),
`order`, and `row_gap` and `column_gap`. The children of a flex container
are BLOCKIFIED here — the same step, and the same `specified_inline` bit,
as an absolute box's (POSITION.md § Pass 1), so a static position keeps
working — unless they leave the flow. `float` on an item is ignored, as
it is everywhere in libflow.

### Pass 2 — boxes

- **A flex container is never an inline formatting context.** Its
  element children are block-level after pass 1, so `build_container`
  already makes them block boxes, and a run of text among them already
  goes into an anonymous block (`target`) made only on its first
  significant character. Two changes: a container holding nothing but
  text is not an IFC either — its text is one anonymous item; and a run
  of text between items that is all white space makes no item even when
  `white-space` preserves it (`blank_run`), where a block would make a
  line of it.
- `FBox` gains `flex`: this box lays its children out as flex items. An
  `inline-flex` element is an atom whose content box is the container,
  as an `inline-block`'s is its block.
- **`order`**: the items are left in document order in the box tree —
  the tree's pre-order is the document's, which the partial-tree
  relations and the dumps rely on (LAYOUT.md § Proof) — and pass 3 sorts
  its own list of them, stably, by `order`.
- An absolute or fixed child is not an item; it hangs under the container
  as it would under any block (POSITION.md § Pass 2).

### Pass 3 — layout

`block_at` hands a flex container's content to `flex()` where it would
have called `children()` or `lines()`. A flex container starts a block
formatting context of its own (`bfc_root`): nothing collapses through it,
and its items' margins never collapse with each other or with it.

1. **The items**, in `order` order, and each one's **outer
   hypothetical main size**. Its FLEX BASE SIZE is `flex-basis` when it
   is a length, or a percentage of a main size that is known; else, for
   `auto`, its `width` (row) or `height` (column) when that is given;
   else — and for `content`, and a percentage of a size not known — its
   content: in a row its max-content width (`content_max`); in a column the
   height its content took laid out at its cross size (`FBox.content_h`,
   whatever `height` the page gave — for an item that is a flex container,
   the height its own items ask; for a table, its rows'; this is where a
   column lays each item out, once). The base is NOT held to the item's
   limits: they weigh nothing in shrinking. The hypothetical size is that,
   held to its limits, with `min-width: auto` (row) or `min-height: auto` (column)
   read as the automatic minimum (§ 4.5): in a row its CONTENT'S
   min-content width — `intrinsic` keeps the content's two widths beside
   the ones a set width overrides (`content_min`, `content_max`), and the
   automatic minimum and `flex-basis: content` read those — in a column
   its content height (`content_h`) — capped in either by a size the page
   gave it — and zero for an item that is a scroll container (overflow
   other than `visible` or `clip`). A column item is laid out at its
   cross size: the container's inner width less its margins when it
   stretches, else fit-content (`fit_content`, as a shrink-to-fit box).
2. **Lines** (F2). Items are taken in `order` order until the next one's
   outer hypothetical size would pass the container's inner main size
   (§ 9.3), each line at least one item; a container that does not wrap
   has one. A column breaks into lines only where its height is given or
   limited by `max-height`: with neither, it is as tall as all its items,
   on one line. Steps 4 and 8 are each line's own.
3. **The container's main size**: a row's is its width, which block
   layout already knows (`widths`) — or, as an atom or a shrink-to-fit box,
   what `intrinsic` answers for a flex container (below); a column's is
   its `height` when given, else the sum of its items' hypothetical sizes
   and gaps, held to `min-height`/`max-height`.
4. **Resolving the flexible lengths** (§ 9.7), per line: free space is the
   inner main size less the items' outer hypothetical sizes and the gaps;
   positive, the growing items share it by `flex-grow`; negative, the
   shrinking ones give it back by `flex-shrink` × base size, weighed as
   real numbers so that a small factor on a base under a pixel is not
   rounded to nothing; an item a
   share would carry past a limit is FROZEN at the limit and the rest
   share again, until none is. A sum of `flex-grow` below 1 shares only
   that fraction. 26.6 all the way, the remainder of a share to the last
   unfrozen item, so the sizes add up exactly and a line of `flex: 1`
   items meets its end.
5. **Laying the row's items out**: each at its main size (the forced
   width), `block_at` as any block is, at its line's cross start. Its
   height is then its HYPOTHETICAL CROSS SIZE; a picture whose height is
   `auto` takes it from that width by its own ratio.
6. **Cross sizes**: a single-line container's line is as tall as its
   `height` if given, else its tallest item (outer), held to its limits; a
   multi-line container's lines are each their tallest item — the
   baseline-aligned ones by what they reach above and below their shared
   baseline — and `align-content` places them in the container's height:
   `normal`, which is `stretch`, shares what is left among them, the rest
   place them as `justify-content` places items. `wrap-reverse` stacks
   them from the far side and swaps `flex-start` and `flex-end` for the
   items in them. A wrapping COLUMN's lines are as wide as their widest
   item, which is not known before its items are laid out, so there each
   item is laid out at its own width (fit-content) and a stretched one's
   box is widened to its line without laying its content out again
   (booked).
7. **Cross alignment**, per item: `stretch` — the initial value — sets the
   item's height (row) or width (column) to the line's less its margins,
   when that dimension is `auto` and neither cross margin is `auto`, held
   to its limits. A row item's height is only its box, so nothing inside
   moves — unless the item is a flex container itself: then its items are
   placed again against the height it now has (`refit`): everything
   after laying its items out, over the same laid-out boxes, from what
   laying them out left — their natural sizes (`natural_h`, `natural_w`),
   never an earlier placement's, and a row's lines as laying out formed
   them (`FBox.flex_line`), while a column forms its lines again against
   the height, which is now known. A column item grown or shrunk is refit
   the same way. The absolute boxes an item contains
   are laid out after its OUTERMOST flex container has finished with it
   (`sized_later`, `items_done` walking down through nested containers),
   against the size it ends with. `flex-start`, `flex-end` and `center`
   move the item. `baseline`
   lines the items' first baselines up (`first_baseline_in`), the item
   with the most above its baseline at the line's start; an item with no
   line of text has one made from its border box's bottom edge. Auto
   margins on the
   cross axis take the space first: `margin: auto 0` centres.
8. **Main alignment**: auto margins on the main axis take the free space,
   equally; if there are none, `justify-content` places the items — and
   the reverse directions and `flex-end` mirror what `flex-start` does.
   `space-between` with one item is `flex-start`, `space-around` and
   `space-evenly` with one item are `center`. Negative free space is never
   spread: all three `space-*` values fall back to `flex-start` — what
   Chrome 154 does (`tools/flex_probe/flex-probe.html`; Flexbox 1's older
   text sent `space-around` to `center`) — and `center` overflows both
   ways. `left` and `right` are physical: in a row they are the page's
   sides, and on a column (not the inline axis) both are `start` (Box
   Alignment 3 § 5.1).
9. **Column items** are placed by moving them: they were laid out in step
   1, and steps 4–8 decide where and how tall. A relative item keeps its
   offset on top of the place it is moved to (`laid_offset`).

**A flex container's intrinsic widths** (`intrinsic`, for an
`inline-flex` atom, a flex container in a table cell or an absolute box):
a row's max-content is the sum of its items' max-content outer widths and
gaps, its min-content the sum of their min-contents when it does not wrap
and the largest when it does; a column's are its items' largest. An item
that cannot shrink asks at least its `flex-basis` when that is a length.
(CSS Flexbox § 9.9.1's full answer weighs items by their flex factors;
Chrome 154 answers the plain sum — the probe's inline-flex of items
growing 1 and 3 is exactly their two max-content widths wide — with that
one floor: two empty `flex: 0 0 100px` items are 200 wide, and a
`flex: 0 1 100px` item holding 200px of content asks 200.)

**A flex container's baseline**, for `inline-flex` on a line and
`align-items: baseline` on its own container: the first baseline among its
items in document order (`first_baseline_in`, which an item `order` moves
does not change; booked with paint order).

**Absolute children** (POSITION.md): a flex container is their containing
block when it is positioned, as any block is. The static position of an
absolute box whose parent is a flex container is the container's content
box's start corner — Flexbox § 4.1 places it as if it were the only item,
with `justify-content` and `align-items`, and that is booked.

**Percentages**: a percentage `flex-basis`, width or margin of an item is
of the container's inner size on that axis; a percentage height inside a
stretched item is booked with GARB.md's percentage-height row.

**All or nothing.** Items are laid out in TREE order, whatever `order`
says, and placed where they belong only once every item is laid out; a
layout that stops in between WITHDRAWS the container's items
(`items_done`), because each has only its provisional place, and the
container is left unfinished. A box tree the boxes pass cut short is laid
out with the items it has, which may place them differently from the
whole page, as a table's columns come from the cells it has: the relation
sweep compares neither under an unfinished flex container
(`sized_by_its_children`).

### The door

Nothing new. A flex item is an ordinary box in the tree, painted and hit
in the tree's order: an item `order` moves still paints where the
document put it among its siblings, which only differs where items
overlap (booked below, with `z-index` on a static item, which also makes
a stacking context in CSS). An `inline-flex` atom is an atom. The box
dump (pass 2's) gains ` flex` on a container's line.

### The face

Nothing. yonder draws what the tree says.

## Decisions (web semantics: Chrome is the yardstick)

1. **An item is laid out once** (above): the design rather than an
   optimisation, and the reason F1 can be judged by LAYOUT.md's bounds
   unchanged. A nested flex container given a new height places its items
   again (`refit`), which moves and sizes boxes and lays none out. It
   costs nothing CSS asks for except what is booked: a percentage height
   inside a stretched item, which needs the stretched size first.
2. **`order` does not reorder the tree.** Paint and hit order stay the
   document's; placement follows `order`. Chrome paints in `order` order,
   which differs only for overlapping items.
3. **The intrinsic width of a flex container is the plain sum**, not §
   9.9.1's flex-weighted one: Chrome 154 answers the sum (the probe's case
   2, items growing 1 and 3), with an item that cannot shrink asking at
   least its length basis, and a page's inline-flex is as wide there as
   here.
4. **Negative free space is never spread by `justify-content`**: the
   `space-*` values start at the start edge and `center` overflows both
   ways — what Chrome 154 does (the probe's case 1), and what `safe` and
   `unsafe` (booked) would let a page choose.
5. **F1 laid a wrapping container out on one line**, its items squeezed
   rather than stacked, as the nearer of the two to the designed page,
   until F2 wrapped it.

## Slices

| Slice | What | Proof |
|---|---|---|
| F1 | libgarb reads every property in the table and the three shorthands; `display: flex`/`inline-flex` laid out and supported; pass 1 blockifies items; pass 2's text-only container; single-line flex in all four directions: bases, the automatic minimum, § 9.7's freezing loop, gaps, `order`, auto margins, `justify-content`, `align-items`/`align-self` with stretch and baseline; the container's intrinsic widths and baseline; absolute children at the content box's corner | hand-laid pages worked from § 9 for each step, with the arithmetic in the test (a row of `flex: 1` at every width; grow with a max-width freezing; shrink with a min-content floor; each `justify-content` with 1, 2 and 3 items; stretch, center, baseline; margin-left auto; column with a given height and without; each reverse; `order`); the corpus dumps unchanged where no sheet says flex; the flex fuzz; the partial-tree relations; a guest page: danlegt's nav row and zombo's loading line against Chrome |
| F2 | `flex-wrap: wrap` and `wrap-reverse`: lines, `align-content`, a multi-line container's cross sizes and baselines, `@supports (flex-wrap: wrap)` | hand-laid wrapped rows (cards at three widths), each `align-content` with a given height; the fuzz; a guest page of cards |

Chris had all three built stacked on 2026-09-30 — F1, F2 on it, then
grid — each reviewed in turn.

## Proof, and the invariants that hold

- **Every box laid out once**: the fuzz already asserted every box's
  widths are measured at most once (`intrinsic_once`); F1 counts how often
  each box is laid out (`FBox.laid`) and asserts at most once for every
  box, flex item or not.
- **The sizes add up**: shares are given out by cumulative rounding, so a
  line whose items all flex meets its end to the 26.6 unit — the fixtures'
  line of three `flex: 1` items in 400 is 133, 134 and 133 once rounded.
- **Containment and the relations** are LAYOUT.md's and POSITION.md's,
  unchanged: an item is an in-flow child of its container.
- **The flex fuzz**: the positioned soup (POSITION.md § Proof) gains
  `display: flex` and `inline-flex`, every property in the table with its
  edge values (`flex: 0 0 0`, `flex: 1e9`, a negative `order`, `gap:
  -5px` which is invalid, a percentage basis in an auto-width atom), and
  nesting, laid out through the cascade.
- **Fixtures** are computed by hand from § 9 (LAYOUT.md § Proof's rule:
  never captured from the engine), and the probe page
  (`tools/flex_probe/flex-probe.html`, which prints what it measures) is
  where any case the spec leaves to the reader is put to Chrome — headless
  from this machine, no one's desk needed.

## Rulings

1. **The slice order: F1 (everything on one line), then F2 (wrapping),
   then grid.** Recommended: F1 alone turns danlegt's nav and zombo's
   loading line into rows, and every toolbar and header on the web with
   them; wrapped card rows are next most common, and squeezed onto one
   line until F2 rather than stacked. Grid after, since its sizing reuses
   what F1 builds (content sizes, the freeze-and-share loop).

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| Paint and hit in `order` order, and `z-index` on a static flex item | the tree's order is the document's; the two differ only where items overlap | a page whose overlapping items draw in the wrong order |
| A percentage height inside a stretched item | the stretched size is decided after the item is laid out, and nothing is laid out twice | GARB.md's percentage-height row |
| The static position of an absolute child as the only item (§ 4.1) | the content box's corner is where it usually is already | a page whose absolute icon sits at the wrong end of its flex row |
| `safe` and `unsafe` alignment, `last baseline`, `first baseline` spelled out | rarely written | a page that writes them |
| `visibility: collapse` on flex items (§ 4.4, struts) | rare | a page that collapses an item |
| `flex-basis: min-content` and `fit-content` | read as `content` (max-content), the nearest this reads | a page whose item they would size differently |
| A stretched item in a wrapping column | its box is widened to its line and its content not laid out again, since nothing is laid out twice: text centred or right-aligned in it stays where the narrower layout put it | a page whose stretched column item shows it |
| `align-items: baseline` in a `wrap-reverse` container | lined up from each line's top, as in a container that does not reverse | a page that writes both |
| A table as a flex item | laid out by the table layout in the container's width at its place, not at the width flexing gave it | a page whose table item is the wrong width |
