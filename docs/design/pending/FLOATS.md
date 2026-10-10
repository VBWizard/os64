# FLOATS.md — floats and clear: boxes that stand aside and let the text run past

*Written 2026-10-09 by Opus, as batch 3's second part (YONDER_DIAGNOSTICS.md
§ Batch 3), for Fable's review before any code. The library is libflow and
its rules are [LAYOUT.md](../completed/LAYOUT.md)'s; positioned boxes beside
floats follow [POSITION.md](../completed/POSITION.md). The questions here are
web semantics, decided in § Decisions with Chrome as the yardstick. The two
questions that were Chris's are answered in § Rulings. Fable reviewed it on
2026-10-10, against userland 6d988565; the fourteen findings are folded in.*

## Why this, and why now

libgarb reads `float` and `clear`. libflow records them and lays every float
out where it was written: in the flow, as the block it computes to, or, for
an `<img align=left>`, inline at its baseline. LAYOUT.md booked this on the
first day, with the trigger "the first page whose layout is unreadable
without a float — image-beside-text pages of the old web will vote early".
The census has voted, and so has the old web:

- **suckless.org** is a 200px `float: left` nav column (`#nav`) beside the
  `#main` text, which keeps clear of it with `margin-left: 200px`. Without
  floats the nav is a full-width block and the page's text starts a screen
  down. Its menu bar's right half is a `float: right` span (`.right`) inside
  an `overflow: hidden` bar: a float in inline content, and a box that
  grows to hold its floats.
- **The old web's picture beside its text**: `<img align=left>` and
  `<table align=right>` turn into floats under HTML's Rendering chapter, and
  libflow already maps them that way (style.c, §15.4.3 and §15.3.8). Each one
  is laid out as a picture in the middle of a line, with the text running on
  after it rather than beside it.
- **Two-column sites** of every era float their columns, and
  `<br clear=all>` or `clear: both` ends the floating section.
- **The modern web uses floats less**, but it still writes the clearfix
  (`::after { content: ""; display: table; clear: both }`) and `display:
  flow-root`, and `@supports (float: left)` answers false today, which
  hands the page's fallback to the one browser that needs it least.

A page without its floats still reads, in document order. That is why
floats waited behind positioning and flex. But on the pages above, it is
now the largest difference between yonder and Chrome.

## What CSS asks (CSS 2.1 § 9.5, § 9.5.1, § 9.5.2, § 10.3.5, § 10.6.7, Appendix E)

- **A float is taken out of the flow** (§ 9.5): it is a block of its own,
  shifted left or right as far as it can go. Content in the same BLOCK
  FORMATTING CONTEXT flows down its other side.
  - The float's `display` is BLOCKIFIED (§ 9.7). It establishes a block
    formatting context of its own.
  - Its auto width SHRINKS TO FIT (§ 10.3.5).
  - Its auto height is its content's, its own floats included (§ 10.6.7).
  - Its margins never collapse with anything's (§ 8.3.1).
- **Where it goes: § 9.5.1's nine rules.** They come down to this. A left
  float's left margin edge is at its containing block's left content edge
  or at the right margin edge of an earlier left float (mirror everything
  for right). It may not be higher than:
  - the top of its containing block;
  - the top of any earlier float;
  - the top of the line box holding content written before it.

  It goes as high as it can, then as far left as it can. Where it does not
  fit beside the floats already there, it moves down until it fits or
  until nothing is left beside it.
- **A block box in the flow ignores floats; its LINE BOXES do not** (§ 9.5).
  A line beside a float is shortened by the float's margin box. A line too
  short for any content moves down until something fits or no floats
  remain.
- **Some boxes may not overlap a float at all**: a table, a block-level
  replaced element, and a block that establishes its own formatting context
  (`overflow` other than `visible` or `clip`, `display: flow-root`, a flex
  or grid container). Each one's border box goes beside the floats, made
  narrower when its width is auto, or below them when it does not fit.
- **`clear`** (§ 9.5.2) moves a block's top border edge below the margin
  edges of earlier floats on the named side, in the same formatting
  context. The space this opens is CLEARANCE, and clearance stops the
  block's top margin from collapsing with what is above it.
  - HTML maps `<br clear=left|right|all>` to `clear` on the `br`, and every
    engine honours it: the line after the break starts below the floats.
- **A formatting context's root grows to hold its floats** (§ 10.6.7).
  Its auto height reaches the bottom margin edge of every float inside it.
  An ordinary block's does not: a float hangs out of the bottom of a
  paragraph shorter than it, which is why the clearfix exists.
- **Paint order** (Appendix E, step 5): a stacking context paints its block
  backgrounds first, then its floats, then the inline content. Each float
  is painted whole, as if it were a stacking context, except that its
  positioned descendants belong to the parent context.

## How it enters libflow

### libgarb

Nothing new is read. `float` and `clear`, with their logical values, are
already parsed and cascaded. They leave `kApproximated` (props.c) in the
slice that lays them out, which turns `@supports (float: left)` true. The
list's own comment says they would. `display: flow-root` leaves it in the
same slice (below).

### Pass 1 — style

- **A float is blockified.** `blockify` (style.c) clears `float_side` as
  well today, which would turn a float into a block and then drop its
  float. So that line moves out of `blockify` to where the rule it
  enforces lives: "position wins over float" in `positioning()`, which
  clears `float_side` for an out-of-flow box, and "an item never floats" in
  `finish()`'s item rule. `blockify` then only changes `display`, and it
  runs for a box whose `float` is not none as well. So a floated span is a
  block, and an absolute float is an absolute box with no float.
- **Ruling 1 lands in `positioning()`**: the line where `static_only`
  forces `position: static` forces `float_side = NONE` too. `clear` is left
  alone. With no floats, `clear_y` is the top and clearance is zero.
- **A float on the root element** has no formatting context round it to
  be placed in. What Chrome does with `html { float: left }` is the probe's
  case 6. Until then libflow lays the root out as it does now.
- **`display: flow-root` gets its own value** (`FLOW_DISPLAY_FLOW_ROOT`).
  It is a block on the outside and a formatting context root on the
  inside. Today it is mapped to `block`, which is why it is in
  `kApproximated`.
- **`contributes_block` treats a float as leaving the flow**, as it does an
  absolute box. A float inside a `<span>` does not split the span, and a
  paragraph holding a floated picture is still one inline formatting
  context. Only an in-flow block-level box makes a container mix: that is
  what every engine does, and how CSS Display 3 words it.
  - One helper, `f_leaves_flow(style)` (internal.h), means "out of flow or
    floated" in pass 1 and pass 2 alike.
  - `f_out_of_flow` keeps its meaning, the positioned one, because it
    files containing blocks and floats have none of their own.

### Pass 2 — boxes

**A float is built where it was written and hung under the context it was
written in, as an absolute box is** (boxes.c's `absolute_box`).

- Among blocks, it is a block child of its container. `children()` meets it
  in order.
- Among inline content, it is a child of the inline formatting context's
  box, with a FLOAT item (`FI_FLOAT`) in the item sequence where it stood.
  That item is where `lines()` meets it. An inline element round it is not
  split, and no anonymous block is made for it.

`FBox` gains `floated` (FLOW_FLOAT_LEFT or RIGHT, NONE for every other box)
and `clearance`, and every box gains `holds_float`: some float lies in its
subtree. Paint and hit testing use it to skip subtrees with no float in
them (§ The door).

**Depth.** Among blocks, a float costs one block level, since it is laid
out inside its parent's frame like any block. Among inline content it costs
TWO, as an inline-block's content does (LAYOUT.md § Bounds): it is laid out
from inside `lines()`'s frame, so its block frame sits on a line's. A chain
of `<span><div style="float:left"><span><div style="float:left">…` is the
page this charge is for. Pass 2 charges it the extra `descend()` that
`absolute_box` and an inline-block's content already take.

### Pass 3 — layout

**THE EXCLUSION SPACE.** Each formatting context root owns one, for the
floats placed in that context. It holds, PER SIDE, a STEP FUNCTION of y: how
far the left floats reach in from the left, and how far the right floats
reach in from the right. Each entry covers a band of y and records that
side's edge over the band. A float placed on the left raises the left edge
to its right margin edge over the band its margin box spans. Three
questions are asked of it:

1. **`band(y, h, cx, cw)`**: the left and right edges of the room free
   across all of `[y, y + h)`, inside the content edges `[cx, cx + cw)`.
   This is what a line box and a box that may not overlap floats are given.
2. **`place(side, y_min, w, h, cx, cw)`**: the highest y at or below
   `y_min` where a margin box `w` wide fits between BOTH sides' edges over
   `[y, y + h)`, and the x it starts at there. The other side counts too
   (§ 9.5.1 rule 3: a left float's right outer edge may not pass the left
   outer edge of a right float beside it), so a left float dropped beside
   a left column still has to clear the right column at that y. It is the
   same walk down the band boundaries, asking `band()`'s width rather than
   one edge, and it stops at the first band that fits, or past the last
   float.
3. **`clear_y(side)`**: the bottom margin edge of the lowest float on that
   side, or on both.

**The shape of the space.** Each side is its own step function: a sorted
array of entries, each the y its band starts at and that side's edge over
the band, which runs to the next entry's y. Placing a float writes at most
two boundaries into its side, its margin box's top and bottom, and moves
the edge between them.

**Nothing is dropped, because the flow does not only descend.** `y_min`
for the next float is never above the previous float's top (§ 9.5.1 rules
5 and 6), so PLACEMENT walks down. But a negative `margin-top` moves a
later block's lines above an earlier float's top, and those lines must
still be shortened by it. So each side keeps a BOOKMARK (not the
layout's Cursor): the entry the last ask landed in. An ask at or below it
walks forward from it; an ask above it walks back. The common page never
walks back, and the cost of the pages that do is measured (§ Bounds), not
assumed.

Coordinates are document coordinates, like everything else in pass 3.
There is one exception. A RELATIVE box lays its subtree out moved by its
offset from the start (`block()`), while a float inside it belongs to the
OUTER context, which must see it where it would be unmoved (§ 9.4.3: the
offset does not move what flows round it). So the layout keeps the sum of
the relative offsets open inside the current context. It subtracts that
sum when it records into the space and when it asks the space, and adds it
back to what it is answered. The box itself is drawn where it lies.

**A relative float** (`float: left; position: relative; top: 10px`) is the
same split from the other side. `block()` applies `rel_offset` before
`block_at`, so done the obvious way the space would record the MOVED box.
Instead the float is laid out and recorded at its float position, and its
own relative offset is added after placement, by the `translate()` that
moves it there, outside the space. What flows round it uses the unmoved
box (§ 9.4.3). A float written inside a RELATIVE INLINE moves with it the
same way: the inline's offset goes on with the float's, after placement.

**The space's lifetime** is the root's layout. `block_at` pushes a space
when `bfc_root(b)` is true, lays the content out, takes the height
(below), and pops the space. `bfc_root` gains the float itself and
`flow-root`; everything it already lists stays. The spaces nest as the
roots do, and each is scratch memory, charged where a table's working
memory is charged (LAYOUT.md § Bounds).

**A float among blocks** (`children()`):

- It is laid out as a block at its shrink-to-fit width (`shrink_to_fit`,
  layout.c), against its containing block's content width. That is the
  width `fit_content` uses, held by the float's own limits.
- Its margin box is then placed by `place()`, and the subtree is
  `translate()`d there. This does not lay it out twice: its width does not
  depend on where it goes, so the size is known first and the place found
  after, the same order FLEX.md uses.
- It adds nothing to the cursor: the flow goes on as if it were not there.
- **A float with `clear`** is placed below what it clears: `y_min =
  max(y_min, clear_y(side))` before `place()` is asked. `float: left;
  clear: left` is how two-column sites stack boxes down a column and how a
  clearfix-era sidebar starts a new row. Clearance as the flow means it
  (below) is for boxes in the flow; a float has no margins that collapse,
  so there is nothing else to stop.

**A float waits for the margins above it to resolve** (Decision 1). The
float's own top is not a margin that collapses, but WHERE the flow is
while margins are still pending is not known. Take `<div
style="margin-top: 30px"><img style="float: left">text</div>`: the image
sits at the div's content top, which is 30px down once the margin
resolves. ONE rule covers every place a float can be met:

- **A float met while its container's top is unresolved waits**, whether
  it is met in `children()` or in `lines()`: the cursor's `first` is still
  `-1`, so the container's top margin can still collapse with what comes
  next. Once something in the container has resolved its top, a float met
  after it does not wait: it goes below the previous sibling's own bottom
  margin, `y + margins_sum(pm)`, and not the margin that sibling will
  collapse to with the next, which has not been met (an absolute box's
  static position, POSITION.md ruling 5; the probe's case 1d).
- **A waiting float is placed at the y where the margins resolve**: a line
  with height; the border edge of a box in the flow, a table's included;
  the end of a box whose margins do not pass its bottom; or the end of a
  formatting context root. It is placed BEFORE the thing that resolved
  them asks the space for room. Laying out a float, an inline-block's
  content, an absolute box or a flex or grid item reaches a border edge
  too, but not one in this context's flow, and resolves nothing here.
- **For a line, that is the provisional break.** The line is broken in the
  band at `cur->y + margins_sum(pm)`. Once it has height, the margins
  resolve there, the waiting floats are placed at its y, and the band is
  asked again. If it is narrower, the line is broken again. That is the
  same re-break a tall line already takes (below), so the two are one
  mechanism.
- **A line with no height resolves nothing.** A container holding ONLY a
  float (`<div style="margin-top:10px"><div style="float:left">…</div>
  </div>`) is an inline formatting context whose one line is empty; it
  takes `block_at`'s collapses-through path, and the float keeps waiting
  until the parent's collapsed margins land, which is where Chrome puts it.
- **The waiting list belongs to the SPACE**, pushed and popped with it.
  A float waiting in the outer context is not resolved by the first line
  inside a nested root's own cursor. A root's end resolves what is still
  waiting in its space.

**A float among inline content** (`lines()`, the FLOAT segment):

- **At the start of a line**, before anything with width and with no
  margins pending, it is placed with `y_min` at the line's top and
  shortens that same line. With margins pending it waits, as above.
- **In the middle of a line**, it is placed on THIS line if its margin box
  fits in what the line has left beside the content already on it. The
  line is then broken again in its narrower band; what came before the
  float still fits, by the test that let the float in. If it does not fit,
  it is placed at the top of the NEXT line (§ 9.5.1 rule 6, and Chrome:
  Decision 2).
- The FLOAT segment has no width in `break_line` and is never a break
  opportunity of its own.

**A line box is given its band** before it is broken. The band is asked for
over the line's height, which is not known until the line is broken, so:

- the line is broken in the band the STRUT's height asks for;
- if its height comes out taller and the band over that height is
  narrower than what the line used, it is broken again in the narrower
  band (Decision 3).
- So a line is broken once, plus once for each float placed on it (a
  waiting float or a mid-line one), plus once for its height. That is
  linear in the floats and the lines, which is what § Bounds needs.
- `line->x` and `line->w` are the band's. `text-align`'s extra is measured
  against the band, so a centred line centres in the room beside the
  float.
- A line whose band holds none of its first segment moves down to the
  next band boundary below and tries again. This repeats until the
  segment fits or no float remains beside it. Then it overflows, as a word
  wider than the line always has: such a line overlaps the floats beside
  it by design, and the fuzz exempts it (§ Proof).

**A box that may not overlap floats** (a table, a block-level replaced
box, a formatting context root that is in the flow) asks
`band(y_border, h, cx, cw)` where its border box would go:

- An auto width is worked by `widths()` against the band's width instead of
  the containing block's.
- A width that does not fit in the band moves the box down to the next band
  boundary, as a float moves (Decision 4).
- Its own top margin still collapses as it would have. Only the x and the
  width change.

**`clear`** on a block: after `children()` has worked out the block's
hypothetical top border edge (pending margins summed), and before it lays
the block out:

- if the edge is above `clear_y(side)`, the box's border edge goes to
  `clear_y(side)`;
- its pending margins are resolved there and nothing above collapses
  through it. That is the clearance, kept on the box for the dump.
- **`<br clear>`** is the BREAK segment carrying the `br`'s `clear`. The
  line after it starts at `max(its own top, clear_y(side))`.

**The root's height**: when `block_at` finishes a formatting context root
whose height is auto, its content height is held to at least the bottom
margin edge of every float in its space (§ 10.6.7). An ordinary block's is
not, so a float hangs out of a paragraph shorter than it, and the page's
extent and its parent's overflow rect still reach it.

**Intrinsic widths** (`intrinsic`, `ifc_intrinsic`), since a float in a
table cell decides the cell's width (Decision 5):

- **min-content**: a float's own min-content margin box counts like any
  other unbreakable thing, and it joins no word round it.
  `foo<img align=left>bar` is `max(foo, img, bar)`, not `foo + img + bar`:
  in `ifc_intrinsic`'s terms, the FLOAT segment ends the word before it and
  is part of none.
- **max-content**: floats are summed onto the line where they stand, inline
  or among blocks. A cell holding `<img align=left width=200>` and a
  paragraph is as wide as both side by side when it can be. What ENDS a run
  of floats among blocks (does a `<p>` after a float end the run, or sit
  beside it in the sum?) is the one rule here taken from Chrome rather than
  derived: the probe's case 5 asks it, and the code follows the answer.

**A positioned box's static position** (POSITION.md's row):

- Among blocks, it is where it always was: the left content edge, since
  block boxes ignore floats and § 10.3.7 places it as a block.
- Among inline content, it is the placeholder, which now stands on a
  shortened line and is right by construction.

### The door, the face

`flow_box_t` gains `floated`, the side. The dump prints `float-left`,
`float-right` and `clearance=N`. Two walks in flow.c change.

**Paint** (`flow_visit`):

- `visit_blocks` skips a floated subtree, and so does `visit_inline`.
- Between the two, a FLOAT STEP walks the LAYER's tree in order and
  paints each float it meets whole: its blocks, then its own floats by
  this same step, then its inline content. It does not descend into a
  float it has painted, nor into a subtree whose `holds_float` is clear,
  so a page with no floats pays nothing for the step.
- It is the STACKING context's walk, not the formatting context's: a float
  inside an `overflow: hidden` box paints in the same step as its parent's
  floats, in tree order. In flow.c that means `flow_visit_groups`' arms
  that paint a layer's body call the float step between `visit_blocks` and
  `visit_inline`, all of them.
- This is Appendix E's step 5 inside the walk that already does steps 4
  and 7. The stacked boxes are untouched: a positioned descendant of a
  float is still the parent context's member, which is what step 5 says.
- yonder paints whatever `flow_visit` hands it. Widgets are placed from
  box rects, and pictures from `flow_box_for`. **The face changes
  nothing.**

**Hit testing** (`flow_hit`): the same order backwards. The deepest box
holding the point wins, as now, but across three ranks: the inline
content, then the floats, then the block backgrounds.

- This is the case it fixes: a `<p>` written after a float has a border
  box running UNDER the float, and today it would win because it comes
  later in tree order.
- `hit_kids` keeps the best rank it has met along with the box, and a
  higher rank beats a lower one whatever the tree order. The float rank's
  walk skips subtrees whose `holds_float` is clear, as paint's does.
- `flow_box_covered` asks `flow_hit`, so it agrees with this for free.

## Decisions (web semantics: Chrome is the yardstick)

1. **A float waits for the margins above it** (Chrome's "unpositioned
   floats"; LayoutNG places a float once its formatting context's block
   offset is known). Placing it before the margins below it have collapsed
   would put it above its box's text wherever a float is the first thing
   inside a box with a top margin. The probe's case 1 puts it to Chrome.
2. **A float in the middle of a line goes on that line if it fits beside
   what is already there, else at the top of the next one**, as Chrome
   does. CSS 2.1 permits either, and the old web's `text <img align=right>
   more text` reads as its author saw it only this way.
3. **A line's HEIGHT is retried once**: it is broken in the strut's band,
   and again if its height reaches a narrower band. Chrome finds a line's
   room over the height the line ends up with. A second height retry would
   only matter for a line whose tallest thing grows when broken narrower,
   and that is booked. The re-breaks floats cause are separate and counted
   in § Pass 3: one per float placed on the line. The probe's case 3 puts
   a tall picture beside a staircase of floats to Chrome.
4. **A box that may not overlap floats, and has a set width wider than the
   room beside them, goes below them** rather than overlapping or
   squeezing. An auto-width one is squeezed down to its min-content width
   first, and goes below only when that does not fit (Chrome; the probe's
   case 4: `<img align=left>` beside a `<table width=100%>`).
5. **Intrinsic widths sum floats on their line** (Chrome's
   `max-content` contribution). CSS 2.1 leaves max-content to the reader.
   This is what makes a table cell holding a floated picture as wide as
   the picture beside its text. What ends a run of floats among blocks is
   probed, not remembered: the probe's case 5 is four cells, float +
   paragraph, float + table, float + paragraph + float, and float + `clear`
   + paragraph.
6. **`<br clear>` clears the next line**, as HTML maps it and every engine
   honours it, although CSS 2.1 says `clear` applies to block-level
   elements only.
7. **`float: inline-start` / `inline-end` are `left` / `right`.** libflow
   lays out left to right only, and style.c already maps them so.

## Bounds

LAYOUT.md's rule: linear in the page. A page is free to write ten thousand
floats.

- **Placing a float walks down from the previous float's top**, never up
  (§ 9.5.1 rules 5 and 6). The band boundaries it crosses telescope:
  across a whole formatting context, each boundary is crossed by at most
  the floats placed between its creation and the moment the walk passes
  it for good.
- **Asking for a line's band** reads the entries overlapping the line,
  from the side's bookmark. The flow mostly descends, so the bookmark mostly
  walks forward and the asks telescope as placement does. A negative top
  margin makes it walk BACK, and nothing bounds that by construction, so
  the bound suite measures it (below) rather than the doc claiming it.
- **A float as wide as nothing** (`width: 0`, no margins) changes no edge
  and adds no entry. Floats side by side at one y overwrite one entry
  rather than adding one each.
- The host suite lays out these pages under an alarm and asserts a
  bound on the exclusion space's work, as the table cases assert their
  layout count:
  - 10,000 floats side by side;
  - a staircase of 10,000 left floats, each a pixel narrower than the
    last;
  - 10,000 alternating left and right floats, each cleared;
  - 10,000 one-pixel floats beside a 10,000-line paragraph;
  - 10,000 floats in blocks with negative top margins, each block's lines
    asking above the float before it: what the bookmark's walk back costs.
    If it is not linear, the space becomes a tree keyed by y (log per ask,
    the shape the table code's min-tree already has), and this paragraph
    says so.
- **Memory**: each side is an array of steps, a step being a y and a
  reach, and a float writes at most two steps into its side (its margin
  box's top and bottom; steps that come out the same are joined again).
  So a space holds at most two steps per float placed in it, plus one per
  side, and a waiting list of the floats met before their margins
  resolved. All of it is scratch, charged to the arena budget as a table's
  working memory is. A page that reaches the budget stops `incomplete`,
  as any other does.
- **Depth**: a float among blocks costs one block level, laid out inside
  its parent's frame as a block is. A float among inline content costs
  two, as an inline-block's content does (§ Pass 2).

## Proof

- **Fixtures computed by hand from § 9.5.1 and § 9.5.2** (LAYOUT.md's rule:
  fixed expected geometry, never captured from the engine), one family at
  a time:
  - a left float and a right float beside a paragraph, at widths where the
    text fits beside them and where it does not;
  - three left floats that wrap to a second row;
  - three `float: left; clear: left` boxes that would otherwise fit side by
    side, stacked down a column;
  - a left float that fits beside the left floats but not between them
    and a right float, so it drops;
  - a float wider than its container;
  - a float in the middle of a line, both fitting and not;
  - `clear` on each side, with margins that clearance stops collapsing;
  - `<br clear=all>`;
  - a formatting context root holding its floats (`overflow: hidden`,
    `flow-root`, a table cell, an inline-block), and an ordinary block
    that does not;
  - a table and a `flow-root` block beside a float;
  - a float waiting for its margins, met among blocks, at the start of a
    paragraph with a top margin (`<p style="margin-top:20px"><img
    align=left>text</p>`), and as the only thing in a container with a top
    margin;
  - a float waiting in an outer context while a nested root lays out its
    own first line;
  - a negative top margin pulling a block's lines up beside an earlier
    float;
  - a relative box holding a float, and a relative float;
  - `foo<img align=left>bar`'s min-content width;
  - an `<img align=left>` and an `<img align=right>` in a quirks-mode page,
    with their 3px (§ 15.4.2);
  - the intrinsic widths of a cell holding a floated picture and text;
  - an absolute box's static position beside a float, among blocks and
    inline.
  Sized boxes, so the arithmetic in the test is the spec's and not the
  font's; text only where a line's shortening is the point, measured with
  the harness's face.
- **The probe page** (`tools/float_probe/float-probe.html`, which prints
  what it measures, FLEX.md's shape) is where each Decision is put to
  Chrome; § Decisions records the version that answered. It is the source of any case the
  spec leaves to the reader, and nothing else.
- **The invariants, in the fuzz** (LAYOUT.md § Proof, POSITION.md's soup
  gaining `float` and `clear` on every element it draws, `<img align>`,
  and `<br clear>`):
  - every box still laid out once (`FBox.laid`);
  - no line's fragments overlap any float's margin box in its formatting
    context. Two kinds of line are exempt: one wholly below every float
    beside it, which is the rule a line that found no room uses, and one
    whose first segment is wider than its band, which overflows by design
    (the first long URL beside a sidebar);
  - no two floats in one context overlap;
  - every formatting context root's border box holds its floats' margin
    boxes, when its height is auto and no limit cut it.
- **The partial-tree relations treat a float as an out-of-flow box.** A
  float met on a line hangs under its context after ALL of the context's
  lines, but is laid out while its line is built, so a tree cut in a
  later line holds the float where the whole tree holds that line: (a)
  the prefix and (c) the unfinished ancestors are asserted over the
  in-flow tree, without floats, as they are without absolute boxes. A
  float placed whole is held to (b): where the whole layout puts it. That
  holds because a float whose place the cut could change is NOT PLACED:
  one still waiting when its root's content was cut short (pass 2) or
  stopped (pass 3), since that end is not the whole layout's and the
  margins after the cut would have moved it; one met on a line that did
  not join whole, as an absolute box's placeholder loses its place there;
  and one that could not be set waiting for want of memory. A box cut
  short resolves no margins at its own edges either. The relation sweep
  has a page of floats (`float_sweep_cases`).
- **The corpus dumps move**, everywhere a page writes `align=left` or
  `float`, and the diff is read, not just rewritten.
  floodgap's and the old-web pages' pictures go beside their text.
- **In the guest**: suckless.org beside Chrome (the nav column and the
  menu's right half), and an old-web image-beside-text page (theoldnet's
  archive pages have them). The P5 run is Chris's.

## Slices

| Slice | What | Proof |
|---|---|---|
| FL1 | Everything above. Floats among blocks and among inline content; the exclusion space; line shortening; boxes that may not overlap floats; `clear` and `<br clear>`; roots holding their floats; `flow-root`; the paint and hit order; the intrinsic widths; `kApproximated` loses `float`, `clear` and `flow-root` | § Proof, whole |

One slice, because no smaller part reads right. Floats without line
shortening are drawn on top of the text. Line shortening without `clear` and
roots holding their floats makes every clearfix page overlap its footer.
And the old web's pictures are inline floats, so the inline half cannot
wait for the block half.

One PR, but not one lump: it lands as REVIEWABLE COMMITS, each with its
own fixtures, so the reviewer can read it a commit at a time:

1. the space and floats among blocks;
2. floats among inline content;
3. `clear` and roots holding their floats;
4. paint and hit order;
5. intrinsic widths, and `kApproximated`.

## Rulings

Chris ruled both on 2026-10-09.

1. **Positioning off (`flow_env_t.static_only`, yonder's "Positioning
   off") lays floats out where they stand, too.** The mode exists for the
   page as it reads in document order when its designed layout gets in the
   way. A float is the next most common thing to take content out of that
   order, and a person who turned positioning off because the page was
   unreadable should not have to find a second switch. The status line's
   "Positioning off" sentence says so. (Chris: the mode itself may not
   last; this is the rule while it does.)
2. **Fable reviews this design before any code; Quinn or Codex reviews the
   code**, whoever is free. The slice rewrites how `lines()` is given its
   room, and touches margin collapsing, intrinsic sizing, paint order and
   hit order. Every page in the corpus moves.

## Booked, with their triggers

| Debt | Why it waits | Trigger |
|---|---|---|
| A line's height retried more than once against a staircase of floats | Decision 3: the second band is the narrower, so nothing overlaps a float; another height retry would only ever widen a line again | a page whose text beside stepped floats breaks visibly shorter than Chrome's |
| `shape-outside`, `shape-margin` | the exclusion is the margin box only | a page whose text should wrap round a circle |
| Floats in a right-to-left context, and `inline-start`/`inline-end` by direction | libflow lays out left to right | the bidi line layout |
| Floats beside a multi-column or a ruby | neither is laid out | either's slice |
| A float inside a table row, row group or column (not inside a cell) | the table's anonymous objects wrap it in a cell first (§ 17.2.1), which is what Chrome does; nothing to do unless a page shows otherwise | a page that shows otherwise |
