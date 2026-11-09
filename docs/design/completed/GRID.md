# GRID.md — grid layout: tracks, lines and the items placed in them

*Written 2026-09-30 by Opus, the same night as FLEX.md, as the last part
of GARB.md's pile 2: Chris asked for flexbox's two slices and grid, built
stacked. The library is libflow and its rules are
[LAYOUT.md](LAYOUT.md)'s; the properties come from libgarb
([GARB.md](GARB.md)); it reuses what
[FLEX.md](FLEX.md) built — blockified items, content sizes kept per box,
the once-only layout, the alignment vocabulary. Decisions are web
semantics, made with Chrome as the yardstick (§ Decisions).*

## Why this, and why now

danlegt.com lays its home page out as a grid: `display: grid;
grid-template-columns: repeat(6, 1fr); grid-template-rows: repeat(12,
150px); gap: 10px`, each panel placed with `grid-column: span 4;
grid-row: span 5`. Today the grid is a block and the panels stack, full
width, in document order: "Tune In" above "Welcome!" instead of beside it.
Its sheets also write the grid of the modern web's small parts — icon
tiles (`repeat(auto-fill, minmax(104px, 1fr))`), a label beside its value
(`92px 1fr`), a sidebar beside its content (`minmax(140px, 42%) 1fr`). A
page's skeleton is often a grid too, named with `grid-template-areas`.

## What CSS asks (CSS Grid Layout 2, the parts every page writes)

A GRID CONTAINER (`display: grid`, `inline-grid`) divides its content box
into COLUMNS and ROWS — its TRACKS — separated by numbered GRID LINES, and
places each in-flow child, a GRID ITEM, into an area of whole tracks.

| Property | On | What it says |
|---|---|---|
| `grid-template-columns`, `grid-template-rows` | container | the explicit tracks: a list of sizes — a length, a percentage, `auto`, `min-content`, `max-content`, `<n>fr`, `minmax(min, max)`, `fit-content(len)` — with `repeat(n, …)` and `repeat(auto-fill \| auto-fit, …)` |
| `grid-template-areas` | container | named areas as strings, one per row, a name per cell, `.` for none |
| `grid-auto-columns`, `grid-auto-rows` | container | the size of each track the placement adds past the explicit ones |
| `grid-auto-flow` | container | `row` (initial) or `column`, and `dense` |
| `grid-column-start`, `-end`, `grid-row-start`, `-end` | item | a line number (negative counts from the end), `span n`, an area's name, or `auto` |
| `grid-column`, `grid-row`, `grid-area` | item | shorthands: `start / end`; `grid-area` a name or four lines |
| `justify-items`, `justify-self` | container, item | the item's place across its area's width: `stretch` (for `normal`), `start`, `end`, `center` |
| `align-items`, `align-self` | container, item | the same down its area's height (FLEX.md already reads them) |
| `justify-content`, `align-content` | container | where the tracks go when they do not fill the container |
| `place-items`, `place-self`, `place-content` | | shorthands: align then justify |
| `gap`, `row-gap`, `column-gap` | container | between tracks (FLEX.md already reads them) |

The layout is § 11–§ 12's: the items are PLACED (§ 8: definite
positions first, then the auto-placement cursor, adding implicit tracks
as it needs them), the column tracks are SIZED from the items' widths,
each item is laid out in its columns, the row tracks are sized from the
items' heights, and each item is aligned in its area.

## How it enters libflow

The rule FLEX.md set holds: **every item is laid out once.** Column sizes
need only widths, which `intrinsic` knows without laying anything out
(and keeps the content's own, `content_min`/`content_max`); rows need
heights, which only laying out gives. So: place, size the columns, lay
each item out once at its columns' width, size the rows from the heights,
then move each item into its rows. An item stretched down its rows has
its box heightened, and nothing inside moves — as a flex row's stretch,
and with its exception: an item that is a flex or grid container places
its items again against the height it now has (FLEX.md's `refit`; a
grid's refit sizes its rows again). A grid's content height, what a flex
column reads as its content (`FBox.content_h`), is its rows as its items
alone size them, whatever height it was given.

### libgarb

Every property in the table, with the shorthands `grid-column`,
`grid-row`, `grid-area`, `place-items`, `place-self`, `place-content`.
A track list is read into the value's `items`: each track a length or
percentage, a keyword, an `fr` (a new unit, `GARB_U_FR`, which only a
track list accepts), or a function held as a keyword with its arguments in
`items` — `minmax`, `fit-content`, `repeat` (its count a number or the
keyword `auto-fill`/`auto-fit`, then the tracks it repeats). Line names in
brackets are read and dropped: placement by an explicit line NAME is
booked (decision 3).
`grid-template-areas` is its strings, checked here: every row the same
number of cells and every name a rectangle, or the declaration is
invalid (§ 7.3). `display: grid`/`inline-grid` leave the approximations,
so `@supports (display: grid)` says yes; `grid-template` and `grid`, the
two all-in-one shorthands, are not read (booked).

### Pass 1 — style

`flow_display_t` gains `GRID` and `INLINE_GRID`, and `flow_style_t` the
properties: the track lists as arrays in the style arena with each
`repeat(n)` written out (`flow_tracks_t`: each track a minimum and a
maximum sizing function, and where the list's auto-repeat block is), the
areas as rectangles, the four lines. A grid container's children are
blockified — the bit FLEX.md added, renamed for what it now means
(`FStyled.lays_out_items`). `justify-items: legacy` is read as `normal`,
which, like `align-items: normal`, places a non-replaced item as
`stretch` and a replaced one as `start` (Box Alignment 3 § 6.1).

### Pass 2 — boxes

As a flex container's: never an inline formatting context, text runs as
anonymous items, `FBox.grid` beside `FBox.flex`, `inline-grid` an atom.

### Pass 3 — layout

`block_at` hands a grid container to `grid()`.

1. **The explicit grid**: each template's tracks, `repeat(n)` written out;
   an auto-repeat block repeated as often as it fits the container's
   width (columns), or its given height or else its max-height (rows) —
   each track at its fixed size, or its minimum when only that is fixed,
   counted as 1px when it is less (a track of nothing would repeat
   without end; its size does not change), at least once (§ 7.2.3.2) —
   and `grid-template-areas` adding rows and columns up to its own size.
2. **Placement** (§ 8.5), items in `order` order: those with a definite
   position on both axes; then, with `grid-auto-flow: row`, those with a
   definite row, each at the first free column in it; then the rest at an
   auto-placement cursor that moves along the rows (`dense` goes back to
   the start for each item). An area's name places both edges, and
   `name-start`/`name-end` either one; a line number counts from the
   start, a negative one from the explicit grid's end; `span n` reaches n
   tracks. The grid grows implicit tracks, sized by
   `grid-auto-rows`/`-columns` (cycling through a list), as it needs them,
   up to 1000 on either axis, a bound of libflow's own (decision 4); an
   item placed past it is held to the last track. The items fixed to a
   row may add columns; a line before the grid's start is its start
   (booked). An axis with no explicit track and no item has no tracks at
   all: `grid-auto-rows` makes no row of its own.
3. **The columns** (§ 12, the parts pages write): a fixed track is its
   size; a percentage is of the content width; an intrinsic minimum
   (`auto`, `min-content`) starts at the largest min-content width among
   the items that span it alone — an `auto` minimum is the automatic
   minimum, the item's content's — and an intrinsic maximum (`auto`,
   `max-content`) at the largest max-content; an item spanning several
   intrinsic tracks adds what they lack, shared equally (decision 2); an
   item crossing an `fr` track grows only the `fr` tracks' minimums, by
   their factors, after all the others. Then the free space grows every
   track to its maximum, equally; then `fr` tracks take the rest by their
   factors, a track already bigger than its share keeping its size and
   the rest sharing again, and a sum of factors below 1 taking only that
   part (§ 12.7); then `auto` tracks stretch into anything left, unless
   `justify-content` says otherwise. `fit-content(len)` is
   `minmax(auto, max-content)` held to `len`; a percentage of a size not
   known is `auto`. The sizes add up by FLEX.md's cumulative rounding.
4. **Each item laid out once**, in tree order, at its columns' width
   less its margins when it stretches (`justify-self`), or at fit-content
   and moved across when it does not.
5. **The rows**, the same way from the items' heights: a fixed row its
   size; an intrinsic one the tallest single-row item in it; a spanning
   item's extra shared among its intrinsic rows; `fr` rows sharing a
   height the container was given, or, with none, sized by § 12.7.1's
   rule for a size not known — one fr is the most any `fr` row or item in
   one asks of it, so `1fr 2fr` over items 30 and 20 tall is 30 and 60.
   `auto` rows stretch into a given height, or a min-height. An item's
   minimum contribution down its rows is as across (§ 6.6): a height the
   page gave, whatever its `min-height`; else its automatic minimum — a
   `min-height` the page gave, only its frame when it scrolls, or its
   content's height. A
   percentage row gap counts as nothing while a height not given is found,
   and is of that height once it is (Box Alignment 3 § 8.3).
6. **Alignment**: each item moved down into its rows by `align-self`
   (`stretch` heightens its box; `baseline` is read as `start`, booked),
   auto margins taking the room first, and across into its columns by
   `justify-self`, at the width it was laid out at — a refit plans the
   grid again, and may put it in others; and the tracks placed in the container by `justify-content`
   and `align-content` where they leave room — overflowing tracks centred
   or ended all the same.
7. **The container's height**: its `height`, or its rows and gaps, held to
   its limits.

**A grid container's intrinsic widths**: its columns sized as step 3 would
size them for the smallest and the largest room — each intrinsic track at
its items' min-content, or max-content, `fr` tracks as their minimum or
their items' max-content — and the gaps.

**A grid container's baseline** (§ 10.8, `grid_baseline`): its first
item's in grid order — the lowest row, then the leftmost column, then
`order` — made from its border box's bottom edge when it holds no text, as
a flex container's is.

**Absolute children** are placed at the content box's corner (the grid
area as their containing block is booked).

**All or nothing** as FLEX.md: items laid out in tree order, placed once
all of them are, and withdrawn when a layout stops in between
(`items_done`); an item's absolute boxes are laid out after its rows have
sized and moved it — after the OUTERMOST container has, when grids and
flex containers nest — and a relative item keeps its offset when they move
it, its percentages being of its grid area, its containing block, both
ways (`sized_later`, `rel_offset`).

### The door, the face

Nothing new. A grid item is a box; the box dump marks a grid container
` grid`, and the style dump prints the grid properties a style sets.

## Decisions (web semantics: Chrome is the yardstick)

1. **Every item laid out once** (above); what it gives up is booked: a
   percentage height inside a stretched item, and a row track whose size
   would depend on an item's height at a width a later step changes.
2. **Spanning items share equally** among the intrinsic tracks they span,
   where § 12.5 distributes by growth limits and affected sizes in
   stages. Equal is the simple rule; `tools/grid_probe/` holds the
   spanning cases pages write, and Chrome agrees on every one of them.
   The staged rules are booked.
3. **Line names are dropped**, areas are kept: `grid-area: header` is how
   most pages that name anything name it, and each area's edges are its
   implicit lines (`header-start`, `header-end`). A name that matches no
   area is § 8.3.1's line nobody named: every implicit line takes the
   name, so it is the first line past the explicit grid — which is where
   Chrome puts it too.
4. **At most 1000 tracks on either axis**: the occupancy of a grid is
   rows × columns, and a page asking for `grid-row: 1 / 100000` would
   otherwise cost what it names. Chrome has a bound of its own; a page
   meeting either one is not one anybody lays out by hand.

## Slices

| Slice | What | Proof |
|---|---|---|
| G1 | everything above | hand-laid grids worked from § 8 and § 11–§ 12 (`tools/test_libflow_grid.inc`: danlegt's six-column home at 400 and 1200; `repeat(auto-fill, minmax(104px, 1fr))` at three widths and auto-fit; `92px 1fr`; `minmax(140px, 42%) 1fr` both ways; an auto track over text; 1fr held open by its content; spans, negative lines, areas and their edges, a name nobody defines, auto-placement with and without `dense`, `grid-auto-flow: column`, a row-fixed item, `order`, a line past the bound; auto rows from content; `fr` rows in a given height; `justify-self`/`align-self`; `justify-content` and `align-content` on the tracks; an inline grid's width); the grid ingredients in the fuzz soup, every share clean; `tools/grid_probe/`, 56 pages against Chrome, all agreeing; a guest page: danlegt's home against Chrome |

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| Placement by an explicit line name (`[main-start]`), and `span <name>` | areas cover what pages name; names in brackets are read and dropped, and a span of a name is a span of one | a page that places by a bracketed name |
| Implicit tracks before the explicit grid | a line counted back past the start, or a span reaching back from line 1, is held to the start | a page whose negative lines reach past its grid |
| `grid-template` and `grid` shorthands | rarely written | a page that writes them |
| § 12.5's staged distribution for spanning items | decision 2 | a page whose spanning item sizes its tracks differently from Chrome |
| Subgrid, masonry | Grid 2 and 3's newest | a page that writes them |
| The grid area as an absolute child's containing block | the content box's corner first | an absolute badge in a grid cell at the wrong place |
| A percentage height inside a stretched item | nothing is laid out twice | GARB.md's percentage-height row |
| Baseline alignment in grid (`align-self: baseline`) | read as `start` | a page whose grid row lines up text |
| A grid's `min-`/`max-width` or a definite min or max height in § 12's indefinite cases | an auto-repeat counts once, and `fr` rows are sized as for a height not known, unless the page gave a height (a max-height is read for the auto-repeat) | a page whose grid reads them to size its tracks |
| `order` in paint order | as FLEX.md's row | FLEX.md's trigger |
